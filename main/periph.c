#include "sesame.h"
#include "camera.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/i2c_master.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "driver/sdm.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "periph";

static bool safe_pin(int pin)
{
    return pin >= 0 && pin <= 48 &&
           !(pin >= 26 && pin <= 32) && !(pin >= 33 && pin <= 37);
}

static int parse_hex_bytes(const char *s, uint8_t *out, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == ',') {
            s++;
        }
        if (!*s) {
            break;
        }
        char *end = NULL;
        long v = strtol(s, &end, 16);
        if (end == s || v < 0 || v > 0xff) {
            return -1;
        }
        out[n++] = (uint8_t)v;
        s = end;
    }
    return n;
}

static int cmd_i2creg(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 3) {
        sesame_write(out,
            "usage: i2creg read <addr> <reg> [count]\n"
            "       i2creg write <addr> <reg> <hex bytes...>\n"
            "addresses and registers are hex, e.g. 3c 00\n");
        return 1;
    }

    if (!camera_ready()) {
        sesame_write(out, "i2creg: the I2C bus is only up when the camera is\n");
        return 1;
    }
    i2c_master_bus_handle_t bus = camera_i2c_bus();

    int addr = (int)strtol(argv[2], NULL, 16);
    if (addr < 0x08 || addr > 0x77) {
        sesame_printf(out, "i2creg: 0x%02x is outside the 7-bit range\n", addr);
        return 1;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 100000,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
        sesame_write(out, "i2creg: could not attach to that address\n");
        return 1;
    }

    int rc = 1;
    if (strcmp(argv[1], "read") == 0 && argc >= 4) {
        uint8_t reg = (uint8_t)strtol(argv[3], NULL, 16);
        int count = argc > 4 ? atoi(argv[4]) : 1;
        if (count < 1)  count = 1;
        if (count > 64) count = 64;

        uint8_t buf[64] = { 0 };
        if (i2c_master_transmit_receive(dev, &reg, 1, buf, count, 200) == ESP_OK) {
            for (int i = 0; i < count; i++) {
                sesame_printf(out, "%02x%s", buf[i], i + 1 < count ? " " : "\n");
            }
            rc = 0;
        } else {
            sesame_printf(out, "i2creg: no response from 0x%02x\n", addr);
        }
    } else if (strcmp(argv[1], "write") == 0 && argc >= 5) {
        uint8_t payload[65];
        payload[0] = (uint8_t)strtol(argv[3], NULL, 16);

        char joined[192] = { 0 };
        for (int i = 4; i < argc; i++) {
            strncat(joined, argv[i], sizeof(joined) - strlen(joined) - 2);
            strncat(joined, " ", sizeof(joined) - strlen(joined) - 1);
        }
        int n = parse_hex_bytes(joined, payload + 1, sizeof(payload) - 1);
        if (n < 0) {
            sesame_write(out, "i2creg: could not parse those hex bytes\n");
        } else if (i2c_master_transmit(dev, payload, n + 1, 200) == ESP_OK) {
            sesame_printf(out, "wrote %d byte%s to 0x%02x reg %02x\n",
                          n, n == 1 ? "" : "s", addr, payload[0]);
            rc = 0;
        } else {
            sesame_printf(out, "i2creg: write to 0x%02x failed\n", addr);
        }
    } else {
        sesame_write(out, "usage: i2creg read|write <addr> <reg> ...\n");
    }

    i2c_master_bus_rm_device(dev);
    return rc;
}

static int cmd_spi(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 6) {
        sesame_write(out,
            "usage: spi <sclk> <mosi> <miso|-1> <cs> <hex bytes...>\n"
            "example: spi 12 11 13 10 01 8f 00\n");
        return 1;
    }

    int sclk = atoi(argv[1]), mosi = atoi(argv[2]);
    int miso = atoi(argv[3]), cs = atoi(argv[4]);

    uint8_t tx[64];
    char joined[192] = { 0 };
    for (int i = 5; i < argc; i++) {
        strncat(joined, argv[i], sizeof(joined) - strlen(joined) - 2);
        strncat(joined, " ", sizeof(joined) - strlen(joined) - 1);
    }
    int n = parse_hex_bytes(joined, tx, sizeof(tx));
    if (n <= 0) {
        sesame_write(out, "spi: could not parse those hex bytes\n");
        return 1;
    }

    spi_bus_config_t bus = {
        .mosi_io_num     = mosi,
        .miso_io_num     = miso,
        .sclk_io_num     = sclk,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 64,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        sesame_write(out, "spi: could not initialise SPI2 (already in use?)\n");
        return 1;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 1000000,
        .mode           = 0,
        .spics_io_num   = cs,
        .queue_size     = 1,
    };
    spi_device_handle_t dev = NULL;
    if (spi_bus_add_device(SPI2_HOST, &dev_cfg, &dev) != ESP_OK) {
        spi_bus_free(SPI2_HOST);
        sesame_write(out, "spi: could not attach the device\n");
        return 1;
    }

    uint8_t rx[64] = { 0 };
    spi_transaction_t t = {
        .length    = (size_t)n * 8,
        .tx_buffer = tx,
        .rx_buffer = miso >= 0 ? rx : NULL,
    };

    int rc = 1;
    if (spi_device_transmit(dev, &t) == ESP_OK) {
        if (miso >= 0) {
            sesame_write(out, "rx: ");
            for (int i = 0; i < n; i++) {
                sesame_printf(out, "%02x%s", rx[i], i + 1 < n ? " " : "\n");
            }
        } else {
            sesame_printf(out, "sent %d byte%s\n", n, n == 1 ? "" : "s");
        }
        rc = 0;
    } else {
        sesame_write(out, "spi: transfer failed\n");
    }

    spi_bus_remove_device(dev);
    spi_bus_free(SPI2_HOST);
    return rc;
}

static int cmd_tone(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 3) {
        sesame_write(out, "usage: tone <pin> <freq_hz> [ms]\n");
        return 1;
    }

    int pin  = atoi(argv[1]);
    int freq = atoi(argv[2]);
    int ms   = argc > 3 ? atoi(argv[3]) : 200;

    if (freq < 20 || freq > 20000) {
        sesame_write(out, "tone: frequency must be 20..20000 Hz\n");
        return 1;
    }
    if (ms < 10)    ms = 10;
    if (ms > 10000) ms = 10000;

    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_3,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz         = freq,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK) {
        sesame_write(out, "tone: that frequency is out of range for the timer\n");
        return 1;
    }

    ledc_channel_config_t chan = {
        .gpio_num   = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_3,
        .timer_sel  = LEDC_TIMER_3,
        .duty       = 512,
        .hpoint     = 0,
    };
    if (ledc_channel_config(&chan) != ESP_OK) {
        sesame_write(out, "tone: cannot drive that pin\n");
        return 1;
    }

    vTaskDelay(pdMS_TO_TICKS(ms));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3);
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, 0);

    sesame_printf(out, "%d Hz on pin %d for %d ms\n", freq, pin, ms);
    return 0;
}

static void write_wav_header(FILE *f, int rate, uint32_t data_bytes)
{
    uint8_t h[44] = { 0 };
    uint32_t chunk = 36 + data_bytes;
    uint32_t byte_rate = rate * 2;

    memcpy(h + 0,  "RIFF", 4);
    memcpy(h + 4,  &chunk, 4);
    memcpy(h + 8,  "WAVEfmt ", 8);
    h[16] = 16;
    h[20] = 1;
    h[22] = 1;
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    h[32] = 2;
    h[34] = 16;
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);

    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), f);
}

static int i2s_record(int argc, char **argv, sesame_out_t *out)
{

    bool pdm = strcmp(argv[1], "pdmrec") == 0;
    int need = pdm ? 5 : 6;
    if (argc < need) {
        sesame_write(out, pdm
            ? "usage: i2s pdmrec <clk> <din> <file.wav> [secs] [rate]\n"
            : "usage: i2s rec <bclk> <ws> <din> <file.wav> [secs] [rate]\n");
        return 1;
    }

    int a = 2;
    int bclk = atoi(argv[a++]);
    int ws   = pdm ? -1 : atoi(argv[a++]);
    int din  = atoi(argv[a++]);
    const char *fname = argv[a++];
    int secs = argc > a ? atoi(argv[a]) : 3;
    int rate = argc > a + 1 ? atoi(argv[a + 1]) : 16000;

    if (secs < 1)  secs = 1;
    if (secs > 30) secs = 30;
    if (rate < 8000)  rate = 8000;
    if (rate > 48000) rate = 48000;

    char path[288];
    if (fname[0] == '/') {
        snprintf(path, sizeof(path), "%s", fname);
    } else {
        snprintf(path, sizeof(path), "%s/%s", SESAME_MOUNT, fname);
    }

    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan_cfg, NULL, &rx) != ESP_OK) {
        sesame_write(out, "i2s: no free channel\n");
        return 1;
    }

    esp_err_t err;
    if (pdm) {
        i2s_pdm_rx_config_t cfg = {
            .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(rate),
            .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .clk = bclk,
                .din = din,
                .invert_flags = { false },
            },
        };
        err = i2s_channel_init_pdm_rx_mode(rx, &cfg);
    } else {
        i2s_std_config_t cfg = {
            .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(rate),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                            I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = bclk,
                .ws   = ws,
                .dout = I2S_GPIO_UNUSED,
                .din  = din,
                .invert_flags = { false, false, false },
            },
        };
        err = i2s_channel_init_std_mode(rx, &cfg);
    }
    if (err != ESP_OK || i2s_channel_enable(rx) != ESP_OK) {
        sesame_write(out, "i2s: could not configure those pins for input\n");
        i2s_del_channel(rx);
        return 1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        sesame_printf(out, "i2s: cannot write %s\n", path);
        i2s_channel_disable(rx);
        i2s_del_channel(rx);
        return 1;
    }

    uint8_t hdr[44] = { 0 };
    fwrite(hdr, 1, sizeof(hdr), f);

    static int16_t buf[512];
    uint32_t written = 0;
    uint32_t want = (uint32_t)rate * 2 * secs;
    int32_t peak = 0;

    while (written < want) {
        size_t got = 0;
        if (i2s_channel_read(rx, buf, sizeof(buf), &got, 1000) != ESP_OK || got == 0) {
            break;
        }

        int n = got / sizeof(int16_t);
        for (int i = 0; i < n; i++) {
            int32_t v = buf[i] < 0 ? -buf[i] : buf[i];
            if (v > peak) peak = v;
        }
        fwrite(buf, 1, got, f);
        written += got;
    }

    write_wav_header(f, rate, written);
    fclose(f);
    i2s_channel_disable(rx);
    i2s_del_channel(rx);

    sesame_printf(out, "%s: %u bytes, %.1f s at %d Hz mono\n",
                  path, (unsigned)written, (double)written / (rate * 2), rate);
    sesame_printf(out, "peak level %d/32767%s\n", (int)peak,
                  peak < 40 ? "  — essentially silent; check the mic wiring" : "");
    return 0;
}

static int cmd_i2s(int argc, char **argv, sesame_out_t *out)
{
    if (argc >= 2 && (strcmp(argv[1], "rec") == 0 || strcmp(argv[1], "pdmrec") == 0)) {
        return i2s_record(argc, argv, out);
    }

    if (argc < 5) {
        sesame_write(out,
            "usage: i2s tone <bclk> <lrclk> <dout> [freq] [ms]\n"
            "       i2s play <bclk> <lrclk> <dout> <file.wav>\n"
            "       i2s rec <bclk> <ws> <din> <file.wav> [secs] [rate]\n"
            "       i2s pdmrec <clk> <din> <file.wav> [secs] [rate]\n"
            "out: a MAX98357A-style amp. in: an INMP441/SPH0645 (rec) or a\n"
            "PDM mic such as the MP34DT05 (pdmrec). UNTESTED — no hardware here.\n");
        return 1;
    }

    bool play = strcmp(argv[1], "play") == 0;
    int bclk = atoi(argv[2]), lrclk = atoi(argv[3]), dout = atoi(argv[4]);

    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan_cfg, &tx, NULL) != ESP_OK) {
        sesame_write(out, "i2s: no free channel\n");
        return 1;
    }

    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk,
            .ws   = lrclk,
            .dout = dout,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    if (i2s_channel_init_std_mode(tx, &std) != ESP_OK ||
        i2s_channel_enable(tx) != ESP_OK) {
        sesame_write(out, "i2s: could not configure those pins\n");
        i2s_del_channel(tx);
        return 1;
    }

    int rc = 1;
    size_t written = 0;

    if (play) {
        if (argc < 6) {
            sesame_write(out, "i2s play needs a file\n");
        } else {
            char path[288];
            if (argv[5][0] == '/') {
                snprintf(path, sizeof(path), "%s", argv[5]);
            } else {
                snprintf(path, sizeof(path), "%s/%s", SESAME_MOUNT, argv[5]);
            }
            FILE *f = fopen(path, "rb");
            if (!f) {
                sesame_printf(out, "i2s: cannot open %s\n", path);
            } else {

                fseek(f, 44, SEEK_SET);
                static uint8_t buf[1024];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                    size_t w = 0;
                    i2s_channel_write(tx, buf, n, &w, 1000);
                    written += w;
                }
                fclose(f);
                sesame_printf(out, "played %u bytes from %s\n",
                              (unsigned)written, path);
                rc = 0;
            }
        }
    } else {
        int freq = argc > 5 ? atoi(argv[5]) : 440;
        int ms   = argc > 6 ? atoi(argv[6]) : 500;
        if (ms > 5000) ms = 5000;

        const int rate = 16000;
        int total = rate * ms / 1000;
        static int16_t frame[256];
        int phase = 0, period = rate / (freq > 0 ? freq : 440);
        if (period < 2) period = 2;

        while (total > 0) {
            int n = total > 256 ? 256 : total;
            for (int i = 0; i < n; i++) {
                int p = phase % period;
                int tri = (p * 2 < period) ? p : period - p;
                frame[i] = (int16_t)((tri * 16000) / (period / 2 + 1) - 8000);
                phase++;
            }
            size_t w = 0;
            i2s_channel_write(tx, frame, n * sizeof(int16_t), &w, 1000);
            written += w;
            total -= n;
        }
        sesame_printf(out, "%d Hz for %d ms on bclk=%d lrclk=%d dout=%d\n",
                      freq, ms, bclk, lrclk, dout);
        rc = 0;
    }

    i2s_channel_disable(tx);
    i2s_del_channel(tx);
    return rc;
}

#define DAC_OVERSAMPLE_HZ  (10 * 1000 * 1000)
#define DAC_SAMPLE_US      100
#define DAC_MAX_POINTS     500

static sdm_channel_handle_t s_dac_chan;
static int                  s_dac_pin = -1;

static esp_err_t dac_ensure(int pin)
{
    if (s_dac_chan && s_dac_pin == pin) {
        return ESP_OK;
    }
    if (s_dac_chan) {
        sdm_channel_disable(s_dac_chan);
        sdm_del_channel(s_dac_chan);
        s_dac_chan = NULL;
    }
    sdm_config_t cfg = {
        .gpio_num        = pin,
        .clk_src         = SDM_CLK_SRC_DEFAULT,
        .sample_rate_hz  = DAC_OVERSAMPLE_HZ,
    };
    if (sdm_new_channel(&cfg, &s_dac_chan) != ESP_OK) {
        return ESP_FAIL;
    }
    if (sdm_channel_enable(s_dac_chan) != ESP_OK) {
        sdm_del_channel(s_dac_chan);
        s_dac_chan = NULL;
        return ESP_FAIL;
    }
    s_dac_pin = pin;
    return ESP_OK;
}

typedef struct {
    sdm_channel_handle_t chan;
    const int8_t *wave;
    int n, i;
} dac_tone_ctx_t;

static bool IRAM_ATTR dac_tone_cb(gptimer_handle_t t, const gptimer_alarm_event_data_t *e, void *arg)
{
    (void)t; (void)e;
    dac_tone_ctx_t *c = arg;
    sdm_channel_set_pulse_density(c->chan, c->wave[c->i]);
    c->i = (c->i + 1) % c->n;
    return false;
}

static void dac_play_one(sdm_channel_handle_t chan, int freq, int ms)
{
    int n = 1000000 / (DAC_SAMPLE_US * freq);
    if (n < 4)              n = 4;
    if (n > DAC_MAX_POINTS) n = DAC_MAX_POINTS;

    int8_t wave[DAC_MAX_POINTS];
    for (int i = 0; i < n; i++) {
        wave[i] = (int8_t)(sinf(2.0f * (float)M_PI * i / n) * 120.0f);
    }

    dac_tone_ctx_t ctx = { .chan = chan, .wave = wave, .n = n, .i = 0 };

    gptimer_handle_t timer = NULL;
    gptimer_config_t tcfg = {
        .clk_src      = GPTIMER_CLK_SRC_DEFAULT,
        .direction    = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
    };
    if (gptimer_new_timer(&tcfg, &timer) != ESP_OK) {
        return;
    }
    gptimer_alarm_config_t alarm = {
        .alarm_count = DAC_SAMPLE_US,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    gptimer_event_callbacks_t cbs = { .on_alarm = dac_tone_cb };
    gptimer_set_alarm_action(timer, &alarm);
    gptimer_register_event_callbacks(timer, &cbs, &ctx);
    gptimer_enable(timer);
    gptimer_start(timer);

    vTaskDelay(pdMS_TO_TICKS(ms));

    gptimer_stop(timer);
    gptimer_disable(timer);
    gptimer_del_timer(timer);
    sdm_channel_set_pulse_density(chan, 0);
}

static int cmd_dac(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) {
        sesame_write(out,
            "usage: dac <pin> <level 0-255>\n"
            "       dac tone <pin> <freq_hz> [ms]\n"
            "       dac play <pin> <freq:ms,freq:ms,...>\n"
            "       dac off\n"
            "needs an RC low-pass filter (e.g. 1k + 100nF) between the pin and\n"
            "whatever reads the voltage; the raw pin is still a fast digital toggle\n");
        return 1;
    }

    if (strcmp(argv[1], "off") == 0) {
        if (s_dac_chan) {
            sdm_channel_disable(s_dac_chan);
            sdm_del_channel(s_dac_chan);
            s_dac_chan = NULL;
            s_dac_pin = -1;
        }
        sesame_write(out, "dac off\n");
        return 0;
    }

    if (strcmp(argv[1], "tone") == 0 || strcmp(argv[1], "play") == 0) {
        bool play = strcmp(argv[1], "play") == 0;
        if (argc < 4) {
            sesame_printf(out, "usage: dac %s <pin> <%s>\n", argv[1],
                          play ? "freq:ms,freq:ms,..." : "freq_hz> [ms");
            return 1;
        }
        int pin = atoi(argv[2]);
        if (!safe_pin(pin)) {
            sesame_printf(out, "dac: %d is not a usable pin on this chip\n", pin);
            return 1;
        }
        if (dac_ensure(pin) != ESP_OK) {
            sesame_write(out, "dac: could not open a sigma-delta channel on that pin\n");
            return 1;
        }

        if (!play) {
            int freq = atoi(argv[3]);
            int ms   = argc > 4 ? atoi(argv[4]) : 500;
            if (freq < 20)   freq = 20;
            if (freq > 4000) freq = 4000;
            if (ms < 20)     ms = 20;
            if (ms > 5000)   ms = 5000;
            dac_play_one(s_dac_chan, freq, ms);
            sesame_printf(out, "%d Hz sine on pin %d for %d ms\n", freq, pin, ms);
            return 0;
        }

        int notes = 0, total_ms = 0;
        char buf[256];
        strlcpy(buf, argv[3], sizeof(buf));
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            if (notes >= 24 || total_ms >= 12000) {
                sesame_printf(out, "dac play: stopped at %d notes / %dms (limit)\n", notes, total_ms);
                break;
            }
            char *colon = strchr(tok, ':');
            if (!colon) continue;
            *colon = '\0';
            int freq = atoi(tok);
            int ms   = atoi(colon + 1);
            if (freq < 20)   freq = 20;
            if (freq > 4000) freq = 4000;
            if (ms < 20)     ms = 20;
            if (ms > 1500)   ms = 1500;
            dac_play_one(s_dac_chan, freq, ms);
            notes++;
            total_ms += ms;
        }
        sesame_printf(out, "played %d note%s (%d ms) on pin %d\n",
                      notes, notes == 1 ? "" : "s", total_ms, pin);
        return notes ? 0 : 1;
    }

    if (argc < 3) {
        sesame_write(out, "usage: dac <pin> <level 0-255>\n");
        return 1;
    }
    int pin   = atoi(argv[1]);
    int level = atoi(argv[2]);
    if (!safe_pin(pin)) {
        sesame_printf(out, "dac: %d is not a usable pin on this chip\n", pin);
        return 1;
    }
    if (level < 0)   level = 0;
    if (level > 255) level = 255;

    if (dac_ensure(pin) != ESP_OK) {
        sesame_write(out, "dac: could not open a sigma-delta channel on that pin\n");
        return 1;
    }
    sdm_channel_set_pulse_density(s_dac_chan, (int8_t)(level - 128));
    sesame_printf(out, "dac %d: level %d/255 (needs the RC filter to read as a steady voltage)\n",
                  pin, level);
    return 0;
}

#define IR_MAX_SYMBOLS  64

static bool ir_rx_done(rmt_channel_handle_t chan, const rmt_rx_done_event_data_t *edata, void *ctx)
{
    (void)chan;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR((QueueHandle_t)ctx, edata, &woken);
    return woken == pdTRUE;
}

static int cmd_ir(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 3) {
        sesame_write(out,
            "usage: ir rx <pin> [ms]\n"
            "       ir tx <pin> <+mark,-space,+mark,-space,...>\n"
            "durations are microseconds; + is the carrier on, - is off. rx needs a\n"
            "demodulating IR receiver (e.g. TSOP38238); tx needs an IR LED, ideally\n"
            "through a transistor for range. rx's output pastes straight into tx.\n");
        return 1;
    }

    int pin = atoi(argv[2]);
    if (!safe_pin(pin)) {
        sesame_printf(out, "ir: %d is not a usable pin on this chip\n", pin);
        return 1;
    }

    if (strcmp(argv[1], "rx") == 0) {
        int ms = argc > 3 ? atoi(argv[3]) : 8000;
        if (ms < 200)   ms = 200;
        if (ms > 15000) ms = 15000;

        rmt_rx_channel_config_t rx_cfg = {
            .clk_src           = RMT_CLK_SRC_DEFAULT,
            .resolution_hz     = 1000000,
            .mem_block_symbols = IR_MAX_SYMBOLS,
            .gpio_num          = pin,
        };
        rmt_channel_handle_t chan = NULL;
        if (rmt_new_rx_channel(&rx_cfg, &chan) != ESP_OK) {
            sesame_write(out, "ir: could not open an RMT RX channel on that pin\n");
            return 1;
        }

        QueueHandle_t q = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
        if (!q) {
            rmt_del_channel(chan);
            return 1;
        }
        rmt_rx_event_callbacks_t cbs = { .on_recv_done = ir_rx_done };
        rmt_rx_register_event_callbacks(chan, &cbs, q);
        rmt_enable(chan);

        rmt_symbol_word_t syms[IR_MAX_SYMBOLS];
        rmt_receive_config_t recv_cfg = {
            .signal_range_min_ns = 1000,
            .signal_range_max_ns = 15000000,
        };
        rmt_receive(chan, syms, sizeof(syms), &recv_cfg);

        rmt_rx_done_event_data_t rx;
        int rc = 1;
        if (xQueueReceive(q, &rx, pdMS_TO_TICKS(ms)) == pdTRUE && rx.num_symbols > 0) {
            for (size_t i = 0; i < rx.num_symbols; i++) {
                sesame_printf(out, "%s%d,%s%d%s",
                              rx.received_symbols[i].level0 ? "+" : "-", rx.received_symbols[i].duration0,
                              rx.received_symbols[i].level1 ? "+" : "-", rx.received_symbols[i].duration1,
                              i + 1 < rx.num_symbols ? "," : "\n");
            }
            sesame_printf(out, "%u symbols captured on pin %d\n", (unsigned)rx.num_symbols, pin);
            rc = 0;
        } else {
            sesame_printf(out, "ir: nothing received on pin %d in %d ms\n", pin, ms);
        }

        rmt_disable(chan);
        rmt_del_channel(chan);
        vQueueDelete(q);
        return rc;
    }

    if (strcmp(argv[1], "tx") == 0) {
        if (argc < 4) {
            sesame_write(out, "usage: ir tx <pin> <+mark,-space,...>\n");
            return 1;
        }

        rmt_symbol_word_t syms[IR_MAX_SYMBOLS] = { 0 };
        int n = 0, have = 0;
        char buf[512];
        strlcpy(buf, argv[3], sizeof(buf));
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok && n < IR_MAX_SYMBOLS; tok = strtok_r(NULL, ",", &save)) {
            bool level = tok[0] != '-';
            long dur = labs(strtol(tok, NULL, 10));
            if (dur < 1)     dur = 1;
            if (dur > 32000) dur = 32000;
            if (!have) {
                syms[n].level0 = level; syms[n].duration0 = (uint16_t)dur; have = 1;
            } else {
                syms[n].level1 = level; syms[n].duration1 = (uint16_t)dur; have = 0; n++;
            }
        }
        if (have) {
            syms[n].level1 = 0; syms[n].duration1 = 1; n++;
        }
        if (n == 0) {
            sesame_write(out, "ir: could not parse that mark/space list\n");
            return 1;
        }

        rmt_tx_channel_config_t tx_cfg = {
            .clk_src           = RMT_CLK_SRC_DEFAULT,
            .resolution_hz     = 1000000,
            .mem_block_symbols = IR_MAX_SYMBOLS,
            .trans_queue_depth = 1,
            .gpio_num          = pin,
        };
        rmt_channel_handle_t chan = NULL;
        if (rmt_new_tx_channel(&tx_cfg, &chan) != ESP_OK) {
            sesame_write(out, "ir: could not open an RMT TX channel on that pin\n");
            return 1;
        }

        rmt_carrier_config_t carrier = { .frequency_hz = 38000, .duty_cycle = 0.33 };
        rmt_apply_carrier(chan, &carrier);

        rmt_copy_encoder_config_t enc_cfg = { };
        rmt_encoder_handle_t enc = NULL;
        if (rmt_new_copy_encoder(&enc_cfg, &enc) != ESP_OK) {
            rmt_del_channel(chan);
            sesame_write(out, "ir: could not create the encoder\n");
            return 1;
        }
        rmt_enable(chan);

        rmt_transmit_config_t tx_opt = { .loop_count = 0 };
        int rc = 1;
        if (rmt_transmit(chan, enc, syms, n * sizeof(rmt_symbol_word_t), &tx_opt) == ESP_OK) {
            rmt_tx_wait_all_done(chan, 2000);
            sesame_printf(out, "sent %d symbols on pin %d (38kHz carrier)\n", n, pin);
            rc = 0;
        } else {
            sesame_write(out, "ir: transmit failed\n");
        }

        rmt_disable(chan);
        rmt_del_channel(chan);
        rmt_del_encoder(enc);
        return rc;
    }

    sesame_write(out, "usage: ir rx <pin> [ms] | ir tx <pin> <list>\n");
    return 1;
}

static const sesame_cmd_t CMDS[] = {
    { "i2creg", "i2creg read|write <addr> <reg> [bytes]",
      "read or write a register on an I2C device", cmd_i2creg },
    { "spi",    "spi <sclk> <mosi> <miso|-1> <cs> <hex...>",
      "one SPI transfer (displays, e-paper)",      cmd_spi },
    { "tone",   "tone <pin> <freq_hz> [ms]",
      "square wave on a pin, for a buzzer",        cmd_tone },
    { "i2s",    "i2s tone|play|rec|pdmrec <pins...> ...",
      "audio out to an amp, or record from a mic to WAV", cmd_i2s },
    { "dac",    "dac <pin> <level>|tone <pin> <hz> [ms]|play <pin> <hz:ms,...>|off",
      "a real analog level or tone in pure software (sigma-delta + RC filter)", cmd_dac },
    { "ir",     "ir rx <pin> [ms] | ir tx <pin> <+mark,-space,...>",
      "record or replay any infrared remote, no protocol assumed", cmd_ir },
};

void cmd_periph_register(void)
{
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
    ESP_LOGD(TAG, "peripheral commands registered");
}
