#include "cfg.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cfg";
static const char *NAMESPACE = "sesame";

#define CFG_VALUE_MAX 160

static struct {
    const char *key;
    const char *dflt;
    char        val[CFG_VALUE_MAX];
} s_cfg[] = {
    { CFG_WIFI_SSID, ""                         },
    { CFG_WIFI_PASS, ""                         },
    { CFG_API_KEY,   ""                         },
    { CFG_API_URL,   "https://api.deepseek.com" },
    { CFG_API_MODEL, "deepseek-v4-flash"        },
    { CFG_SSH_USER,  "sesame"                   },
    { CFG_SSH_PASS,  ""                         },
    { CFG_DEV_NAME,  "sesame-agent"             },

    { CFG_LED_PIN,   "48"                       },
    { CFG_LED_INV,   "0"                        },
    { "cam.pins",    ""                         },
};

static const int CFG_N = sizeof(s_cfg) / sizeof(s_cfg[0]);

static int find(const char *key)
{
    for (int i = 0; i < CFG_N; i++) {
        if (strcmp(s_cfg[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

esp_err_t cfg_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored config yet; running on defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    for (int i = 0; i < CFG_N; i++) {
        size_t len = sizeof(s_cfg[i].val);
        if (nvs_get_str(h, s_cfg[i].key, s_cfg[i].val, &len) != ESP_OK) {
            s_cfg[i].val[0] = '\0';
        }
    }
    nvs_close(h);
    return ESP_OK;
}

const char *cfg_get(const char *key, const char *dflt)
{
    int i = find(key);
    if (i < 0) {
        return dflt;
    }
    if (s_cfg[i].val[0]) {
        return s_cfg[i].val;
    }
    if (s_cfg[i].dflt && s_cfg[i].dflt[0]) {
        return s_cfg[i].dflt;
    }
    return dflt;
}

bool cfg_has(const char *key)
{
    const char *v = cfg_get(key, NULL);
    return v && v[0];
}

void cfg_set_ram(const char *key, const char *val)
{
    int i = find(key);
    if (i >= 0) {
        strlcpy(s_cfg[i].val, val, CFG_VALUE_MAX);
    }
}

bool cfg_is_stored(const char *key)
{
    int i = find(key);
    return i >= 0 && s_cfg[i].val[0] != '\0';
}

esp_err_t cfg_set(const char *key, const char *val)
{
    int i = find(key);
    if (i < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (strlen(val) >= CFG_VALUE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {

        strlcpy(s_cfg[i].val, val, CFG_VALUE_MAX);
    }
    return err;
}

esp_err_t cfg_erase(const char *key)
{
    int i = find(key);
    if (i < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, key);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_commit(h);
        err = ESP_OK;
    }
    nvs_close(h);

    s_cfg[i].val[0] = '\0';
    return err;
}

bool cfg_provisioned(void)
{
    return cfg_has(CFG_WIFI_SSID);
}
