#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define CFG_WIFI_SSID  "wifi.ssid"
#define CFG_WIFI_PASS  "wifi.pass"
#define CFG_API_KEY    "api.key"
#define CFG_API_URL    "api.url"
#define CFG_API_MODEL  "api.model"
#define CFG_SSH_USER   "ssh.user"
#define CFG_SSH_PASS   "ssh.pass"
#define CFG_DEV_NAME   "dev.name"
#define CFG_LED_PIN    "led.pin"
#define CFG_LED_INV    "led.inv"

esp_err_t   cfg_init(void);
const char *cfg_get(const char *key, const char *dflt);
esp_err_t   cfg_set(const char *key, const char *val);
bool        cfg_has(const char *key);
esp_err_t   cfg_erase(const char *key);

void        cfg_set_ram(const char *key, const char *val);

bool        cfg_is_stored(const char *key);

bool cfg_provisioned(void);
