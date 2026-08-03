#include "led.h"
#include "cfg.h"
#include "sesame.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "led";

#define LED_TIMER    LEDC_TIMER_2
#define LED_CHANNEL  LEDC_CHANNEL_2

#define TICK_MS      20
#define BREATH_MS    4200
#define BEAT_MS      1100

static led_mode_t s_mode = LED_OFF;
static int  s_pin = -1;
static bool s_inv;
static bool s_ready;
static bool s_addressable;
static int  s_transient_ms;
static led_strip_handle_t s_strip;
// The animation task and any command that sets a colour both drive the same
// RMT channel. Without this they can overlap mid-transmission and the driver
// rejects the second one with "channel not in init state", which showed up as
// dropped colours in the middle of a sequence.
static SemaphoreHandle_t s_strip_lock;
static uint8_t s_manual[3];

static bool strip_open(int pin)
{
    if (s_strip && pin == s_pin) {
        return true;
    }
    if (s_strip) {
        led_strip_del(s_strip);
        s_strip = NULL;
    }
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
        s_strip = NULL;
        return false;
    }
    return true;
}

static void write_level(int duty, int r, int g, int b)
{
    if (!s_ready) {
        return;
    }
    if (duty < 0)   duty = 0;
    if (duty > 255) duty = 255;

    if (s_addressable) {
        if (!s_strip) {
            return;
        }
        if (s_strip_lock && xSemaphoreTake(s_strip_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
            return;   // someone else is mid-refresh; a dropped frame is fine
        }
        led_strip_set_pixel(s_strip, 0, r * duty / 255, g * duty / 255, b * duty / 255);
        led_strip_refresh(s_strip);
        if (s_strip_lock) {
            xSemaphoreGive(s_strip_lock);
        }
        return;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_CHANNEL, s_inv ? 255 - duty : duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_CHANNEL);
}

static void write_duty(int duty) { write_level(duty, 255, 255, 255); }

bool led_rgb(int r, int g, int b, int pin)
{
    if (!s_addressable) {
        return false;
    }
    if (pin >= 0 && pin != s_pin) {
        if (!strip_open(pin)) {
            return false;
        }
        s_pin = pin;
    }
    if (!s_strip && !strip_open(s_pin)) {
        return false;
    }
    s_ready = true;
    s_manual[0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
    s_manual[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
    s_manual[2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
    s_transient_ms = 0;
    s_mode = LED_MANUAL;

    write_level(255, s_manual[0], s_manual[1], s_manual[2]);
    return true;
}

static int breath(int phase_ms, int period_ms, int peak)
{
    int half = period_ms / 2;
    int up = phase_ms < half ? phase_ms : period_ms - phase_ms;
    int lin = up * 1000 / half;
    return (int)((int64_t)lin * lin / 1000 * peak / 1000);
}

static void led_task(void *arg)
{
    (void)arg;
    int t = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        t += TICK_MS;

        led_mode_t m = s_mode;
        if (s_transient_ms > 0) {
            s_transient_ms -= TICK_MS;
            if (s_transient_ms <= 0) {
                s_mode = LED_IDLE;
                t = 0;
            }
        }

        switch (m) {
        case LED_OFF:
            write_duty(0);
            break;

        case LED_IDLE:

            write_level(breath(t % BREATH_MS, BREATH_MS, 26), 40, 90, 255);
            break;

        case LED_THINKING: {

            int p = t % BEAT_MS;
            int d = 0;
            if      (p < 130)              d = breath(p, 260, 210);
            else if (p >= 190 && p < 320)  d = breath(p - 190, 260, 150);
            write_level(d, 255, 150, 20);
            break;
        }

        case LED_OK:
            write_level(200, 0, 255, 60);
            break;

        case LED_ERROR: {
            int p = t % 200;
            write_level(p < 100 ? 255 : 0, 255, 0, 0);
            break;
        }

        case LED_MANUAL:
            // Already written by led_rgb, and it does not change. Re-sending it
            // every tick only competes with whoever sets the next colour.
            break;
        }
    }
}

void led_init(void)
{

    s_pin = atoi(cfg_get("led.pin", "48"));
    s_inv = atoi(cfg_get("led.inv", "0")) != 0;
    s_addressable = strcmp(cfg_get("led.type", "ws2812"), "pwm") != 0;

    if (s_pin < 0) {
        ESP_LOGI(TAG, "status LED disabled (led.pin = -1)");
        return;
    }

    if (!s_strip_lock) {
        s_strip_lock = xSemaphoreCreateMutex();
    }

    if (s_addressable) {
        if (!strip_open(s_pin)) {
            ESP_LOGW(TAG, "cannot drive GPIO %d as a WS2812 over RMT", s_pin);
            return;
        }
        s_ready = true;
        s_mode  = LED_IDLE;
        xTaskCreate(led_task, "led", 3072, NULL, 2, NULL);
        ESP_LOGI(TAG, "status LED: WS2812 on GPIO %d", s_pin);
        return;
    }

    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LED_TIMER,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK) {
        ESP_LOGW(TAG, "no LEDC timer for the status LED");
        return;
    }

    ledc_channel_config_t chan = {
        .gpio_num   = s_pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LED_CHANNEL,
        .timer_sel  = LED_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    if (ledc_channel_config(&chan) != ESP_OK) {
        ESP_LOGW(TAG, "cannot drive GPIO %d as the status LED", s_pin);
        return;
    }

    s_ready = true;
    s_mode  = LED_IDLE;
    xTaskCreate(led_task, "led", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "status LED on GPIO %d%s (pwm)", s_pin, s_inv ? " (active low)" : "");
}

void led_set(led_mode_t mode)
{
    if (mode == LED_OK) {
        s_transient_ms = 400;
    } else if (mode == LED_ERROR) {
        s_transient_ms = 700;
    } else {
        s_transient_ms = 0;
    }
    s_mode = mode;
}

static const int CANDIDATES[] = { 21, 48, 38, 47, 39, 40, 41, 42, 2, 1, 14 };

static int cmd_led(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        static const char *NAMES[] = { "off", "idle", "thinking", "ok", "error", "manual" };
        sesame_printf(out, "pin    %d%s\n", s_pin, s_inv ? " (active low)" : "");
        sesame_printf(out, "type   %s\n", s_addressable ? "ws2812 (addressable, over RMT)"
                                                        : "plain LED (pwm)");
        sesame_printf(out, "driver %s\n", s_ready ? "ready" : "not configured");
        sesame_printf(out, "mode   %s\n", NAMES[s_mode]);
        if (s_addressable) {
            sesame_write(out, "\nthis board's other two LEDs cannot be a status light:\n"
                              "  red   wired across 3V3, no GPIO — always on\n"
                              "  blue  on GPIO43 = UART0 TX; it blinks with console\n"
                              "        traffic, and taking it would cost the console\n");
        }
        return 0;
    }

    if (strcmp(argv[1], "find") == 0) {
        unsigned n = sizeof(CANDIDATES) / sizeof(CANDIDATES[0]);

        sesame_printf(out, "%u pins, ~2s each. Count the steps and note WHICH ONE "
                           "lights up (1 = first).\n", n);
        for (unsigned i = 0; i < n; i++) {
            int pin = CANDIDATES[i];
            sesame_printf(out, "  [%u/%u] gpio %d\n", i + 1, n, pin);
            gpio_reset_pin(pin);
            gpio_set_direction(pin, GPIO_MODE_OUTPUT);
            for (int k = 0; k < 8; k++) {
                gpio_set_level(pin, k % 2);
                vTaskDelay(pdMS_TO_TICKS(230));
            }
            gpio_set_level(pin, 0);
            vTaskDelay(pdMS_TO_TICKS(700));
        }
        sesame_write(out, "\nnothing? the LED may be addressable (WS2812) rather "
                          "than plain: try `led rgbfind`.\n"
                          "otherwise: cfg set led.pin <n>, then reboot\n");
        return 0;
    }

    if (strcmp(argv[1], "blink") == 0 && argc >= 3) {
        int pin = atoi(argv[2]);
        int secs = argc > 3 ? atoi(argv[3]) : 5;
        if (secs < 1)  secs = 1;
        if (secs > 20) secs = 20;
        sesame_printf(out, "blinking gpio %d for %d s\n", pin, secs);
        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        for (int k = 0; k < secs * 4; k++) {
            gpio_set_level(pin, k % 2);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        gpio_set_level(pin, 0);
        sesame_write(out, "done\n");
        return 0;
    }

    if (strcmp(argv[1], "rgbfind") == 0) {
        static const int RGB_CANDIDATES[] = { 48, 38, 21, 47, 39, 40, 41, 42 };
        unsigned n = sizeof(RGB_CANDIDATES) / sizeof(RGB_CANDIDATES[0]);
        sesame_printf(out, "%u pins, ~2s each, driving BLUE. Note which step "
                           "lights up (1 = first).\n", n);
        for (unsigned i = 0; i < n; i++) {
            int pin = RGB_CANDIDATES[i];
            sesame_printf(out, "  [%u/%u] gpio %d\n", i + 1, n, pin);
            char cmd[48];
            snprintf(cmd, sizeof(cmd), "rgb 0 0 255 %d", pin);
            int rc = 0;
            char *o = sesame_capture(cmd, &rc);
            free(o);
            vTaskDelay(pdMS_TO_TICKS(1800));
            snprintf(cmd, sizeof(cmd), "rgb 0 0 0 %d", pin);
            o = sesame_capture(cmd, &rc);
            free(o);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        sesame_write(out, "\nfound one? cfg set rgb.pin <n>\n");
        return 0;
    }

    if (strcmp(argv[1], "pin") == 0 && argc >= 3) {
        cfg_set("led.pin", argv[2]);
        sesame_printf(out, "led.pin = %s (reboot to apply)\n", argv[2]);
        return 0;
    }

    if (strcmp(argv[1], "invert") == 0 && argc >= 3) {
        cfg_set("led.inv", argv[2]);
        sesame_printf(out, "led.inv = %s (reboot to apply)\n", argv[2]);
        return 0;
    }

    if (strcmp(argv[1], "off") == 0)      { led_set(LED_OFF);      sesame_write(out, "off\n");      return 0; }
    if (strcmp(argv[1], "idle") == 0)     { led_set(LED_IDLE);     sesame_write(out, "idle\n");     return 0; }
    if (strcmp(argv[1], "think") == 0)    { led_set(LED_THINKING); sesame_write(out, "thinking\n"); return 0; }
    if (strcmp(argv[1], "ok") == 0)       { led_set(LED_OK);       sesame_write(out, "ok\n");       return 0; }
    if (strcmp(argv[1], "error") == 0)    { led_set(LED_ERROR);    sesame_write(out, "error\n");    return 0; }

    sesame_write(out, "usage: led [status|find|rgbfind|blink <pin> [s]|pin <n>|"
                      "invert <0|1>|off|idle|think|ok|error]\n");
    return 1;
}

static const sesame_cmd_t CMDS[] = {
    { "led", "led [status|find|rgbfind|blink <pin> [s]|pin <n>|off|idle|think]",
      "status light: find its pin, or drive it", cmd_led },
};

void cmd_led_register(void)
{
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
}
