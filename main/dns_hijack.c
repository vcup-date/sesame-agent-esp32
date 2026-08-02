#include "net.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "dns";
static TaskHandle_t s_task;
static int s_sock = -1;

#define DNS_PORT     53
#define DNS_MAX_LEN  512

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

static uint8_t rx[DNS_MAX_LEN];
static uint8_t tx[DNS_MAX_LEN];

static void dns_task(void *arg)
{
    (void)arg;

    struct sockaddr_in server = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(DNS_PORT),
    };

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    if (bind(s_sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        ESP_LOGE(TAG, "bind: errno %d", errno);
        close(s_sock);
        s_sock = -1;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "hijacking DNS on port %d", DNS_PORT);

    while (1) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);

        int len = recvfrom(s_sock, rx, DNS_MAX_LEN, 0,
                           (struct sockaddr *)&client, &client_len);
        if (len < (int)sizeof(dns_header_t)) {
            if (len < 0) {
                break;
            }
            continue;
        }

        dns_header_t *hdr = (dns_header_t *)rx;
        if (ntohs(hdr->qd_count) < 1) {
            continue;
        }

        int q = sizeof(dns_header_t);
        while (q < len && rx[q] != 0) {
            q += rx[q] + 1;
        }
        q += 1 + 4;
        if (q > len) {
            continue;
        }

        memcpy(tx, rx, q);
        dns_header_t *out = (dns_header_t *)tx;
        out->flags    = htons(0x8180);
        out->an_count = htons(1);
        out->ns_count = 0;
        out->ar_count = 0;

        int p = q;
        tx[p++] = 0xC0;
        tx[p++] = 0x0C;
        tx[p++] = 0x00; tx[p++] = 0x01;
        tx[p++] = 0x00; tx[p++] = 0x01;
        tx[p++] = 0x00; tx[p++] = 0x00;
        tx[p++] = 0x00; tx[p++] = 0x3C;
        tx[p++] = 0x00; tx[p++] = 0x04;

        esp_netif_ip_info_t ip = { 0 };
        esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (ap) {
            esp_netif_get_ip_info(ap, &ip);
        }
        uint32_t addr = ip.ip.addr;
        memcpy(&tx[p], &addr, 4);
        p += 4;

        sendto(s_sock, tx, p, 0, (struct sockaddr *)&client, client_len);
    }

    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

void dns_hijack_start(void)
{
    if (s_task) {
        return;
    }
    xTaskCreate(dns_task, "dns_hijack", 4096, NULL, 4, &s_task);
}

void dns_hijack_stop(void)
{
    if (s_sock >= 0) {
        shutdown(s_sock, 0);
        close(s_sock);
        s_sock = -1;
    }
}
