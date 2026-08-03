#include "net.h"
#include "cfg.h"
#include "sesame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/apps/netbiosns.h"
#include "mdns.h"
#include "nvs_flash.h"

static const char *TAG = "net";

#define MAX_RETRY 6

static net_state_t s_state = NET_DOWN;
static int         s_retry;
static uint8_t     s_bssid[6];
static bool        s_bssid_pinned;
static int         s_pin_fails;

#define MAX_PIN_FAILS 3
static char        s_ap_ssid[33];
static char        s_ap_pass[16];
static char        s_ssid[33];
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;

static void start_portal_mode(void);
static void start_mdns(void);
static void start_netbios(const char *name);

static void bring_up_services(void *arg)
{
    (void)arg;

    dns_hijack_stop();
    portal_stop();
    web_start();
    start_mdns();
    start_netbios(cfg_get(CFG_DEV_NAME, "sesame-agent"));
    sshd_start();

    vTaskDelete(NULL);
}

const char *net_ap_ssid(void) { return s_ap_ssid; }
const char *net_ap_pass(void) { return s_ap_pass; }
const char *net_ssid(void)    { return s_ssid; }
net_state_t net_state(void)   { return s_state; }

void net_ip(char *buf, size_t len)
{
    esp_netif_ip_info_t ip = { 0 };
    esp_netif_t *netif = (s_state == NET_PORTAL) ? s_ap_netif : s_sta_netif;

    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    } else {
        snprintf(buf, len, "0.0.0.0");
    }
}

int net_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_state == NET_ONLINE && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;

    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = data;

        if (s_state == NET_ONLINE) {

            ESP_LOGW(TAG, "link lost (reason %d), reconnecting", e->reason);
            s_state = NET_CONNECTING;
            s_retry = 0;

            if (s_bssid_pinned && ++s_pin_fails >= MAX_PIN_FAILS) {
                ESP_LOGW(TAG, "this AP dropped us %d times in a row, releasing "
                              "the BSSID pin to find another node", s_pin_fails);
                wifi_config_t sta = { 0 };
                esp_wifi_get_config(WIFI_IF_STA, &sta);
                sta.sta.bssid_set = false;
                esp_wifi_set_config(WIFI_IF_STA, &sta);
                s_bssid_pinned = false;
                s_pin_fails = 0;
            }
            esp_wifi_connect();
            return;
        }

        if (++s_retry <= MAX_RETRY) {
            ESP_LOGW(TAG, "join failed (reason %d), retry %d/%d",
                     e->reason, s_retry, MAX_RETRY);
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "cannot join '%s'; opening the setup portal",
                     cfg_get(CFG_WIFI_SSID, ""));
            start_portal_mode();
        }
        return;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;

    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_retry = 0;
        s_pin_fails = 0;
        s_state = NET_ONLINE;
        strlcpy(s_ssid, cfg_get(CFG_WIFI_SSID, ""), sizeof(s_ssid));
        ESP_LOGI(TAG, "online as " IPSTR " on '%s'", IP2STR(&e->ip_info.ip), s_ssid);

        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            wifi_config_t sta = { 0 };
            esp_wifi_get_config(WIFI_IF_STA, &sta);
            memcpy(sta.sta.bssid, ap.bssid, 6);
            sta.sta.bssid_set = true;
            esp_wifi_set_config(WIFI_IF_STA, &sta);
            memcpy(s_bssid, ap.bssid, 6);
            s_bssid_pinned = true;
            ESP_LOGI(TAG, "pinned to %02x:%02x:%02x:%02x:%02x:%02x (rssi %d dBm)",
                     ap.bssid[0], ap.bssid[1], ap.bssid[2],
                     ap.bssid[3], ap.bssid[4], ap.bssid[5], ap.rssi);
        }

        static bool started;
        if (!started) {
            started = true;
            xTaskCreate(bring_up_services, "netsvc", 12288, NULL, 5, NULL);
        }
    }
}

static void derive_ap_identity(void)
{
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", cfg_get(CFG_DEV_NAME, "sesame-agent"));
    s_ap_pass[0] = '\0';
}

// Windows does not resolve .local reliably, and neither do some Android
// browsers, so the same name is answered over NetBIOS as well. It is an old
// protocol and costs almost nothing: one UDP socket on port 137 answering a
// single name. Between mDNS, NetBIOS and the DHCP hostname above, the device
// is reachable by name from most clients without anyone reading an IP address.
static void start_netbios(const char *name)
{
    static bool started;
    if (started) {
        return;
    }
    started = true;
    netbiosns_init();
    // NetBIOS names are capped at 15 characters. A longer dev.name still works
    // over mDNS, it just gets truncated here.
    netbiosns_set_name(name);
    ESP_LOGI(TAG, "netbios up: http://%s", name);
}

static void start_mdns(void)
{
    static bool started;
    if (started) {
        return;
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init: %s (no .local name)", esp_err_to_name(err));
        return;
    }
    started = true;

    const char *name = cfg_get(CFG_DEV_NAME, "sesame-agent");
    err = mdns_hostname_set(name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns hostname: %s", esp_err_to_name(err));
    }
    mdns_instance_name_set("sesame agent");
    err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns service: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "mdns up: http://%s.local", name);
}

static void start_portal_mode(void)
{
    derive_ap_identity();

    wifi_config_t ap = { 0 };

    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len       = strlen(s_ap_ssid);
    ap.ap.channel        = 1;
    ap.ap.max_connection = 4;

    ap.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_dhcps_stop(s_ap_netif);
    char uri[] = "http://192.168.4.1";
    esp_err_t err = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_CAPTIVEPORTAL_URI,
                                           uri, strlen(uri));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dhcp option 114 unavailable (%s); relying on DNS",
                 esp_err_to_name(err));
    }
    esp_netif_dhcps_start(s_ap_netif);

    s_state = NET_PORTAL;
    strlcpy(s_ssid, s_ap_ssid, sizeof(s_ssid));

    dns_hijack_start();
    portal_start();
    start_mdns();
    start_netbios(s_ap_ssid);

    ESP_LOGI(TAG, "setup portal up: join '%s' (open network)", s_ap_ssid);
}

// A fixed address, when net.ip is set. Worth having because every way of
// reaching this device by name outside mDNS depends on the address holding
// still: a DNS A record, a bookmark, a firewall rule. DHCP hands out whatever
// it likes and the lease can move when the device reconnects to another mesh
// node. Leave net.ip empty for DHCP, which is the default and the right choice
// for most people.
static void apply_static_ip(void)
{
    const char *ip = cfg_get("net.ip", "");
    if (!ip[0]) {
        return;
    }

    esp_netif_ip_info_t info = { 0 };
    esp_netif_str_to_ip4(ip, &info.ip);
    esp_netif_str_to_ip4(cfg_get("net.gw", ""), &info.gw);
    esp_netif_str_to_ip4(cfg_get("net.mask", "255.255.255.0"), &info.netmask);

    // The DHCP client has to stop first, or it will overwrite this the moment
    // the lease arrives.
    esp_netif_dhcpc_stop(s_sta_netif);
    if (esp_netif_set_ip_info(s_sta_netif, &info) != ESP_OK) {
        ESP_LOGE(TAG, "static ip %s rejected; falling back to DHCP", ip);
        esp_netif_dhcpc_start(s_sta_netif);
        return;
    }

    // Without DHCP there is no DNS server either, so resolution would break
    // and the agent could not reach its API. The gateway is the safe guess.
    esp_netif_dns_info_t dns = { 0 };
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = info.gw.addr;
    esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);

    ESP_LOGI(TAG, "static ip %s gw %s", ip, cfg_get("net.gw", ""));
}

static void start_station_mode(void)
{
    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, cfg_get(CFG_WIFI_SSID, ""), sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, cfg_get(CFG_WIFI_PASS, ""), sizeof(sta.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    apply_static_ip();
    ESP_ERROR_CHECK(esp_wifi_start());

    s_state = NET_CONNECTING;
    s_retry = 0;

}

esp_err_t net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    // Claim the name in the DHCP request (option 12), before the interface is
    // ever brought up, because the hostname is only sent when the lease is
    // taken. Most home routers put that name into their own DNS, which is what
    // makes http://sesame-xxxx reachable with no .local suffix and no IP. It
    // depends on the router, so it is a bonus path rather than the main one.
    const char *host = cfg_get(CFG_DEV_NAME, "sesame-agent");
    esp_netif_set_hostname(s_sta_netif, host);
    esp_netif_set_hostname(s_ap_netif, host);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    if (cfg_provisioned()) {
        start_station_mode();
    } else {
        start_portal_mode();
    }
    return ESP_OK;
}

esp_err_t net_connect(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = cfg_set(CFG_WIFI_SSID, ssid);
    if (err != ESP_OK) {
        return err;
    }
    cfg_set(CFG_WIFI_PASS, pass ? pass : "");

    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, pass ? pass : "", sizeof(sta.sta.password));

    esp_wifi_disconnect();
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &sta));

    s_state = NET_CONNECTING;
    s_retry = 0;
    s_bssid_pinned = false;
    s_pin_fails = 0;
    return esp_wifi_connect();
}

char *net_scan_json(void)
{
    wifi_scan_config_t scan = { .show_hidden = false };
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) {
        return strdup("[]");
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        return strdup("[]");
    }
    if (n > 24) {
        n = 24;
    }

    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) {
        return strdup("[]");
    }
    esp_wifi_scan_get_ap_records(&n, recs);

    size_t cap = n * 96 + 8;
    char *json = malloc(cap);
    if (!json) {
        free(recs);
        return strdup("[]");
    }

    int p = snprintf(json, cap, "[");
    for (int i = 0; i < n; i++) {

        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp((char *)recs[i].ssid, (char *)recs[j].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup || recs[i].ssid[0] == '\0') {
            continue;
        }

        char esc[64];
        int e = 0;
        for (const char *c = (char *)recs[i].ssid; *c && e < (int)sizeof(esc) - 2; c++) {
            if (*c == '"' || *c == '\\') {
                esc[e++] = '\\';
            }
            esc[e++] = *c;
        }
        esc[e] = '\0';

        p += snprintf(json + p, cap - p, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"lock\":%s}",
                      p > 1 ? "," : "", esc, recs[i].rssi,
                      recs[i].authmode == WIFI_AUTH_OPEN ? "false" : "true");
    }
    snprintf(json + p, cap - p, "]");

    free(recs);
    return json;
}

static int cmd_wifi(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        static const char *NAMES[] = { "down", "connecting", "online", "portal" };
        char ip[16];
        net_ip(ip, sizeof(ip));

        sesame_printf(out, "state   %s\n", NAMES[s_state]);
        sesame_printf(out, "ssid    %s\n", s_ssid[0] ? s_ssid : "-");
        sesame_printf(out, "ip      %s\n", ip);
        if (s_state == NET_ONLINE) {
            sesame_printf(out, "rssi    %d dBm\n", net_rssi());
            sesame_printf(out, "names   http://%s.local  (mDNS: Apple, Linux, Windows 10+)\n",
                          cfg_get(CFG_DEV_NAME, "sesame-agent"));
            sesame_printf(out, "        http://%s        (NetBIOS, and DHCP-registered on\n"
                               "                              routers that publish lease names)\n",
                          cfg_get(CFG_DEV_NAME, "sesame-agent"));
            if (s_bssid_pinned) {
                sesame_printf(out, "ap      pinned to %02x:%02x:%02x:%02x:%02x:%02x "
                                   "(won't roam between mesh nodes unless it drops %d times)\n",
                              s_bssid[0], s_bssid[1], s_bssid[2],
                              s_bssid[3], s_bssid[4], s_bssid[5], MAX_PIN_FAILS);
            }
        }
        if (s_state == NET_PORTAL) {
            sesame_printf(out, "setup   join '%s' (open), then http://192.168.4.1\n", s_ap_ssid);
        }
        return 0;
    }

    if (strcmp(argv[1], "scan") == 0) {
        char *json = net_scan_json();
        sesame_printf(out, "%s\n", json);
        free(json);
        return 0;
    }

    if (strcmp(argv[1], "connect") == 0 && argc >= 3) {
        esp_err_t err = net_connect(argv[2], argc > 3 ? argv[3] : "");
        sesame_printf(out, "%s\n", err == ESP_OK ? "joining" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }

    if (strcmp(argv[1], "static") == 0) {
        if (argc < 4) {
            sesame_write(out, "usage: wifi static <ip> <gateway> [mask]\n"
                              "       wifi dhcp        (back to automatic)\n");
            return 1;
        }
        cfg_set("net.ip", argv[2]);
        cfg_set("net.gw", argv[3]);
        cfg_set("net.mask", argc > 4 ? argv[4] : "255.255.255.0");
        sesame_printf(out, "static %s via %s (reboot to apply)\n", argv[2], argv[3]);
        return 0;
    }

    if (strcmp(argv[1], "dhcp") == 0) {
        cfg_erase("net.ip");
        cfg_erase("net.gw");
        sesame_write(out, "back to DHCP (reboot to apply)\n");
        return 0;
    }

    if (strcmp(argv[1], "forget") == 0) {
        cfg_erase(CFG_WIFI_SSID);
        cfg_erase(CFG_WIFI_PASS);
        sesame_write(out, "forgotten; reboot to open the setup portal\n");
        return 0;
    }

    sesame_write(out, "usage: wifi [status|scan|connect <ssid> [pass]|static <ip> <gw>|dhcp|forget]\n");
    return 1;
}

static const sesame_cmd_t CMDS[] = {
    { "wifi", "wifi [status|scan|connect <ssid> [pass]|static <ip> <gw>|dhcp|forget]",
      "network status and joining", cmd_wifi },
};

void cmd_net_register(void)
{
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
}
