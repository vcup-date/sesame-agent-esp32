#include "camera.h"
#include "cfg.h"
#include "sesame.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "camera";

#define CAM_PIN_COUNT 16
static const int CAM_DEFAULT[CAM_PIN_COUNT] = {
    -1, -1, 15, 4, 5, 16, 17, 18, 12, 10, 8, 9, 11, 6, 7, 13
};
static int s_pins[CAM_PIN_COUNT];

enum { P_PWDN, P_RESET, P_XCLK, P_SIOD, P_SIOC, P_D7, P_D6, P_D5,
       P_D4, P_D3, P_D2, P_D1, P_D0, P_VSYNC, P_HREF, P_PCLK };

static const char *CAM_PIN_NAMES[CAM_PIN_COUNT] = {
    "pwdn", "reset", "xclk", "siod", "sioc", "d7", "d6", "d5",
    "d4", "d3", "d2", "d1", "d0", "vsync", "href", "pclk"
};

static void load_pins(void)
{
    memcpy(s_pins, CAM_DEFAULT, sizeof(s_pins));

    const char *spec = cfg_get("cam.pins", "");
    if (!spec[0]) {
        return;
    }

    int tmp[CAM_PIN_COUNT];
    int n = 0;
    const char *p = spec;
    while (*p && n < CAM_PIN_COUNT) {
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) {
            break;
        }
        tmp[n++] = (int)v;
        p = end;
        while (*p == ',' || *p == ' ') {
            p++;
        }
    }

    if (n == CAM_PIN_COUNT) {
        memcpy(s_pins, tmp, sizeof(s_pins));
        ESP_LOGI(TAG, "using cam.pins override");
    } else {
        ESP_LOGW(TAG, "cam.pins has %d of %d entries; ignoring it", n, CAM_PIN_COUNT);
    }
}

#define PIN_SIOD  s_pins[P_SIOD]
#define PIN_SIOC  s_pins[P_SIOC]

#define CAM_I2C_PORT I2C_NUM_0

static bool s_ready;

static bool s_failed;
static i2c_master_bus_handle_t s_bus;

i2c_master_bus_handle_t camera_i2c_bus(void) { return s_bus; }

int camera_pins(int *out, int max)
{
    load_pins();
    int n = max < CAM_PIN_COUNT ? max : CAM_PIN_COUNT;
    memcpy(out, s_pins, n * sizeof(int));
    return n;
}

static void i2c_bus_recover(int sda, int scl)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << sda) | (1ULL << scl),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(sda, 1);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(10);

    if (gpio_get_level(sda) == 1) {
        gpio_reset_pin(sda);
        gpio_reset_pin(scl);
        return;
    }

    ESP_LOGW(TAG, "SDA held low, clocking the bus free");

    for (int i = 0; i < 18 && gpio_get_level(sda) == 0; i++) {
        gpio_set_level(scl, 0);
        esp_rom_delay_us(5);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(5);
    }

    gpio_set_level(sda, 0);
    esp_rom_delay_us(5);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(5);
    gpio_set_level(sda, 1);
    esp_rom_delay_us(5);

    ESP_LOGI(TAG, "bus recovery %s", gpio_get_level(sda) ? "succeeded" : "FAILED");

    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
}

esp_err_t camera_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    load_pins();
    i2c_bus_recover(PIN_SIOD, PIN_SIOC);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = CAM_I2C_PORT,
        .sda_io_num        = PIN_SIOD,
        .scl_io_num        = PIN_SIOC,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = ESP_OK;
    if (i2c_master_get_bus_handle(CAM_I2C_PORT, &s_bus) != ESP_OK || s_bus == NULL) {
        err = i2c_new_master_bus(&bus_cfg, &s_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c bus: %s", esp_err_to_name(err));
            s_failed = true;
            return err;
        }
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_bus_reset(s_bus));

    camera_config_t cfg = {
        .pin_pwdn     = s_pins[P_PWDN],
        .pin_reset    = s_pins[P_RESET],
        .pin_xclk     = s_pins[P_XCLK],

        .pin_sccb_sda = -1,
        .pin_sccb_scl = -1,
        .sccb_i2c_port = CAM_I2C_PORT,

        .pin_d7 = s_pins[P_D7], .pin_d6 = s_pins[P_D6],
        .pin_d5 = s_pins[P_D5], .pin_d4 = s_pins[P_D4],
        .pin_d3 = s_pins[P_D3], .pin_d2 = s_pins[P_D2],
        .pin_d1 = s_pins[P_D1], .pin_d0 = s_pins[P_D0],
        .pin_vsync = s_pins[P_VSYNC],
        .pin_href  = s_pins[P_HREF],
        .pin_pclk  = s_pins[P_PCLK],

        .xclk_freq_hz = 20000000,

        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = FRAMESIZE_VGA,
        .jpeg_quality = 12,
        .fb_count     = 3,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,
    };

    err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init: %s", esp_err_to_name(err));
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        s_failed = true;
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {

        s->set_hmirror(s, 1);
        s->set_vflip(s, 0);
    }

    s_ready = true;
    ESP_LOGI(TAG, "cam init ok");
    return ESP_OK;
}

bool camera_ready(void) { return s_ready; }

esp_err_t camera_snap_to_file(const char *path, size_t *size, int *w, int *h)
{
    if (!s_ready) {
        if (s_failed) {
            return ESP_ERR_NOT_FOUND;
        }
        esp_err_t err = camera_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "frame grab failed");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    FILE *f = fopen(path, "wb");
    if (!f) {
        err = ESP_FAIL;
    } else {
        size_t written = fwrite(fb->buf, 1, fb->len, f);
        fclose(f);
        if (written != fb->len) {
            err = ESP_ERR_NO_MEM;
        }
    }

    if (size) *size = fb->len;
    if (w)    *w    = fb->width;
    if (h)    *h    = fb->height;

    esp_camera_fb_return(fb);
    return err;
}

static void ensure_photo_dir(void)
{
    struct stat st;
    if (stat(SESAME_MOUNT "/photos", &st) != 0) {
        mkdir(SESAME_MOUNT "/photos", 0777);
    }
}

static int cmd_snap(int argc, char **argv, sesame_out_t *out)
{
    char path[256];

    if (argc > 1) {
        if (argv[1][0] == '/') {
            snprintf(path, sizeof(path), "%s", argv[1]);
        } else {
            snprintf(path, sizeof(path), "%s/%s", SESAME_MOUNT, argv[1]);
        }
    } else {

        ensure_photo_dir();
        struct stat st;
        int n = 0;
        do {
            snprintf(path, sizeof(path), "%s/photos/%03d.jpg", SESAME_MOUNT, n++);
        } while (stat(path, &st) == 0 && n < 1000);
    }

    size_t size = 0;
    int w = 0, h = 0;
    esp_err_t err = camera_snap_to_file(path, &size, &w, &h);
    if (err != ESP_OK) {

        sesame_write(out,
            "snap: the camera did not answer on SCCB. Retrying will not help — "
            "each attempt costs ~18s of bus timeouts. It is one of: the wrong "
            "pinout for this board (see `camera pins`), the ribbon connector, "
            "or a sensor that needs its power removed (a reset is not enough). "
            "Report this to the user and carry on; every other command works.\n");
        return 1;
    }

    sesame_printf(out, "%s  %dx%d  %u bytes\n", path, w, h, (unsigned)size);
    return 0;
}

static int cmd_camera(int argc, char **argv, sesame_out_t *out)
{
    if (argc > 1 && strcmp(argv[1], "init") == 0) {
        s_failed = false;
        esp_err_t err = camera_init();
        sesame_printf(out, "%s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }

    if (argc > 1 && strcmp(argv[1], "pins") == 0) {
        load_pins();
        if (argc > 2) {

            char spec[160] = { 0 };
            for (int i = 2; i < argc; i++) {
                strncat(spec, argv[i], sizeof(spec) - strlen(spec) - 1);
            }
            if (cfg_set("cam.pins", spec) != ESP_OK) {
                sesame_write(out, "camera: could not save that pinout\n");
                return 1;
            }
            sesame_printf(out, "cam.pins = %s\nreboot to apply\n", spec);
            return 0;
        }
        for (int i = 0; i < CAM_PIN_COUNT; i++) {
            sesame_printf(out, "%-6s %d\n", CAM_PIN_NAMES[i], s_pins[i]);
        }
        sesame_write(out,
            "\nset with: camera pins "
            "pwdn,reset,xclk,siod,sioc,d7,d6,d5,d4,d3,d2,d1,d0,vsync,href,pclk\n"
            "-1 means not connected. Reboot to apply.\n");
        return 0;
    }

    if (!s_ready) {
        sesame_write(out, "camera not initialised (try 'camera init', or "
                          "'camera pins' to check the wiring map)\n");
        return 1;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (argc > 2 && strcmp(argv[1], "size") == 0) {
        framesize_t fs = FRAMESIZE_SVGA;
        if      (strcmp(argv[2], "qvga") == 0) fs = FRAMESIZE_QVGA;
        else if (strcmp(argv[2], "vga")  == 0) fs = FRAMESIZE_VGA;
        else if (strcmp(argv[2], "svga") == 0) fs = FRAMESIZE_SVGA;
        else if (strcmp(argv[2], "xga")  == 0) fs = FRAMESIZE_XGA;
        else if (strcmp(argv[2], "uxga") == 0) fs = FRAMESIZE_UXGA;
        else {
            sesame_write(out, "sizes: qvga vga svga xga uxga\n");
            return 1;
        }
        s->set_framesize(s, fs);
        sesame_printf(out, "frame size %s\n", argv[2]);
        return 0;
    }

    sesame_printf(out, "sensor  0x%02x\n", s ? s->id.PID : 0);
    sesame_printf(out, "status  ready\n");
    sesame_write(out, "usage: camera [init|size <qvga|vga|svga|xga|uxga>]\n");
    return 0;
}

static const sesame_cmd_t CMDS[] = {
    { "snap",   "snap [path]",         "take a photo and save it as JPEG", cmd_snap },
    { "camera", "camera [init|pins|size]", "camera status, wiring map and settings",
      cmd_camera },
};

void cmd_cam_register(void)
{
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
}
