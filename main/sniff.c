#include "sesame.h"
#include "net.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sniff";

#define MAX_SEEN 32

typedef struct {
    uint8_t mac[6];
    int     frames;
    int8_t  rssi;
} seen_t;

static seen_t   s_seen[MAX_SEEN];
static int      s_nseen;
static uint32_t s_mgmt, s_data, s_ctrl, s_misc;
static bool     s_running;

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_running) {
        return;
    }
    switch (type) {
    case WIFI_PKT_MGMT: s_mgmt++; break;
    case WIFI_PKT_DATA: s_data++; break;
    case WIFI_PKT_CTRL: s_ctrl++; return;
    default:            s_misc++; return;
    }

    const wifi_promiscuous_pkt_t *pkt = buf;
    if (pkt->rx_ctrl.sig_len < 16) {
        return;
    }

    const uint8_t *addr2 = pkt->payload + 10;

    for (int i = 0; i < s_nseen; i++) {
        if (memcmp(s_seen[i].mac, addr2, 6) == 0) {
            s_seen[i].frames++;
            if (pkt->rx_ctrl.rssi > s_seen[i].rssi) {
                s_seen[i].rssi = pkt->rx_ctrl.rssi;
            }
            return;
        }
    }
    if (s_nseen < MAX_SEEN) {
        memcpy(s_seen[s_nseen].mac, addr2, 6);
        s_seen[s_nseen].frames = 1;
        s_seen[s_nseen].rssi = pkt->rx_ctrl.rssi;
        s_nseen++;
    }
}

static int cmd_sniff(int argc, char **argv, sesame_out_t *out)
{
    if (argc > 1 && strcmp(argv[1], "help") == 0) {
        sesame_write(out,
            "usage: sniff [seconds] [channel]\n"
            "Counts frames heard on a channel: how busy the air actually is.\n"
            "Reads frame type and transmitter address only — no payloads are\n"
            "captured or stored. The radio leaves the network while this runs.\n");
        return 0;
    }

    if (net_state() != NET_ONLINE && net_state() != NET_PORTAL) {
        sesame_write(out, "sniff: the radio is not up\n");
        return 1;
    }

    int secs = argc > 1 ? atoi(argv[1]) : 5;
    if (secs < 1)  secs = 1;
    if (secs > 15) secs = 15;

    uint8_t home_ch = 0;
    wifi_second_chan_t home_sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&home_ch, &home_sec);

    int ch = argc > 2 ? atoi(argv[2]) : home_ch;
    if (ch < 1 || ch > 14) {
        ch = home_ch;
    }

    s_nseen = 0;
    s_mgmt = s_data = s_ctrl = s_misc = 0;
    memset(s_seen, 0, sizeof(s_seen));

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                       WIFI_PROMIS_FILTER_MASK_DATA |
                       WIFI_PROMIS_FILTER_MASK_CTRL,
    };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(promisc_cb);

    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
        sesame_write(out, "sniff: could not enter promiscuous mode\n");
        return 1;
    }
    if (ch != home_ch) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    }

    s_running = true;
    sesame_printf(out, "listening on channel %d for %d s...\n", ch, secs);
    vTaskDelay(pdMS_TO_TICKS(secs * 1000));
    s_running = false;

    esp_wifi_set_promiscuous(false);

    if (ch != home_ch) {
        esp_wifi_set_channel(home_ch, home_sec);
    }

    uint32_t total = s_mgmt + s_data + s_ctrl + s_misc;
    sesame_printf(out, "\n%u frames in %d s  (%u/s)\n",
                  (unsigned)total, secs, (unsigned)(total / secs));
    sesame_printf(out, "  mgmt %-6u data %-6u ctrl %-6u other %u\n",
                  (unsigned)s_mgmt, (unsigned)s_data,
                  (unsigned)s_ctrl, (unsigned)s_misc);

    if (s_nseen == 0) {
        sesame_write(out, "\nno transmitters identified\n");
        return 0;
    }

    for (int i = 0; i < s_nseen; i++) {
        for (int j = i + 1; j < s_nseen; j++) {
            if (s_seen[j].frames > s_seen[i].frames) {
                seen_t t = s_seen[i]; s_seen[i] = s_seen[j]; s_seen[j] = t;
            }
        }
    }

    sesame_printf(out, "\n%-18s %7s %5s\n", "transmitter", "frames", "rssi");
    int show = s_nseen > 12 ? 12 : s_nseen;
    for (int i = 0; i < show; i++) {
        sesame_printf(out, "%02x:%02x:%02x:%02x:%02x:%02x  %7d %5d\n",
                      s_seen[i].mac[0], s_seen[i].mac[1], s_seen[i].mac[2],
                      s_seen[i].mac[3], s_seen[i].mac[4], s_seen[i].mac[5],
                      s_seen[i].frames, s_seen[i].rssi);
    }
    if (s_nseen > show) {
        sesame_printf(out, "... and %d more\n", s_nseen - show);
    }
    if (s_nseen >= MAX_SEEN) {
        sesame_printf(out, "(table full at %d; busier than this is not counted)\n", MAX_SEEN);
    }
    return 0;
}

static const sesame_cmd_t CMDS[] = {
    { "sniff", "sniff [seconds] [channel]",
      "how busy the air is: passive frame counts, no payloads", cmd_sniff },
};

void cmd_sniff_register(void)
{
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
    ESP_LOGD(TAG, "sniff registered");
}
