#include "sesame.h"
#include "cfg.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static int cmd_help(int argc, char **argv, sesame_out_t *out)
{
    if (argc > 1) {
        const sesame_cmd_t *c = sesame_lookup(argv[1]);
        if (!c) {
            sesame_printf(out, "no such command: %s\n", argv[1]);
            return 1;
        }
        sesame_printf(out, "%s\n  %s\n", c->usage, c->help);
        return 0;
    }

    for (int i = 0; i < sesame_cmd_count(); i++) {
        const sesame_cmd_t *c = sesame_cmd_at(i);
        sesame_printf(out, "%-10s %s\n", c->name, c->help);
    }
    return 0;
}

static void fmt_uptime(char *buf, size_t n)
{
    int64_t s = esp_timer_get_time() / 1000000;
    int d = (int)(s / 86400);
    int h = (int)((s % 86400) / 3600);
    int m = (int)((s % 3600) / 60);

    if (d) {
        snprintf(buf, n, "%dd %dh %dm", d, h, m);
    } else if (h) {
        snprintf(buf, n, "%dh %dm", h, m);
    } else {
        snprintf(buf, n, "%dm %ds", m, (int)(s % 60));
    }
}

static const char *reset_reason_text(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power on";
    case ESP_RST_SW:        return "software restart (reboot command or OTA)";
    case ESP_RST_PANIC:     return "PANIC — a crash, exception or assert";
    case ESP_RST_INT_WDT:   return "interrupt watchdog — an ISR ran too long";
    case ESP_RST_TASK_WDT:  return "task watchdog — a task stopped yielding";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "BROWNOUT — supply voltage sagged "
                                   "(a motor or servo on the board's own rail?)";
    case ESP_RST_DEEPSLEEP: return "woke from deep sleep";
    case ESP_RST_EXT:       return "external reset pin";
    case ESP_RST_SDIO:      return "SDIO reset";
    case ESP_RST_USB:       return "USB peripheral reset";
    case ESP_RST_JTAG:      return "JTAG reset";
    default:                return "unknown";
    }
}

static int cmd_sysinfo(int argc, char **argv, sesame_out_t *out)
{
    (void)argc; (void)argv;

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const esp_app_desc_t *app = esp_app_get_description();

    char up[32];
    fmt_uptime(up, sizeof(up));

    size_t heap_int  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t heap_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    sesame_printf(out, "firmware   %s %s\n", app->project_name, app->version);
    sesame_printf(out, "idf        %s\n", app->idf_ver);
    sesame_printf(out, "chip       ESP32-S3 rev%d.%d, %d core\n",
                  chip.revision / 100, chip.revision % 100, chip.cores);
    sesame_printf(out, "uptime     %s\n", up);
    sesame_printf(out, "tasks      %u\n", (unsigned)uxTaskGetNumberOfTasks());

    sesame_printf(out, "last boot  %s\n", reset_reason_text());
    sesame_write(out, "\n");

    size_t sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    sesame_bar(out, "sram", sram_total - heap_int, sram_total, 24);
    sesame_bar(out, "psram", psram_total - heap_psram, psram_total, 24);

    uint64_t fs_total = 0, fs_free = 0;
    if (esp_vfs_fat_info(SESAME_MOUNT, &fs_total, &fs_free) == ESP_OK) {
        sesame_bar(out, "storage", fs_total - fs_free, fs_total, 24);
    }
    sesame_printf(out, "\n%9s %u KB free · psram %u KB free · fs %llu KB free\n",
                  "", (unsigned)(heap_int / 1024),
                  (unsigned)(heap_psram / 1024), fs_free / 1024);
    return 0;
}

static int cmd_free(int argc, char **argv, sesame_out_t *out)
{
    (void)argc; (void)argv;

    size_t si_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t si_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t sp_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t sp_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    sesame_bar(out, "sram", si_total - si_free, si_total, 28);
    sesame_printf(out, "%9s %u free, largest block %u\n", "",
                  (unsigned)si_free,
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    sesame_bar(out, "psram", sp_total - sp_free, sp_total, 28);
    sesame_printf(out, "%9s %u free, largest block %u\n", "",
                  (unsigned)sp_free,
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    return 0;
}

static int cmd_uptime(int argc, char **argv, sesame_out_t *out)
{
    (void)argc; (void)argv;
    char up[32];
    fmt_uptime(up, sizeof(up));
    sesame_printf(out, "%s\n", up);
    return 0;
}

static int cmd_reboot(int argc, char **argv, sesame_out_t *out)
{
    (void)argc; (void)argv;
    sesame_write(out, "rebooting\n");

    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return 0;
}

static int cmd_cfg(int argc, char **argv, sesame_out_t *out)
{
    static const char *KEYS[] = {
        CFG_WIFI_SSID, CFG_WIFI_PASS, CFG_API_KEY, CFG_API_URL,
        CFG_API_MODEL, CFG_SSH_USER, CFG_SSH_PASS, CFG_DEV_NAME,
        CFG_LED_PIN,   CFG_LED_INV,
    };
    static const int N = sizeof(KEYS) / sizeof(KEYS[0]);

    bool secret[] = { false, true, true, false, false, false, true, false,
                      false, false };

    if (argc == 1 || strcmp(argv[1], "list") == 0) {
        for (int i = 0; i < N; i++) {
            const char *v = cfg_get(KEYS[i], "");
            if (secret[i] && v[0]) {
                sesame_printf(out, "%-10s (set, hidden)\n", KEYS[i]);
            } else {
                sesame_printf(out, "%-10s %s\n", KEYS[i], v);
            }
        }
        return 0;
    }

    if (strcmp(argv[1], "get") == 0 && argc == 3) {
        sesame_printf(out, "%s\n", cfg_get(argv[2], ""));
        return 0;
    }

    if (strcmp(argv[1], "set") == 0 && argc == 4) {
        if (cfg_set(argv[2], argv[3]) != ESP_OK) {
            sesame_printf(out, "unknown key: %s\n", argv[2]);
            return 1;
        }
        sesame_write(out, "ok\n");
        return 0;
    }

    sesame_write(out, "usage: cfg [list|get <key>|set <key> <value>]\n");
    return 1;
}

static const sesame_cmd_t CMDS[] = {
    { "help",    "help [command]",        "list commands, or explain one",       cmd_help },
    { "sysinfo", "sysinfo",               "firmware, chip, uptime and memory",   cmd_sysinfo },
    { "free",    "free",                  "free heap in SRAM and PSRAM",         cmd_free },
    { "uptime",  "uptime",                "how long since boot",                 cmd_uptime },
    { "reboot",  "reboot",                "restart the device",                  cmd_reboot },
    { "cfg",     "cfg [list|get|set]",    "read and write stored settings",      cmd_cfg },
};

void cmd_sys_register(void)
{
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
}
