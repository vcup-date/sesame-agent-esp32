#include "sesame.h"
#include "camera.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DEFAULT_SDA 4
#define DEFAULT_SCL 5

static bool valid_pin(int pin)
{
    if (pin < 0 || pin > 48) {
        return false;
    }
    if (pin >= 22 && pin <= 25) {
        return false;
    }
    if (pin >= 26 && pin <= 32) {
        return false;
    }

    if (pin >= 33 && pin <= 37) {
        return false;
    }
    return true;
}

static int cmd_gpio(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 3) {
        sesame_write(out, "usage: gpio get <pin> | gpio set <pin> <0|1>\n");
        return 1;
    }

    int pin = atoi(argv[2]);
    if (!valid_pin(pin)) {
        sesame_printf(out, "gpio: %d is not a usable pin on this chip\n", pin);
        return 1;
    }

    if (strcmp(argv[1], "get") == 0) {

        gpio_pull_mode_t mode = GPIO_FLOATING;
        const char *label = "floating";
        if (argc > 3 && strcmp(argv[3], "up") == 0) {
            mode = GPIO_PULLUP_ONLY;  label = "pull-up";
        } else if (argc > 3 && strcmp(argv[3], "down") == 0) {
            mode = GPIO_PULLDOWN_ONLY; label = "pull-down";
        }

        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(pin, mode);
        vTaskDelay(pdMS_TO_TICKS(5));
        sesame_printf(out, "%d  (%s)\n", gpio_get_level(pin), label);
        return 0;
    }

    if (strcmp(argv[1], "set") == 0 && argc >= 4) {
        int level = atoi(argv[3]) ? 1 : 0;
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_set_level(pin, level);
        sesame_printf(out, "gpio %d = %d\n", pin, level);
        return 0;
    }

    sesame_write(out, "usage: gpio get <pin> | gpio set <pin> <0|1>\n");
    return 1;
}

static int cmd_adc(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) {
        sesame_write(out, "usage: adc <channel 0-9>\n");
        return 1;
    }

    int ch = atoi(argv[1]);
    if (ch < 0 || ch > 9) {
        sesame_printf(out, "adc: channel %d out of range (0-9 on ADC1)\n", ch);
        return 1;
    }

    adc_oneshot_unit_handle_t unit = NULL;
    adc_oneshot_unit_init_cfg_t init = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&init, &unit) != ESP_OK) {
        sesame_write(out, "adc: could not open ADC1\n");
        return 1;
    }

    adc_oneshot_chan_cfg_t chan = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    int raw = 0;
    int rc = 1;
    if (adc_oneshot_config_channel(unit, ch, &chan) == ESP_OK &&
        adc_oneshot_read(unit, ch, &raw) == ESP_OK) {

        sesame_printf(out, "channel %d: raw %d (~%d mV)\n", ch, raw, raw * 3100 / 4095);
        rc = 0;
    } else {
        sesame_printf(out, "adc: read failed on channel %d\n", ch);
    }

    adc_oneshot_del_unit(unit);
    return rc;
}

static void scan_grid(i2c_master_bus_handle_t bus, int sda, int scl,
                      bool borrowed, sesame_out_t *out)
{
    sesame_write(out, "     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

    int found = 0;
    for (int row = 0; row < 8; row++) {
        sesame_printf(out, "%02x: ", row * 16);
        for (int col = 0; col < 16; col++) {
            int addr = row * 16 + col;
            if (addr < 0x08 || addr > 0x77) {
                sesame_write(out, "   ");
                continue;
            }

            bool hit = i2c_master_probe(bus, addr, 50) == ESP_OK &&
                       i2c_master_probe(bus, addr, 50) == ESP_OK;
            if (hit) {
                sesame_printf(out, "%02x ", addr);
                found++;
            } else {
                sesame_write(out, "-- ");
            }
        }
        sesame_write(out, "\n");
    }

    sesame_printf(out, "\n%d device%s on sda=%d scl=%d%s\n",
                  found, found == 1 ? "" : "s", sda, scl,
                  borrowed ? " (camera bus)" : "");
}

static int cmd_i2c(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2 || strcmp(argv[1], "scan") != 0) {
        sesame_write(out, "usage: i2c scan [sda] [scl]\n");
        return 1;
    }

    int sda = argc > 2 ? atoi(argv[2]) : DEFAULT_SDA;
    int scl = argc > 3 ? atoi(argv[3]) : DEFAULT_SCL;

    if (camera_ready() && sda == DEFAULT_SDA && scl == DEFAULT_SCL) {
        scan_grid(camera_i2c_bus(), sda, scl, true, out);
        return 0;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = sda,
        .scl_io_num        = scl,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {

        sesame_printf(out, "i2c: cannot open port 0 on sda=%d scl=%d (%s)\n",
                      sda, scl, esp_err_to_name(err));
        return 1;
    }

    scan_grid(bus, sda, scl, false, out);
    i2c_del_master_bus(bus);
    return 0;
}

static int cmd_pwm(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 3) {
        sesame_write(out, "usage: pwm <pin> <duty 0-255> [freq_hz]\n");
        return 1;
    }

    int pin  = atoi(argv[1]);
    int duty = atoi(argv[2]);
    int freq = argc > 3 ? atoi(argv[3]) : 5000;

    if (!valid_pin(pin)) {
        sesame_printf(out, "pwm: %d is not a usable pin on this chip\n", pin);
        return 1;
    }
    if (duty < 0)   duty = 0;
    if (duty > 255) duty = 255;

    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_1,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz         = freq,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK) {
        sesame_printf(out, "pwm: %d Hz is out of range for 8-bit resolution\n", freq);
        return 1;
    }

    ledc_channel_config_t chan = {
        .gpio_num   = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_1,
        .timer_sel  = LEDC_TIMER_1,
        .duty       = duty,
        .hpoint     = 0,
    };
    if (ledc_channel_config(&chan) != ESP_OK) {
        sesame_write(out, "pwm: could not configure channel\n");
        return 1;
    }

    sesame_printf(out, "pwm %d: duty %d/255 at %d Hz\n", pin, duty, freq);
    return 0;
}

static int cmd_mon(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) {
        sesame_write(out, "usage: mon <heap|adc <ch>|gpio <pin>> [samples] [ms]\n");
        return 1;
    }

    bool is_adc  = strcmp(argv[1], "adc") == 0;
    bool is_gpio = strcmp(argv[1], "gpio") == 0;
    int  chan    = (is_adc || is_gpio) && argc > 2 ? atoi(argv[2]) : 0;
    int  argbase = (is_adc || is_gpio) ? 3 : 2;

    int samples = argc > argbase     ? atoi(argv[argbase])     : 48;
    int period  = argc > argbase + 1 ? atoi(argv[argbase + 1]) : 100;

    if (samples < 4)   samples = 4;
    if (samples > 120) samples = 120;
    if (period < 10)   period = 10;

    if ((int64_t)samples * period > 20000) {
        period = 20000 / samples;
    }

    adc_oneshot_unit_handle_t unit = NULL;
    if (is_adc) {
        adc_oneshot_unit_init_cfg_t init = { .unit_id = ADC_UNIT_1 };
        adc_oneshot_chan_cfg_t chan_cfg = {
            .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_oneshot_new_unit(&init, &unit) != ESP_OK ||
            adc_oneshot_config_channel(unit, chan, &chan_cfg) != ESP_OK) {
            sesame_write(out, "mon: cannot open that ADC channel\n");
            if (unit) adc_oneshot_del_unit(unit);
            return 1;
        }
    } else if (is_gpio) {
        gpio_set_direction(chan, GPIO_MODE_INPUT);
    }

    int *buf = malloc(samples * sizeof(int));
    if (!buf) {
        if (unit) adc_oneshot_del_unit(unit);
        return 1;
    }

    for (int i = 0; i < samples; i++) {
        if (is_adc) {
            int raw = 0;
            adc_oneshot_read(unit, chan, &raw);
            buf[i] = raw;
        } else if (is_gpio) {
            buf[i] = gpio_get_level(chan);
        } else {
            buf[i] = (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
        }
        vTaskDelay(pdMS_TO_TICKS(period));
    }

    if (unit) {
        adc_oneshot_del_unit(unit);
    }

    sesame_printf(out, "%s over %d samples @ %dms\n",
                  is_adc ? "adc" : is_gpio ? "gpio" : "free sram (KB)",
                  samples, period);
    sesame_spark(out, buf, samples);
    free(buf);
    return 0;
}

static const sesame_cmd_t CMDS[] = {
    { "mon",  "mon <heap|adc <ch>|gpio <pin>> [samples] [ms]",
      "watch a value over time as a sparkline", cmd_mon },
    { "gpio", "gpio get <pin> [up|down] | gpio set <pin> <0|1>",
      "read a pin (optionally with a pull) or drive it", cmd_gpio },
    { "adc",  "adc <channel 0-9>",                     "read an analog channel",     cmd_adc },
    { "i2c",  "i2c scan [sda] [scl]",                  "probe the I2C bus",          cmd_i2c },
    { "pwm",  "pwm <pin> <duty 0-255> [freq_hz]",      "drive a pin with PWM",       cmd_pwm },
};

void cmd_hw_register(void)
{
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
}
