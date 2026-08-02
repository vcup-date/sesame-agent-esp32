#include "display.h"
#include "camera.h"
#include "cfg.h"
#include "sesame.h"

#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "disp";

#define W 128

static i2c_master_dev_handle_t s_dev;
static int  s_h = 64;
static bool s_ready;
static uint8_t s_fb[W * 8];

static const uint8_t FONT[95][5] = {
    {0,0,0,0,0},{0,0,95,0,0},{0,7,0,7,0},{20,127,20,127,20},{36,42,127,42,18},
    {35,19,8,100,98},{54,73,85,34,80},{0,5,3,0,0},{0,28,34,65,0},{0,65,34,28,0},
    {20,8,62,8,20},{8,8,62,8,8},{0,80,48,0,0},{8,8,8,8,8},{0,96,96,0,0},
    {32,16,8,4,2},{62,81,73,69,62},{0,66,127,64,0},{66,97,81,73,70},
    {33,65,69,75,49},{24,20,18,127,16},{39,69,69,69,57},{60,74,73,73,48},
    {1,113,9,5,3},{54,73,73,73,54},{6,73,73,41,30},{0,54,54,0,0},{0,86,54,0,0},
    {8,20,34,65,0},{20,20,20,20,20},{0,65,34,20,8},{2,1,81,9,6},{50,73,121,65,62},
    {126,17,17,17,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},
    {127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},
    {0,65,127,65,0},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
    {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},
    {62,65,81,33,94},{127,9,25,41,70},{38,73,73,73,50},{1,1,127,1,1},
    {63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},
    {7,8,112,8,7},{97,81,73,69,67},{0,127,65,65,0},{2,4,8,16,32},
    {0,65,65,127,0},{4,2,1,2,4},{64,64,64,64,64},{0,1,2,4,0},
    {32,84,84,84,120},{127,72,68,68,56},{56,68,68,68,32},{56,68,68,72,127},
    {56,84,84,84,24},{8,126,9,1,2},{12,82,82,82,62},{127,8,4,4,120},
    {0,68,125,64,0},{32,64,68,61,0},{127,16,40,68,0},{0,65,127,64,0},
    {124,4,24,4,120},{124,8,4,4,120},{56,68,68,68,56},{124,20,20,20,8},
    {8,20,20,24,124},{124,8,4,4,8},{72,84,84,84,32},{4,63,68,64,32},
    {60,64,64,32,124},{28,32,64,32,28},{60,64,48,64,60},{68,40,16,40,68},
    {12,80,80,80,60},{68,100,84,76,68},{0,8,54,65,0},{0,0,127,0,0},
    {0,65,54,8,0},{8,4,8,16,8},
};

static esp_err_t cmd(uint8_t c)
{
    uint8_t buf[2] = { 0x00, c };
    return i2c_master_transmit(s_dev, buf, 2, 200);
}

esp_err_t display_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = camera_i2c_bus();
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }

    s_h = atoi(cfg_get("disp.h", "64"));
    if (s_h != 32) {
        s_h = 64;
    }
    int addr = (int)strtol(cfg_get("disp.addr", "3c"), NULL, 16);

    if (i2c_master_probe(bus, addr, 100) != ESP_OK) {
        ESP_LOGI(TAG, "no display at 0x%02x", addr);
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 400000,
    };
    if (i2c_master_bus_add_device(bus, &dc, &s_dev) != ESP_OK) {
        return ESP_FAIL;
    }

    static const uint8_t INIT[] = {
        0xAE,
        0xD5, 0x80,
        0xA8, 0x3F,
        0xD3, 0x00,
        0x40,
        0x8D, 0x14,
        0x20, 0x00,
        0xA1,
        0xC8,
        0xDA, 0x12,
        0x81, 0xCF,
        0xD9, 0xF1,
        0xDB, 0x40,
        0xA4,
        0xA6,
        0xAF,
    };
    for (unsigned i = 0; i < sizeof(INIT); i++) {
        uint8_t v = INIT[i];
        if (i == 4)  v = (s_h == 32) ? 0x1F : 0x3F;
        if (i == 15) v = (s_h == 32) ? 0x02 : 0x12;
        if (cmd(v) != ESP_OK) {
            ESP_LOGE(TAG, "init write failed");
            i2c_master_bus_rm_device(s_dev);
            s_dev = NULL;
            return ESP_FAIL;
        }
    }

    s_ready = true;
    memset(s_fb, 0, sizeof(s_fb));
    display_flush();
    ESP_LOGI(TAG, "SSD1306 %dx%d at 0x%02x", W, s_h, addr);
    return ESP_OK;
}

bool display_ready(void) { return s_ready; }

esp_err_t display_flush(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    int pages = s_h / 8;

    if (cmd(0x21) || cmd(0) || cmd(W - 1)) return ESP_FAIL;
    if (cmd(0x22) || cmd(0) || cmd(pages - 1)) return ESP_FAIL;

    for (int p = 0; p < pages; p++) {
        uint8_t buf[1 + W];
        buf[0] = 0x40;
        memcpy(buf + 1, s_fb + p * W, W);
        if (i2c_master_transmit(s_dev, buf, sizeof(buf), 300) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

void display_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void display_text(int row, const char *text)
{
    if (row < 0 || row >= s_h / 8 || !text) {
        return;
    }
    uint8_t *line = s_fb + row * W;
    memset(line, 0, W);

    int x = 0;
    for (const char *p = text; *p && x + 6 <= W; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c > 126) {
            c = '?';
        }
        memcpy(line + x, FONT[c - 32], 5);
        line[x + 5] = 0;
        x += 6;
    }
}

static int cmd_disp(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        sesame_printf(out, "display  %s\n", s_ready ? "ready" : "not found");
        sesame_printf(out, "size     %dx%d\n", W, s_h);
        sesame_printf(out, "address  0x%s\n", cfg_get("disp.addr", "3c"));
        if (!s_ready) {
            sesame_write(out,
                "\nAn SSD1306 OLED shares the camera's I2C bus — wire SDA to 4,\n"
                "SCL to 5, VCC to 3V3, GND to GND. No other pins needed.\n"
                "Then: disp init. For a 128x32 module: cfg set disp.h 32\n");
        }
        return 0;
    }

    if (strcmp(argv[1], "init") == 0) {
        esp_err_t err = display_init();
        sesame_printf(out, "%s\n", err == ESP_OK ? "ok"
                      : err == ESP_ERR_NOT_FOUND ? "no display on the bus"
                      : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }

    if (!s_ready && display_init() != ESP_OK) {
        sesame_write(out, "disp: no display (try 'disp status')\n");
        return 1;
    }

    if (strcmp(argv[1], "clear") == 0) {
        display_clear();
        display_flush();
        sesame_write(out, "cleared\n");
        return 0;
    }

    if (strcmp(argv[1], "text") == 0 && argc >= 3) {
        int row = atoi(argv[2]);
        char msg[64] = { 0 };
        for (int i = 3; i < argc; i++) {
            if (i > 3) strncat(msg, " ", sizeof(msg) - strlen(msg) - 1);
            strncat(msg, argv[i], sizeof(msg) - strlen(msg) - 1);
        }
        display_text(row, msg);
        display_flush();
        sesame_printf(out, "row %d: %s\n", row, msg);
        return 0;
    }

    sesame_write(out, "usage: disp [status|init|clear|text <row> <message>]\n");
    return 1;
}

static const sesame_cmd_t CMDS[] = {
    { "disp", "disp [status|init|clear|text <row> <msg>]",
      "SSD1306 OLED on the camera I2C bus", cmd_disp },
};

void cmd_disp_register(void)
{
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
}
