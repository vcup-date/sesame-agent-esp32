#include "agent.h"
#include "camera.h"
#include "cfg.h"
#include "net.h"
#include "sesame.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"

static const char *TAG = "web";
static httpd_handle_t s_server;

static httpd_handle_t s_ctrl;
#define CTRL_PORT 81

static void allow_cors(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start - 1);
}

static void fmt_uptime(char *buf, size_t n)
{
    int64_t s = esp_timer_get_time() / 1000000;
    int d = (int)(s / 86400), h = (int)((s % 86400) / 3600), m = (int)((s % 3600) / 60);
    if (d)      snprintf(buf, n, "%dd %dh", d, h);
    else if (h) snprintf(buf, n, "%dh %dm", h, m);
    else        snprintf(buf, n, "%dm", m);
}

static esp_err_t status_get(httpd_req_t *req)
{
    static const char *LINK[] = { "offline", "connecting", "online", "portal" };

    char ip[16], up[24];
    net_ip(ip, sizeof(ip));
    fmt_uptime(up, sizeof(up));

    uint64_t fs_total = 0, fs_free = 0;
    esp_vfs_fat_info(SESAME_MOUNT, &fs_total, &fs_free);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "link", LINK[net_state()]);
    cJSON_AddStringToObject(j, "ssid", net_ssid());
    cJSON_AddStringToObject(j, "ip", ip);
    cJSON_AddNumberToObject(j, "rssi", net_rssi());
    cJSON_AddStringToObject(j, "uptime", up);
    cJSON_AddNumberToObject(j, "psram_free",  heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(j, "psram_total", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(j, "sram_free",   heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(j, "fs_free",  (double)fs_free);
    cJSON_AddNumberToObject(j, "fs_total", (double)fs_total);
    cJSON_AddBoolToObject(j, "camera", camera_ready());
    cJSON_AddNumberToObject(j, "commands", sesame_cmd_count());
    cJSON_AddNumberToObject(j, "context", agent_context_chars());
    cJSON_AddStringToObject(j, "model", cfg_get(CFG_API_MODEL, ""));
    cJSON_AddStringToObject(j, "url",   cfg_get(CFG_API_URL, ""));
    cJSON_AddStringToObject(j, "host",  cfg_get(CFG_DEV_NAME, "sesame-agent"));
    cJSON_AddBoolToObject(j, "key", cfg_has(CFG_API_KEY));

    cJSON_AddBoolToObject(j, "busy", agent_busy());

    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);

    allow_cors(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body ? body : "{}");
    free(body);
    return err;
}

typedef struct {
    httpd_req_t *req;
    bool         dead;
} stream_t;

static void stream_emit(void *ctx, agent_ev_t ev, const char *text)
{
    stream_t *s = ctx;
    if (s->dead) {
        return;
    }

    const char *kind = ev == AGENT_TEXT        ? "text"
                     : ev == AGENT_TOOL_CALL   ? "call"
                     : ev == AGENT_TOOL_RESULT ? "result"
                     : ev == AGENT_ERROR       ? "error"
                     : NULL;
    if (!kind || !text) {
        return;
    }

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "t", kind);
    cJSON_AddStringToObject(j, "v", text);
    char *line = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!line) {
        return;
    }

    if (httpd_resp_send_chunk(s->req, line, strlen(line)) != ESP_OK ||
        httpd_resp_send_chunk(s->req, "\n", 1) != ESP_OK) {
        s->dead = true;
    }
    free(line);
}

static esp_err_t chat_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return ESP_FAIL;
    }

    char *body = malloc(req->content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }

    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += n;
    }
    body[got] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    cJSON *msg = root ? cJSON_GetObjectItem(root, "message") : NULL;
    if (!cJSON_IsString(msg)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no message");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/x-ndjson");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    stream_t s = { .req = req, .dead = false };
    agent_run(msg->valuestring, stream_emit, &s);
    cJSON_Delete(root);

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t reset_post(httpd_req_t *req)
{
    agent_reset();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t exec_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 8192) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return ESP_FAIL;
    }

    char *body = malloc(req->content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) { free(body); return ESP_FAIL; }
        got += n;
    }
    body[got] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    cJSON *cmd = root ? cJSON_GetObjectItem(root, "command") : NULL;
    if (!cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no command");
        return ESP_FAIL;
    }

    int rc = 0;
    char *out = sesame_capture(cmd->valuestring, &rc);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t err = httpd_resp_sendstr(req, out ? out : "");
    free(out);
    return err;
}

static cJSON *read_json(httpd_req_t *req, size_t limit)
{
    if (req->content_len <= 0 || (size_t)req->content_len > limit) {
        return NULL;
    }
    char *body = malloc(req->content_len + 1);
    if (!body) {
        return NULL;
    }
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) { free(body); return NULL; }
        got += n;
    }
    body[got] = '\0';
    cJSON *j = cJSON_Parse(body);
    free(body);
    return j;
}

static bool safe_path(char *path)
{
    return strncmp(path, SESAME_MOUNT "/", strlen(SESAME_MOUNT) + 1) == 0
           && strstr(path, "..") == NULL;
}

static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    char *json = net_scan_json();
    allow_cors(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json ? json : "[]");
    free(json);
    return err;
}

static void join_task(void *arg)
{
    char *ssid = arg;
    vTaskDelay(pdMS_TO_TICKS(1200));
    net_connect(ssid, cfg_get(CFG_WIFI_PASS, ""));
    free(ssid);
    vTaskDelete(NULL);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    cJSON *j = read_json(req, 1024);
    cJSON *ssid = j ? cJSON_GetObjectItem(j, "ssid") : NULL;
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) {
        cJSON_Delete(j);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no ssid");
        return ESP_FAIL;
    }
    cJSON *pass = cJSON_GetObjectItem(j, "pass");
    cfg_set(CFG_WIFI_SSID, ssid->valuestring);
    cfg_set(CFG_WIFI_PASS, cJSON_IsString(pass) ? pass->valuestring : "");

    char *copy = strdup(ssid->valuestring);
    cJSON_Delete(j);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    if (copy) {
        xTaskCreate(join_task, "join", 4096, copy, 5, NULL);
    }
    return ESP_OK;
}

static esp_err_t rm_post(httpd_req_t *req)
{
    cJSON *j = read_json(req, 1024);
    cJSON *p = j ? cJSON_GetObjectItem(j, "path") : NULL;
    if (!cJSON_IsString(p)) {
        cJSON_Delete(j);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no path");
        return ESP_FAIL;
    }

    char path[288];
    snprintf(path, sizeof(path), "%s", p->valuestring);
    cJSON_Delete(j);

    if (!safe_path(path)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "outside the working directory");
        return ESP_FAIL;
    }
    if (unlink(path) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such file");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

#define PROFILES_PATH SESAME_MOUNT "/profiles.json"

static esp_err_t profiles_get(httpd_req_t *req)
{
    allow_cors(req);
    httpd_resp_set_type(req, "application/json");

    FILE *f = fopen(PROFILES_PATH, "rb");
    if (!f) {
        return httpd_resp_sendstr(req, "[]");
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 8192) {
        fclose(f);
        return httpd_resp_sendstr(req, "[]");
    }
    char *buf = malloc(n + 1);
    if (!buf) {
        fclose(f);
        return httpd_resp_sendstr(req, "[]");
    }
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    buf[got] = '\0';
    esp_err_t err = httpd_resp_sendstr(req, buf);
    free(buf);
    return err;
}

static esp_err_t profiles_post(httpd_req_t *req)
{
    cJSON *j = read_json(req, 8192);
    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!text) {
        return ESP_FAIL;
    }

    FILE *f = fopen(PROFILES_PATH, "wb");
    if (!f) {
        free(text);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot write");
        return ESP_FAIL;
    }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
    free(text);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t stop_post(httpd_req_t *req)
{
    agent_stop();
    allow_cors(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static void list_dir(cJSON *arr, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char full[320];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);

        struct stat st;
        if (stat(full, &st) != 0 || S_ISDIR(st.st_mode)) {
            continue;
        }
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "name", e->d_name);
        cJSON_AddStringToObject(f, "path", full);
        cJSON_AddNumberToObject(f, "size", (double)st.st_size);
        cJSON_AddItemToArray(arr, f);
    }
    closedir(d);
}

static esp_err_t files_get(httpd_req_t *req)
{
    cJSON *arr = cJSON_CreateArray();
    list_dir(arr, SESAME_MOUNT "/photos");
    list_dir(arr, SESAME_MOUNT);

    char *body = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body ? body : "[]");
    free(body);
    return err;
}

static const char *mime_for(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return "application/octet-stream";
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, ".png"))  return "image/png";
    if (!strcasecmp(dot, ".json")) return "application/json";
    if (!strcasecmp(dot, ".txt") || !strcasecmp(dot, ".log")) return "text/plain; charset=utf-8";
    if (!strcasecmp(dot, ".csv"))  return "text/csv";
    if (!strcasecmp(dot, ".py"))   return "text/x-python";
    return "application/octet-stream";
}

static void url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (r[0] == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], '\0' };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else if (*r == '+') {
            *w++ = ' ';
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static esp_err_t file_get(httpd_req_t *req)
{
    char query[320], path[288];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "p", path, sizeof(path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no path");
        return ESP_FAIL;
    }
    url_decode(path);

    if (!safe_path(path)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "outside the working directory");
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such file");
        return ESP_FAIL;
    }

    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;

    char disp[320];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
    httpd_resp_set_type(req, mime_for(name));

    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t settings_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return ESP_FAIL;
    }

    char *body = malloc(req->content_len + 1);
    if (!body) {
        return ESP_FAIL;
    }
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) { free(body); return ESP_FAIL; }
        got += n;
    }
    body[got] = '\0';

    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }

    struct { const char *json, *key; } map[] = {
        { "url",   CFG_API_URL   },
        { "model", CFG_API_MODEL },
        { "name",  CFG_DEV_NAME  },
        { "key",   CFG_API_KEY   },
        { "ssh",   CFG_SSH_PASS  },
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        cJSON *v = cJSON_GetObjectItem(j, map[i].json);
        if (cJSON_IsString(v) && v->valuestring[0]) {
            cfg_set(map[i].key, v->valuestring);
        }
    }
    cJSON_Delete(j);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t web_start(void)
{
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 20;

    cfg.max_open_sockets = 5;

    cfg.stack_size       = 12288;
    cfg.recv_wait_timeout = 30;
    cfg.send_wait_timeout = 30;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = root_get },

        { .uri = "/api/status",   .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/files",    .method = HTTP_GET,  .handler = files_get },
        { .uri = "/file",         .method = HTTP_GET,  .handler = file_get },
        { .uri = "/api/chat",     .method = HTTP_POST, .handler = chat_post },
        { .uri = "/api/reset",    .method = HTTP_POST, .handler = reset_post },
        { .uri = "/api/stop",     .method = HTTP_POST, .handler = stop_post },
        { .uri = "/api/exec",     .method = HTTP_POST, .handler = exec_post },
        { .uri = "/api/rm",       .method = HTTP_POST, .handler = rm_post },
        { .uri = "/api/wifi",     .method = HTTP_POST, .handler = wifi_post },
        { .uri = "/api/wifi/scan",.method = HTTP_GET,  .handler = wifi_scan_get },
        { .uri = "/api/profiles", .method = HTTP_GET,  .handler = profiles_get },
        { .uri = "/api/profiles", .method = HTTP_POST, .handler = profiles_post },
        { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post },
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }

    httpd_config_t cc = HTTPD_DEFAULT_CONFIG();
    cc.server_port      = CTRL_PORT;
    cc.ctrl_port        = cfg.ctrl_port + 1;
    cc.lru_purge_enable = true;
    cc.max_uri_handlers = 4;
    cc.stack_size       = 4096;
    cc.max_open_sockets = 3;

    err = httpd_start(&s_ctrl, &cc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "control server on :%d failed (%s); stop will be "
                      "unavailable while a turn runs", CTRL_PORT, esp_err_to_name(err));
    } else {
        static const httpd_uri_t ctrl_routes[] = {
            { .uri = "/api/status", .method = HTTP_GET,  .handler = status_get },
            { .uri = "/api/stop",   .method = HTTP_POST, .handler = stop_post },
        };
        for (unsigned i = 0; i < sizeof(ctrl_routes) / sizeof(ctrl_routes[0]); i++) {
            httpd_register_uri_handler(s_ctrl, &ctrl_routes[i]);
        }
        ESP_LOGI(TAG, "control server up on :%d", CTRL_PORT);
    }

    ESP_LOGI(TAG, "web interface up");
    return ESP_OK;
}

void web_stop(void)
{
    if (s_ctrl) {
        httpd_stop(s_ctrl);
        s_ctrl = NULL;
    }
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
