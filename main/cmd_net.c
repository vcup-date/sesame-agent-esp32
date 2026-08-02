#include "sesame.h"
#include "cfg.h"
#include "net.h"

#include <stdlib.h>
#include <string.h>

#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"

typedef struct {
    sesame_out_t     *out;
    SemaphoreHandle_t done;
    int sent, recv;
    uint32_t total_ms;
} ping_ctx_t;

static void on_ping_success(esp_ping_handle_t h, void *args)
{
    ping_ctx_t *c = args;
    uint32_t elapsed = 0, ttl = 0, seq = 0;
    ip_addr_t target;
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    esp_ping_get_profile(h, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    esp_ping_get_profile(h, ESP_PING_PROF_IPADDR, &target, sizeof(target));
    c->recv++;
    c->total_ms += elapsed;

    const ip_addr_t *tp = &target;
    sesame_printf(c->out, "  seq=%u  ttl=%u  %u ms  from %s\n",
                  (unsigned)seq, (unsigned)ttl, (unsigned)elapsed,
                  ipaddr_ntoa(tp));
}

static void on_ping_timeout(esp_ping_handle_t h, void *args)
{
    ping_ctx_t *c = args;
    uint32_t seq = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    sesame_printf(c->out, "  seq=%u  timeout\n", (unsigned)seq);
}

static void on_ping_end(esp_ping_handle_t h, void *args)
{
    ping_ctx_t *c = args;
    esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &c->sent, sizeof(c->sent));
    xSemaphoreGive(c->done);
}

static int cmd_ping(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) {
        sesame_write(out, "usage: ping <host> [count]\n");
        return 1;
    }
    if (net_state() != NET_ONLINE) {
        sesame_write(out, "ping: not on a network\n");
        return 1;
    }

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(argv[1], NULL, &hints, &res) != 0 || !res) {
        sesame_printf(out, "ping: cannot resolve %s\n", argv[1]);
        return 1;
    }
    ip_addr_t target = { 0 };
    ip_addr_t *tgt = &target;
    struct in_addr a = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    ip_addr_set_ip4_u32(tgt, a.s_addr);

    int count = argc > 2 ? atoi(argv[2]) : 4;
    if (count < 1)  count = 1;
    if (count > 10) count = 10;

    ping_ctx_t ctx = { .out = out, .done = xSemaphoreCreateBinary() };
    if (!ctx.done) return 1;

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = count;
    cfg.timeout_ms = 1500;
    cfg.interval_ms = 500;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end     = on_ping_end,
        .cb_args         = &ctx,
    };
    esp_ping_handle_t h = NULL;
    if (esp_ping_new_session(&cfg, &cbs, &h) != ESP_OK) {
        vSemaphoreDelete(ctx.done);
        sesame_write(out, "ping: could not open an ICMP session\n");
        return 1;
    }

    const ip_addr_t *tptr = &target;
    sesame_printf(out, "PING %s (%s)\n", argv[1], ipaddr_ntoa(tptr));
    esp_ping_start(h);
    xSemaphoreTake(ctx.done, pdMS_TO_TICKS((count + 2) * 2500));
    esp_ping_stop(h);
    esp_ping_delete_session(h);
    vSemaphoreDelete(ctx.done);

    sesame_printf(out, "%d sent, %d received, %d%% loss%s\n",
                  ctx.sent, ctx.recv,
                  ctx.sent ? (ctx.sent - ctx.recv) * 100 / ctx.sent : 100,
                  ctx.recv ? "" : " — host unreachable");
    if (ctx.recv) {
        sesame_printf(out, "average %u ms\n", (unsigned)(ctx.total_ms / ctx.recv));
    }
    return ctx.recv ? 0 : 1;
}

static int cmd_resolve(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) { sesame_write(out, "usage: resolve <hostname>\n"); return 1; }

    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(argv[1], NULL, &hints, &res);
    if (rc != 0 || !res) {
        sesame_printf(out, "resolve: %s not found (rc=%d)\n", argv[1], rc);
        return 1;
    }
    int n = 0;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        char buf[64] = "?";
        if (p->ai_family == AF_INET) {
            struct in_addr a = ((struct sockaddr_in *)p->ai_addr)->sin_addr;
            inet_ntoa_r(a, buf, sizeof(buf));
        }
        sesame_printf(out, "%s\n", buf);
        n++;
    }
    freeaddrinfo(res);

    for (int i = 0; i < 2; i++) {
        const ip_addr_t *s = dns_getserver(i);
        if (s && !ip_addr_isany(s)) {
            sesame_printf(out, "(dns server %d: %s)\n", i, ipaddr_ntoa(s));
        }
    }
    return n ? 0 : 1;
}

static int cmd_netstat(int argc, char **argv, sesame_out_t *out)
{
    (void)argc; (void)argv;

    sesame_write(out, "interfaces\n");
    esp_netif_t *it = NULL;
    while ((it = esp_netif_next_unsafe(it)) != NULL) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(it, &ip) != ESP_OK) continue;
        const char *key = esp_netif_get_ifkey(it);
        sesame_printf(out, "  %-12s %s  ip " IPSTR "  mask " IPSTR "  gw " IPSTR "\n",
                      key ? key : "?",
                      esp_netif_is_netif_up(it) ? "up  " : "down",
                      IP2STR(&ip.ip), IP2STR(&ip.netmask), IP2STR(&ip.gw));
    }

    sesame_write(out, "\ndns\n");
    for (int i = 0; i < 2; i++) {
        const ip_addr_t *s = dns_getserver(i);
        if (s && !ip_addr_isany(s)) sesame_printf(out, "  %s\n", ipaddr_ntoa(s));
    }

    sesame_write(out, "\nlistening\n");
    sesame_write(out, "  tcp  80   web interface\n");
    sesame_write(out, "  tcp  81   control (status, stop)\n");
    sesame_printf(out, "  tcp  22   ssh%s\n",
                  cfg_has(CFG_SSH_PASS) ? "" : " (disabled, no password set)");
    sesame_write(out, "  udp  5353 mdns\n");
    if (net_state() == NET_PORTAL) {
        sesame_write(out, "  udp  53   dns hijack (setup portal)\n");
        sesame_write(out, "  udp  67   dhcp server (setup portal)\n");
    }

    if (net_state() == NET_ONLINE) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            sesame_printf(out, "\nwifi\n  ssid %s  rssi %d dBm  channel %d\n",
                          (char *)ap.ssid, ap.rssi, ap.primary);
            sesame_printf(out, "  bssid %02x:%02x:%02x:%02x:%02x:%02x\n",
                          ap.bssid[0], ap.bssid[1], ap.bssid[2],
                          ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        }
    }
    sesame_write(out, "\n(lwIP exposes no per-socket table, so open connections "
                      "cannot be listed the way netstat would on Linux)\n");
    return 0;
}

static const sesame_cmd_t CMDS[] = {
    { "ping",    "ping <host> [count]", "ICMP echo, with loss and latency",   cmd_ping },
    { "resolve", "resolve <hostname>",  "look up a hostname over DNS",        cmd_resolve },
    { "netstat", "netstat",             "interfaces, addresses and open ports", cmd_netstat },
};

void cmd_netcmds_register(void)
{
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
}
