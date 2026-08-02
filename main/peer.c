#include "sesame.h"
#include "cfg.h"
#include "net.h"

#include <stdlib.h>
#include <string.h>

#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "peer";

#define PEER_MAGIC   0x53734d65u
#define PKT_BEACON   0
#define PKT_MSG      1
#define MAX_PEERS    12
#define MAX_INBOX    16
#define PEER_NAME_MAX     24
#define PEER_TEXT_MAX     180
#define BEACON_MS    5000

#define PEER_TTL_MAX 4
#define SEEN_MAX     32
#define RELAY_Q_LEN  8

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  type;
    uint8_t  ttl;
    uint32_t id;
    char     from[PEER_NAME_MAX];
    char     text[PEER_TEXT_MAX];
} peer_pkt_t;

typedef struct {
    uint8_t  mac[6];
    char     name[PEER_NAME_MAX];
    int8_t   rssi;
    int64_t  last_us;
} peer_t;

typedef struct {
    char    from[PEER_NAME_MAX];
    char    text[PEER_TEXT_MAX];
    int8_t  rssi;
    int64_t at_us;
} inbox_t;

static const uint8_t BROADCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static bool             s_ready;
static SemaphoreHandle_t s_lock;
static peer_t           s_peers[MAX_PEERS];
static int              s_npeers;
static inbox_t          s_inbox[MAX_INBOX];
static int              s_in_head;
static int              s_in_count;
static uint32_t         s_rx_total;
static uint32_t         s_relayed;

static uint32_t s_seen[SEEN_MAX];
static int      s_seen_head;

static QueueHandle_t s_relayq;

static const char *my_name(void)
{
    return cfg_get(CFG_DEV_NAME, "sesame");
}

static bool seen_before(uint32_t id)
{
    for (int i = 0; i < SEEN_MAX; i++) {
        if (s_seen[i] == id) {
            return true;
        }
    }
    s_seen[s_seen_head] = id;
    s_seen_head = (s_seen_head + 1) % SEEN_MAX;
    return false;
}

static void note_peer(const uint8_t *mac, const char *name, int8_t rssi)
{
    int slot = -1;
    for (int i = 0; i < s_npeers; i++) {
        if (memcmp(s_peers[i].mac, mac, 6) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (s_npeers >= MAX_PEERS) {

            int oldest = 0;
            for (int i = 1; i < s_npeers; i++) {
                if (s_peers[i].last_us < s_peers[oldest].last_us) {
                    oldest = i;
                }
            }
            slot = oldest;
        } else {
            slot = s_npeers++;
        }
        memcpy(s_peers[slot].mac, mac, 6);
    }
    strlcpy(s_peers[slot].name, name, PEER_NAME_MAX);
    s_peers[slot].rssi = rssi;
    s_peers[slot].last_us = esp_timer_get_time();
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len != sizeof(peer_pkt_t)) {
        return;
    }
    peer_pkt_t pkt;
    memcpy(&pkt, data, sizeof(pkt));
    if (pkt.magic != PEER_MAGIC) {
        return;
    }
    pkt.from[PEER_NAME_MAX - 1] = '\0';
    pkt.text[PEER_TEXT_MAX - 1] = '\0';

    int8_t rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;

    if (strcmp(pkt.from, my_name()) == 0) {
        return;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    note_peer(info->src_addr, pkt.from, rssi);

    bool dup = pkt.id != 0 && seen_before(pkt.id);

    if (pkt.type == PKT_MSG && !dup) {
        s_rx_total++;
        inbox_t *e = &s_inbox[s_in_head];
        strlcpy(e->from, pkt.from, PEER_NAME_MAX);
        strlcpy(e->text, pkt.text, PEER_TEXT_MAX);
        e->rssi = rssi;
        e->at_us = esp_timer_get_time();
        s_in_head = (s_in_head + 1) % MAX_INBOX;
        if (s_in_count < MAX_INBOX) {
            s_in_count++;
        }
    }
    xSemaphoreGive(s_lock);

    if (pkt.type == PKT_MSG && !dup) {
        uint8_t ttl = pkt.ttl > PEER_TTL_MAX ? PEER_TTL_MAX : pkt.ttl;
        if (ttl > 1) {
            pkt.ttl = ttl - 1;

            xQueueSend(s_relayq, &pkt, 0);
        }
    }
}

static void relay_task(void *arg)
{
    (void)arg;
    peer_pkt_t pkt;
    for (;;) {
        if (xQueueReceive(s_relayq, &pkt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(10 + (esp_random() % 60)));
        esp_now_send(BROADCAST, (uint8_t *)&pkt, sizeof(pkt));
        s_relayed++;
    }
}

static void fill_pkt(peer_pkt_t *p, uint8_t type, const char *text)
{
    memset(p, 0, sizeof(*p));
    p->magic = PEER_MAGIC;
    p->type = type;

    p->ttl = (type == PKT_MSG) ? PEER_TTL_MAX : 1;
    p->id  = esp_random();
    strlcpy(p->from, my_name(), PEER_NAME_MAX);
    if (text) {
        strlcpy(p->text, text, PEER_TEXT_MAX);
    }

    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        seen_before(p->id);
        xSemaphoreGive(s_lock);
    }
}

static void beacon_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BEACON_MS));
        if (!s_ready) {
            continue;
        }
        peer_pkt_t p;
        fill_pkt(&p, PKT_BEACON, NULL);
        esp_now_send(BROADCAST, (uint8_t *)&p, sizeof(p));
    }
}

static esp_err_t peer_up(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }

    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init: %s", esp_err_to_name(err));
        return err;
    }
    esp_now_register_recv_cb(recv_cb);

    esp_now_peer_info_t bc = { 0 };
    memcpy(bc.peer_addr, BROADCAST, 6);
    bc.channel = 0;
    bc.ifidx = WIFI_IF_STA;
    bc.encrypt = false;
    esp_now_add_peer(&bc);

    if (!s_relayq) {
        s_relayq = xQueueCreate(RELAY_Q_LEN, sizeof(peer_pkt_t));
    }

    s_ready = true;
    xTaskCreate(beacon_task, "peerbeacon", 3072, NULL, 3, NULL);
    xTaskCreate(relay_task, "peerrelay", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "ESP-NOW up as '%s' (mesh, %d hops)", my_name(), PEER_TTL_MAX);
    return ESP_OK;
}

static bool resolve(const char *name, uint8_t *mac_out)
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_npeers; i++) {
        if (strcasecmp(s_peers[i].name, name) == 0) {
            memcpy(mac_out, s_peers[i].mac, 6);
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

static esp_err_t send_to(const uint8_t *mac, const peer_pkt_t *p)
{
    esp_now_peer_info_t info = { 0 };
    memcpy(info.peer_addr, mac, 6);
    info.channel = 0;
    info.ifidx = WIFI_IF_STA;
    info.encrypt = false;
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_add_peer(&info);
    }
    esp_err_t err = esp_now_send(mac, (const uint8_t *)p, sizeof(*p));
    esp_now_del_peer(mac);
    return err;
}

static void ago(int64_t then_us, char *buf, size_t n)
{
    int secs = (int)((esp_timer_get_time() - then_us) / 1000000);
    if (secs < 60) {
        snprintf(buf, n, "%ds ago", secs);
    } else {
        snprintf(buf, n, "%dm ago", secs / 60);
    }
}

static int cmd_peer(int argc, char **argv, sesame_out_t *out)
{
    if (net_state() != NET_ONLINE && net_state() != NET_PORTAL) {
        sesame_write(out, "peer: no radio up yet\n");
        return 1;
    }
    if (peer_up() != ESP_OK) {
        sesame_write(out, "peer: could not start ESP-NOW\n");
        return 1;
    }

    const char *sub = argc > 1 ? argv[1] : "status";

    if (strcmp(sub, "status") == 0) {
        uint8_t mac[6] = { 0 };
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        uint8_t ch = 0; wifi_second_chan_t sc;
        esp_wifi_get_channel(&ch, &sc);
        sesame_printf(out, "name    %s\n", my_name());
        sesame_printf(out, "mac     %02x:%02x:%02x:%02x:%02x:%02x\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        sesame_printf(out, "channel %d  (ESP-NOW only reaches peers on this channel)\n", ch);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        sesame_printf(out, "peers   %d seen\n", s_npeers);
        sesame_printf(out, "inbox   %d message%s (%u total)\n",
                      s_in_count, s_in_count == 1 ? "" : "s", s_rx_total);
        sesame_printf(out, "mesh    up to %d hops, %u relayed for others\n",
                      PEER_TTL_MAX, (unsigned)s_relayed);
        xSemaphoreGive(s_lock);
        sesame_write(out, "\nMessages carry no encryption — anything in radio range\n"
                          "can read them, and a name in a packet is a claim, not proof.\n");
        sesame_write(out, "usage: peer list | send <name|all> <text> | inbox | name [new]\n");
        return 0;
    }

    if (strcmp(sub, "list") == 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_npeers == 0) {
            xSemaphoreGive(s_lock);
            sesame_write(out, "no other agents heard yet (they beacon every 5s)\n");
            return 0;
        }
        sesame_printf(out, "%-20s %5s  %-9s %s\n", "name", "rssi", "seen", "mac");
        for (int i = 0; i < s_npeers; i++) {
            char age[16];
            ago(s_peers[i].last_us, age, sizeof(age));
            sesame_printf(out, "%-20s %4d  %-9s %02x:%02x:%02x:%02x:%02x:%02x\n",
                          s_peers[i].name, s_peers[i].rssi, age,
                          s_peers[i].mac[0], s_peers[i].mac[1], s_peers[i].mac[2],
                          s_peers[i].mac[3], s_peers[i].mac[4], s_peers[i].mac[5]);
        }
        xSemaphoreGive(s_lock);
        return 0;
    }

    if (strcmp(sub, "send") == 0) {
        if (argc < 4) {
            sesame_write(out, "usage: peer send <name|all> <text>\n");
            return 1;
        }

        char text[PEER_TEXT_MAX] = { 0 };
        for (int i = 3; i < argc; i++) {
            strncat(text, argv[i], sizeof(text) - strlen(text) - 2);
            if (i + 1 < argc) {
                strncat(text, " ", sizeof(text) - strlen(text) - 1);
            }
        }

        peer_pkt_t p;
        fill_pkt(&p, PKT_MSG, text);

        if (strcasecmp(argv[2], "all") == 0) {
            esp_err_t err = esp_now_send(BROADCAST, (uint8_t *)&p, sizeof(p));
            sesame_printf(out, "%s\n", err == ESP_OK ? "broadcast sent" : "send failed");
            return err == ESP_OK ? 0 : 1;
        }

        uint8_t mac[6];
        if (!resolve(argv[2], mac)) {
            sesame_printf(out, "peer: no agent named '%s' heard yet — try `peer list`\n", argv[2]);
            return 1;
        }
        esp_err_t err = send_to(mac, &p);
        sesame_printf(out, "%s\n", err == ESP_OK ? "sent" : "send failed");
        return err == ESP_OK ? 0 : 1;
    }

    if (strcmp(sub, "inbox") == 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_in_count == 0) {
            xSemaphoreGive(s_lock);
            sesame_write(out, "inbox empty\n");
            return 0;
        }

        int start = (s_in_head - s_in_count + MAX_INBOX) % MAX_INBOX;
        for (int k = 0; k < s_in_count; k++) {
            inbox_t *e = &s_inbox[(start + k) % MAX_INBOX];
            char age[16];
            ago(e->at_us, age, sizeof(age));
            sesame_printf(out, "%s (%s): %s\n", e->from, age, e->text);
        }
        bool clear = argc > 2 && strcmp(argv[2], "keep") != 0;
        if (clear) {
            s_in_count = 0;
        }
        xSemaphoreGive(s_lock);
        if (clear) {
            sesame_write(out, "(inbox cleared; `peer inbox keep` to leave them)\n");
        }
        return 0;
    }

    if (strcmp(sub, "name") == 0) {
        if (argc > 2) {
            esp_err_t err = cfg_set(CFG_DEV_NAME, argv[2]);
            sesame_printf(out, "%s\n", err == ESP_OK
                          ? "name set (reboot to update the .local hostname too)"
                          : "could not save the name");
            return err == ESP_OK ? 0 : 1;
        }
        sesame_printf(out, "%s\n", my_name());
        return 0;
    }

    sesame_write(out, "usage: peer [status|list|send <name|all> <text>|inbox [keep]|name [new]]\n");
    return 1;
}

static const sesame_cmd_t CMDS[] = {
    { "peer", "peer [status|list|send <name|all> <text>|inbox|name [new]]",
      "talk to other sesame agents nearby over ESP-NOW", cmd_peer },
};

void cmd_peer_register(void)
{
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
}
