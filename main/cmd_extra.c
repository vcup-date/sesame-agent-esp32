#include "sesame.h"
#include "cfg.h"
#include "led.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "driver/gpio.h"
#include "driver/temperature_sensor.h"
#include "driver/uart.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "extra";

typedef struct { char *buf; size_t len, cap; bool full; FILE *file; } body_t;

static esp_err_t on_http(esp_http_client_event_t *evt)
{
    body_t *b = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !b) {
        return ESP_OK;
    }

    if (b->file) {
        b->len += fwrite(evt->data, 1, evt->data_len, b->file);
        return ESP_OK;
    }
    if (b->full) {
        return ESP_OK;
    }
    size_t room = b->cap - b->len - 1;
    size_t take = evt->data_len < (int)room ? (size_t)evt->data_len : room;
    memcpy(b->buf + b->len, evt->data, take);
    b->len += take;
    b->buf[b->len] = '\0';
    if (take < (size_t)evt->data_len) {
        b->full = true;
    }
    return ESP_OK;
}

static int cmd_http(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) {
        sesame_write(out,
            "usage: http <url> [post-body]\n"
            "       http <url> -o <path>      save the body to a file\n"
            "https works. Without -o the reply is capped at 8KB; with it there\n"
            "is no limit, and `text`, `grep`, `head` can read the file after.\n");
        return 1;
    }

    const char *save = NULL;
    const char *post = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            save = argv[++i];
        } else if (!post) {
            post = argv[i];
        }
    }

    char path[288] = { 0 };
    body_t b = { .cap = 8192 };
    if (save) {
        if (save[0] == '/') snprintf(path, sizeof(path), "%s", save);
        else                snprintf(path, sizeof(path), "%s/%s", SESAME_MOUNT, save);
        b.file = fopen(path, "wb");
        if (!b.file) {
            sesame_printf(out, "http: cannot write %s\n", path);
            return 1;
        }
    }
    if (!b.file) {
        b.buf = heap_caps_malloc(b.cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!b.buf) {
            b.buf = malloc(b.cap);
        }
        if (!b.buf) {
            return 1;
        }
        b.buf[0] = '\0';
    }

    esp_http_client_config_t cfg = {
        .url               = argv[1],
        .method            = post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .event_handler     = on_http,
        .user_data         = &b,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 20000,
        .buffer_size       = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        free(b.buf);
        return 1;
    }
    if (post) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, post, strlen(post));
    }

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (b.file) {
        fclose(b.file);
        b.file = NULL;
    }

    if (err != ESP_OK) {

        if (err == ESP_ERR_NOT_SUPPORTED) {
            sesame_write(out, "http: the server demanded authentication this "
                              "client cannot do (usually a Bearer token). Pass "
                              "the key in the URL or use a public endpoint.\n");
        } else {
            sesame_printf(out, "http: %s\n", esp_err_to_name(err));
        }
        free(b.buf);
        return 1;
    }
    if (save) {
        sesame_printf(out, "HTTP %d, %u bytes saved to %s\n"
                           "(read it with: text %s | head %s | grep <word> %s)\n",
                      status, (unsigned)b.len, path, path, path, path);
        return 0;
    }
    sesame_printf(out, "HTTP %d, %u bytes%s\n\n", status, (unsigned)b.len,
                  b.full ? " (truncated — use -o <path> to save it all)" : "");
    sesame_write(out, b.buf);
    sesame_write(out, "\n");
    free(b.buf);
    return 0;
}

static int cmd_time(int argc, char **argv, sesame_out_t *out)
{
    if (argc > 1 && strcmp(argv[1], "sync") == 0) {
        esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_deinit();
        if (esp_netif_sntp_init(&c) != ESP_OK) {
            sesame_write(out, "time: could not start SNTP\n");
            return 1;
        }
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) != ESP_OK) {
            sesame_write(out, "time: no reply from the time server\n");
            return 1;
        }
        sesame_write(out, "clock set from pool.ntp.org\n");
    }

    setenv("TZ", cfg_get("tz", "UTC0"), 1);
    tzset();

    time_t now = 0;
    time(&now);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm);

    sesame_printf(out, "%s\n", buf);
    if (now < 1700000000) {
        sesame_write(out, "(not set — run 'time sync', needs a network)\n");
    }
    sesame_printf(out, "tz  %s   (cfg set tz CST-8 for China, GMT0BST for UK)\n",
                  cfg_get("tz", "UTC0"));
    return 0;
}

static int cmd_temp(int argc, char **argv, sesame_out_t *out)
{
    (void)argc; (void)argv;
    temperature_sensor_handle_t h = NULL;
    temperature_sensor_config_t c = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&c, &h) != ESP_OK) {
        sesame_write(out, "temp: sensor unavailable\n");
        return 1;
    }
    float t = 0;
    int rc = 1;
    if (temperature_sensor_enable(h) == ESP_OK &&
        temperature_sensor_get_celsius(h, &t) == ESP_OK) {

        sesame_printf(out, "%.1f C (die temperature, not ambient)\n", t);
        rc = 0;
    } else {
        sesame_write(out, "temp: read failed\n");
    }
    temperature_sensor_disable(h);
    temperature_sensor_uninstall(h);
    return rc;
}

static led_strip_handle_t s_strip;
static int s_strip_pin = -1;

static int cmd_rgb(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) {
        sesame_printf(out,
            "usage: rgb <r> <g> <b> [pin]   values 0-255\n"
            "       rgb off\n"
            "A WS2812 needs RMT, not a plain GPIO — this is why 'gpio set' does\n"
            "nothing to the onboard colour LED. Current pin: %d\n",
            s_strip_pin >= 0 ? s_strip_pin : atoi(cfg_get("rgb.pin", "48")));
        return 1;
    }

    bool off = strcmp(argv[1], "off") == 0;
    int r = off ? 0 : atoi(argv[1]);
    int g = (!off && argc > 2) ? atoi(argv[2]) : 0;
    int b = (!off && argc > 3) ? atoi(argv[3]) : 0;
    int pin = (argc > 4) ? atoi(argv[4]) : atoi(cfg_get("rgb.pin", "48"));

    if (led_rgb(r, g, b, pin)) {
        sesame_printf(out, "rgb %d,%d,%d on gpio %d\n", r, g, b, pin);
        if (!off) {
            sesame_write(out, "(status animation paused; 'led idle' resumes it)\n");
        }
        return 0;
    }

    if (s_strip && pin != s_strip_pin) {
        led_strip_del(s_strip);
        s_strip = NULL;
    }
    if (!s_strip) {
        led_strip_config_t sc = {
            .strip_gpio_num = pin,
            .max_leds = 1,
            .led_model = LED_MODEL_WS2812,
            .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        };
        led_strip_rmt_config_t rc = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 10 * 1000 * 1000,
        };
        if (led_strip_new_rmt_device(&sc, &rc, &s_strip) != ESP_OK) {
            sesame_printf(out, "rgb: cannot drive GPIO %d over RMT\n", pin);
            return 1;
        }
        s_strip_pin = pin;
    }

    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
    sesame_printf(out, "rgb %d,%d,%d on gpio %d\n", r, g, b, pin);
    return 0;
}

static int cmd_uart(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 4) {
        sesame_write(out,
            "usage: uart <tx> <rx> <baud> [text to send]\n"
            "sends the text if given, then prints whatever arrives for 1s\n");
        return 1;
    }
    int tx = atoi(argv[1]), rx = atoi(argv[2]), baud = atoi(argv[3]);
    if (baud < 300 || baud > 921600) {
        sesame_write(out, "uart: baud out of range\n");
        return 1;
    }

    const uart_port_t port = UART_NUM_1;
    uart_config_t c = {
        .baud_rate = baud, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_delete(port);
    if (uart_driver_install(port, 2048, 0, 0, NULL, 0) != ESP_OK ||
        uart_param_config(port, &c) != ESP_OK ||
        uart_set_pin(port, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        sesame_write(out, "uart: could not configure those pins\n");
        uart_driver_delete(port);
        return 1;
    }

    if (argc > 4) {
        uart_write_bytes(port, argv[4], strlen(argv[4]));
        uart_write_bytes(port, "\r\n", 2);
    }

    uint8_t buf[513];
    int n = uart_read_bytes(port, buf, sizeof(buf) - 1, pdMS_TO_TICKS(1000));
    uart_driver_delete(port);

    if (n <= 0) {
        sesame_printf(out, "uart %d/%d @%d: nothing received\n", tx, rx, baud);
        return 0;
    }
    buf[n] = '\0';
    for (int i = 0; i < n; i++) {
        if (buf[i] != '\n' && buf[i] != '\r' && (buf[i] < 32 || buf[i] > 126)) {
            buf[i] = '.';
        }
    }
    sesame_printf(out, "%d bytes:\n%s\n", n, (char *)buf);
    return 0;
}

static int cmd_sleep(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) {
        sesame_write(out,
            "usage: sleep <seconds> [wake_gpio]\n"
            "Deep sleep: the device powers down and REBOOTS on wake, so the\n"
            "network, this session and the conversation are all lost.\n");
        return 1;
    }
    int secs = atoi(argv[1]);
    if (secs < 1 || secs > 86400) {
        sesame_write(out, "sleep: 1..86400 seconds\n");
        return 1;
    }
    esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);

    if (argc > 2) {
        int pin = atoi(argv[2]);

        if (pin < 0 || pin > 21) {
            sesame_printf(out, "sleep: gpio %d cannot wake the chip (use 0..21)\n", pin);
            return 1;
        }
        esp_sleep_enable_ext1_wakeup(1ULL << pin, ESP_EXT1_WAKEUP_ANY_HIGH);
    }

    sesame_printf(out, "sleeping %d s — the device will reboot on wake\n", secs);
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_deep_sleep_start();
    return 0;
}

static int cmd_rand(int argc, char **argv, sesame_out_t *out)
{
    int n = argc > 1 ? atoi(argv[1]) : 1;
    if (n < 1)  n = 1;
    if (n > 32) n = 32;
    for (int i = 0; i < n; i++) {
        sesame_printf(out, "%u%s", (unsigned)esp_random(), i + 1 < n ? " " : "\n");
    }
    return 0;
}

static const sesame_cmd_t CMDS[] = {
    { "http",  "http <url> [post-body]",        "fetch a URL (https works)",        cmd_http },
    { "time",  "time [sync]",                   "clock; 'sync' sets it from NTP",   cmd_time },
    { "temp",  "temp",                          "chip die temperature",             cmd_temp },
    { "rgb",   "rgb <r> <g> <b> [pin] | off",   "onboard WS2812 colour LED",        cmd_rgb },
    { "uart",  "uart <tx> <rx> <baud> [text]",  "talk to a serial device",          cmd_uart },
    { "sleep", "sleep <seconds> [wake_gpio]",   "deep sleep; reboots on wake",      cmd_sleep },
    { "rand",  "rand [count]",                  "hardware random numbers",          cmd_rand },
};

void cmd_extra_register(void)
{
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
    ESP_LOGD(TAG, "extra commands registered");
}
