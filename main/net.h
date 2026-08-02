#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"

typedef enum {
    NET_DOWN,
    NET_CONNECTING,
    NET_ONLINE,
    NET_PORTAL,
} net_state_t;

esp_err_t   net_start(void);
net_state_t net_state(void);

void net_ip(char *buf, size_t len);
int  net_rssi(void);
const char *net_ssid(void);

esp_err_t net_connect(const char *ssid, const char *pass);

const char *net_ap_ssid(void);
const char *net_ap_pass(void);

char *net_scan_json(void);

void cmd_net_register(void);

void dns_hijack_start(void);
void dns_hijack_stop(void);

esp_err_t portal_start(void);
void      portal_stop(void);

esp_err_t web_start(void);
void      web_stop(void);

esp_err_t sshd_start(void);
void      cmd_ssh_register(void);
