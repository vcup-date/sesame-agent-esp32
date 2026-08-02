#include "sesame.h"
#include "camera.h"
#include "cfg.h"

#include <stdlib.h>
#include <string.h>

#define N_PINS 49

typedef enum { FREE, CAMERA, FLASH, PSRAM, CONSOLE, USB, STRAP, MISSING, INUSE } use_t;

static const char *LABEL[] = {
    [FREE]    = "free",
    [CAMERA]  = "camera",
    [FLASH]   = "spi flash",
    [PSRAM]   = "psram (octal)",
    [CONSOLE] = "uart console",
    [USB]     = "native usb",
    [STRAP]   = "strapping",
    [MISSING] = "not present",
    [INUSE]   = "in use",
};

static void build(use_t *u, const char **why)
{
    for (int i = 0; i < N_PINS; i++) {
        u[i] = FREE;
        why[i] = "";
    }

    for (int i = 22; i <= 25; i++) { u[i] = MISSING; }
    for (int i = 26; i <= 32; i++) { u[i] = FLASH;   why[i] = "do not touch"; }
    for (int i = 33; i <= 37; i++) { u[i] = PSRAM;   why[i] = "do not touch"; }

    u[43] = CONSOLE; why[43] = "TX";
    u[44] = CONSOLE; why[44] = "RX";
    u[19] = USB;     why[19] = "D-";
    u[20] = USB;     why[20] = "D+";

    u[0]  = STRAP; why[0]  = "boot button — usable with care";
    u[3]  = STRAP; why[3]  = "JTAG select — usable with care";
    u[45] = STRAP; why[45] = "VDD_SPI voltage — avoid";
    u[46] = STRAP; why[46] = "boot mode — avoid";

    static const char *CAM_NAME[] = { "pwdn", "reset", "xclk", "siod (i2c sda)",
        "sioc (i2c scl)", "d7", "d6", "d5", "d4", "d3", "d2", "d1", "d0",
        "vsync", "href", "pclk" };
    int cam[16];
    int n = camera_pins(cam, 16);
    for (int i = 0; i < n; i++) {
        if (cam[i] >= 0 && cam[i] < N_PINS) {
            u[cam[i]] = CAMERA;
            why[cam[i]] = CAM_NAME[i];
        }
    }

    int led = atoi(cfg_get(CFG_LED_PIN, "-1"));
    if (led >= 0 && led < N_PINS && u[led] == FREE) {
        u[led] = INUSE;
        why[led] = "status led";
    }
}

static int cmd_pins(int argc, char **argv, sesame_out_t *out)
{
    use_t u[N_PINS];
    const char *why[N_PINS];
    build(u, why);

    bool only_free = argc > 1 && strcmp(argv[1], "free") == 0;

    if (!only_free) {
        sesame_write(out, "gpio  use             note\n");
        for (int i = 0; i < N_PINS; i++) {
            if (u[i] == MISSING) {
                continue;
            }
            sesame_printf(out, "%4d  %-15s %s\n", i, LABEL[u[i]], why[i]);
        }
        sesame_write(out, "\n");
    }

    char freelist[160] = { 0 };
    int count = 0;
    for (int i = 0; i < N_PINS; i++) {
        if (u[i] == FREE) {
            char b[8];
            snprintf(b, sizeof(b), "%s%d", count ? " " : "", i);
            strncat(freelist, b, sizeof(freelist) - strlen(freelist) - 1);
            count++;
        }
    }
    sesame_printf(out, "free: %s  (%d pins)\n", freelist, count);
    sesame_write(out,
        "\nI2C devices need no free pins at all — a display or sensor shares the\n"
        "camera bus on sda=4 scl=5. See `disp` and `i2creg`.\n");
    return 0;
}

static const sesame_cmd_t CMDS[] = {
    { "pins", "pins [free]", "what every GPIO is used for, and which are free", cmd_pins },
};

void cmd_pins_register(void)
{
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
}
