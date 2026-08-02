#include "net.h"
#include "cfg.h"
#include "sesame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "portal";
static httpd_handle_t s_server;

extern const uint8_t portal_html_start[] asm("_binary_portal_html_start");
extern const uint8_t portal_html_end[]   asm("_binary_portal_html_end");

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)portal_html_start,
                           portal_html_end - portal_html_start - 1);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    char *json = net_scan_json();
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t info_get(httpd_req_t *req)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    uint64_t total = 0, avail = 0;
    esp_vfs_fat_info(SESAME_MOUNT, &total, &avail);

    char body[224];
    snprintf(body, sizeof(body),
             "{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"psram\":%u,\"fs\":%u,\"host\":\"%s\"}",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(avail / 1024),
             cfg_get(CFG_DEV_NAME, "sesame-agent"));

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static void join_later(void *arg)
{
    char *ssid = arg;
    vTaskDelay(pdMS_TO_TICKS(1200));

    ESP_LOGI(TAG, "provisioned; joining '%s'", ssid);
    dns_hijack_stop();
    net_connect(ssid, cfg_get(CFG_WIFI_PASS, ""));

    free(ssid);
    vTaskDelete(NULL);
}

static esp_err_t save_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
        return ESP_FAIL;
    }

    char *body = malloc(req->content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short read");
            return ESP_FAIL;
        }
        got += n;
    }
    body[got] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }

    const cJSON *ssid  = cJSON_GetObjectItem(root, "ssid");
    const cJSON *pass  = cJSON_GetObjectItem(root, "pass");
    const cJSON *key   = cJSON_GetObjectItem(root, "key");
    const cJSON *url   = cJSON_GetObjectItem(root, "url");
    const cJSON *model = cJSON_GetObjectItem(root, "model");

    if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no network chosen");
        return ESP_FAIL;
    }

    cfg_set(CFG_WIFI_SSID, ssid->valuestring);
    cfg_set(CFG_WIFI_PASS, cJSON_IsString(pass) ? pass->valuestring : "");

    if (cJSON_IsString(key)   && key->valuestring[0])   cfg_set(CFG_API_KEY,   key->valuestring);
    if (cJSON_IsString(url)   && url->valuestring[0])   cfg_set(CFG_API_URL,   url->valuestring);
    if (cJSON_IsString(model) && model->valuestring[0]) cfg_set(CFG_API_MODEL, model->valuestring);

    char *ssid_copy = strdup(ssid->valuestring);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    if (ssid_copy) {
        xTaskCreate(join_later, "join", 4096, ssid_copy, 5, NULL);
    }
    return ESP_OK;
}

static esp_err_t redirect_to_portal(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t portal_start(void)
{
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 8;
    cfg.stack_size       = 8192;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",      .method = HTTP_GET,  .handler = root_get },
        { .uri = "/scan",  .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/info",  .method = HTTP_GET,  .handler = info_get },
        { .uri = "/save",  .method = HTTP_POST, .handler = save_post },
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, redirect_to_portal);

    ESP_LOGI(TAG, "portal listening on http://192.168.4.1/");
    return ESP_OK;
}

void portal_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
