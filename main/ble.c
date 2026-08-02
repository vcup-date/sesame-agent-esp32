#include "sesame.h"
#include "cfg.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "ble";

#define MAX_SEEN 40

typedef struct {
    uint8_t addr[6];
    int8_t  rssi;
    char    name[26];
    bool    connectable;
} seen_t;

static seen_t s_seen[MAX_SEEN];
static int    s_count;
static bool   s_ready;
static bool   s_scanning;
static SemaphoreHandle_t s_sync;

static void note(const struct ble_gap_disc_desc *d)
{

    int slot = -1;
    for (int i = 0; i < s_count; i++) {
        if (memcmp(s_seen[i].addr, d->addr.val, 6) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (s_count >= MAX_SEEN) {
            return;
        }
        slot = s_count++;
        memset(&s_seen[slot], 0, sizeof(seen_t));
        memcpy(s_seen[slot].addr, d->addr.val, 6);
    }

    seen_t *s = &s_seen[slot];
    if (d->rssi > s->rssi || s->rssi == 0) {
        s->rssi = d->rssi;
    }
    if (d->event_type == BLE_HCI_ADV_TYPE_ADV_IND ||
        d->event_type == BLE_HCI_ADV_TYPE_ADV_DIRECT_IND_HD) {
        s->connectable = true;
    }

    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0 &&
        f.name != NULL && f.name_len > 0 && s->name[0] == '\0') {
        int n = f.name_len < (int)sizeof(s->name) - 1 ? f.name_len : (int)sizeof(s->name) - 1;
        memcpy(s->name, f.name, n);
        s->name[n] = '\0';
        for (int i = 0; i < n; i++) {
            if (s->name[i] < 32 || s->name[i] > 126) {
                s->name[i] = '.';
            }
        }
    }
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    if (ev->type == BLE_GAP_EVENT_DISC) {
        note(&ev->disc);
    } else if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        s_scanning = false;
    }
    return 0;
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    s_ready = true;
    if (s_sync) {
        xSemaphoreGive(s_sync);
    }
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t ble_bring_up(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (!s_sync) {
        s_sync = xSemaphoreCreateBinary();
    }

    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);

    if (xSemaphoreTake(s_sync, pdMS_TO_TICKS(4000)) != pdTRUE) {
        ESP_LOGE(TAG, "controller did not sync");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "BLE up, %u KB internal RAM taken",
             (unsigned)((before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) / 1024));
    return ESP_OK;
}

static void ble_shutdown(void)
{
    if (!s_ready) {
        return;
    }
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (nimble_port_stop() == 0) {
        nimble_port_deinit();
    }
    s_ready = false;
    ESP_LOGI(TAG, "BLE down, %u KB internal RAM returned",
             (unsigned)((heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - before) / 1024));
}

static bool s_advertising;

static int ble_adv_start(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    const char *name = cfg_get(CFG_DEV_NAME, "sesame");
    fields.name = (const uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        return rc;
    }

    struct ble_gap_adv_params adv = {
        .conn_mode = BLE_GAP_CONN_MODE_NON,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    return ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                             &adv, gap_event, NULL);
}

static int cmd_ble(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        sesame_printf(out, "stack    %s\n", s_ready ? "up" : "not started (starts on first use)");
        sesame_printf(out, "role     observer + broadcaster (scans and advertises;\n"
                           "         cannot be connected to, so nothing can be asked of it)\n");
        sesame_printf(out, "radio    BLE only — the ESP32-S3 has no classic Bluetooth\n");
        sesame_printf(out, "name     %s\n", cfg_get(CFG_DEV_NAME, "sesame"));
        sesame_printf(out, "adv      %s\n", s_advertising
                      ? "on — discoverable, and holding ~60KB of internal RAM"
                      : "off");
        if (s_ready) {
            sesame_printf(out, "last scan %d device%s\n", s_count, s_count == 1 ? "" : "s");
        }
        sesame_write(out, "usage: ble scan [seconds] | ble adv on|off\n");
        return 0;
    }

    if (strcmp(argv[1], "adv") == 0) {
        bool on = argc > 2 && strcmp(argv[2], "on") == 0;
        if (!on) {
            if (s_advertising) {
                ble_gap_adv_stop();
                s_advertising = false;
                ble_shutdown();
            }
            sesame_write(out, "advertising off\n");
            return 0;
        }
        if (ble_bring_up() != ESP_OK) {
            sesame_write(out, "ble: could not start the stack\n");
            return 1;
        }
        int rc = ble_adv_start();
        if (rc != 0) {
            sesame_printf(out, "ble: could not start advertising (rc=%d)\n", rc);
            return 1;
        }
        s_advertising = true;
        sesame_printf(out, "advertising as '%s' — visible to any BLE scan\n"
                           "(stays on, holding ~60KB internal RAM, until 'ble adv off')\n",
                      cfg_get(CFG_DEV_NAME, "sesame"));
        return 0;
    }

    if (strcmp(argv[1], "scan") != 0) {
        sesame_write(out, "usage: ble [status|scan [seconds]|adv on|off]\n");
        return 1;
    }

    int secs = argc > 2 ? atoi(argv[2]) : 4;
    if (secs < 1)  secs = 1;
    if (secs > 20) secs = 20;

    if (ble_bring_up() != ESP_OK) {
        sesame_write(out, "ble: could not start the stack (not enough internal RAM?)\n");
        return 1;
    }

    s_count = 0;
    memset(s_seen, 0, sizeof(s_seen));

    struct ble_gap_disc_params p = {
        .itvl = 0, .window = 0,
        .filter_policy = 0,
        .limited = 0,
        .passive = 1,
        .filter_duplicates = 0
    };

    s_scanning = true;
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, secs * 1000, &p, gap_event, NULL);
    if (rc != 0) {
        s_scanning = false;
        sesame_printf(out, "ble: scan failed (rc=%d)\n", rc);
        return 1;
    }

    int waited = 0;
    while (s_scanning && waited < (secs + 2) * 1000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    ble_gap_disc_cancel();

    if (!s_advertising) {
        ble_shutdown();
    }

    if (s_count == 0) {
        sesame_printf(out, "no BLE devices in %d s\n", secs);
        return 0;
    }

    for (int i = 0; i < s_count; i++) {
        for (int j = i + 1; j < s_count; j++) {
            if (s_seen[j].rssi > s_seen[i].rssi) {
                seen_t t = s_seen[i]; s_seen[i] = s_seen[j]; s_seen[j] = t;
            }
        }
    }

    sesame_printf(out, "%-17s %5s  %-4s %s\n", "address", "rssi", "conn", "name");
    for (int i = 0; i < s_count; i++) {
        seen_t *s = &s_seen[i];
        sesame_printf(out, "%02x:%02x:%02x:%02x:%02x:%02x  %4d  %-4s %s\n",
                      s->addr[5], s->addr[4], s->addr[3],
                      s->addr[2], s->addr[1], s->addr[0],
                      s->rssi, s->connectable ? "yes" : "no",
                      s->name[0] ? s->name : "(no name)");
    }
    sesame_printf(out, "\n%d device%s in %d s\n", s_count, s_count == 1 ? "" : "s", secs);
    return 0;
}

static const sesame_cmd_t CMDS[] = {
    { "ble", "ble [status|scan [seconds]|adv on|off]",
      "scan for nearby BLE devices, or advertise this one", cmd_ble },
};

void cmd_ble_register(void)
{
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
}
