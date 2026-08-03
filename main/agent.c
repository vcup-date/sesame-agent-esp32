#include "agent.h"
#include "cfg.h"
#include "led.h"
#include "net.h"
#include "py.h"
#include "sesame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "agent";

#define MAX_HOPS      12

#define MAX_RESPONSE  (96 * 1024)

#define MAX_CONTEXT   (48 * 1024)

static cJSON            *s_messages;
static SemaphoreHandle_t s_lock;
static int               s_turns;
static bool              s_busy;
static volatile bool     s_stop;

bool agent_busy(void)  { return s_busy; }
int  agent_turns(void) { return s_turns; }

typedef struct { char *buf; size_t len, cap; } pbuf_t;

static void pappend(pbuf_t *b, const char *fmt, ...)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
        va_end(ap);
        if (n < 0) {
            return;
        }
        if ((size_t)n < b->cap - b->len) {
            b->len += n;
            return;
        }
        size_t want = (b->len + n + 1) * 2;
        char *grown = realloc(b->buf, want);
        if (!grown) {
            b->buf[b->len] = '\0';
            return;
        }
        b->buf = grown;
        b->cap = want;
    }
}

static char *build_system_prompt(void)
{
    pbuf_t b = { .buf = malloc(1024), .len = 0, .cap = 1024 };
    if (!b.buf) {
        return NULL;
    }
    b.buf[0] = '\0';

    char ip[16];
    net_ip(ip, sizeof(ip));

    pappend(&b,
        "You are sesame, an agent that runs on an ESP32-S3 microcontroller and "
        "controls its own hardware.\n\n"
        "This is a real device, not a simulation. It has an OV2640 camera, a "
        "16MB flash with a FAT filesystem mounted at %s, 8MB of PSRAM, WiFi, and "
        "GPIO/ADC/PWM/I2C pins exposed on a header. Its address is %s.\n\n"
        "You act by calling the `shell` tool, which runs one command on the "
        "device and returns its output. These are the commands that exist:\n\n",
        SESAME_MOUNT, ip);

    for (int i = 0; i < sesame_cmd_count(); i++) {
        const sesame_cmd_t *c = sesame_cmd_at(i);
        pappend(&b, "  %-44s %s\n", c->usage, c->help);
    }

    pappend(&b,
        "\nThere is no shell beyond this table: no pipes, no globbing, no "
        "programs. One command per call, exactly as spelled above.\n\n"
        "Reply in whatever language the user writes to you in. Command names "
        "and device output stay as they are; only your own prose changes.\n\n"
        "When a task needs logic that no single command expresses — a loop, a "
        "calculation, a sequence with timing — write Python and run it with "
        "`py`. It is a real MicroPython interpreter on this chip. For anything "
        "longer than a line or two, `write` the script to a file first and then "
        "run `py -f <path>`, so it is saved and can be re-run.\n\n"
        "`write` and `py` take the whole rest of the line, newlines and quotes "
        "included, exactly as you send them — so send real multi-line text and "
        "do not try to escape it.\n\n"
        "Python here has no filesystem: there is no `open()`, no `os`, no "
        "`import` of anything you write. Use `write`, `cat` and `ls` for files "
        "and keep Python for computation. `time` is available.\n\n"
        "Python also has NO hardware access. There is no `machine`, no "
        "`neopixel`, no pin or bus objects, and importing them will fail. Every "
        "hardware action is a command in the table above, so reach for the "
        "command rather than testing whether a Python module exists.\n\n"
        "You do control the light: `rgb r g b` sets the colour LED and `led "
        "off|idle|think` drives the status animation. The board's red LED is "
        "wired straight to the power rail with no GPIO, and the blue one is the "
        "UART transmit line, so neither can be switched. Say so plainly rather "
        "than reporting that you have no access to the LEDs.\n\n"
        "Take photos with `snap`, which writes a JPEG and reports its path and "
        "size. You cannot see the image; describe what you did, not what is in "
        "the picture, unless the user tells you.\n\n"
        "Be brief. You are read on a serial terminal and a phone screen. When a "
        "command fails, say what failed and what you will try instead, rather "
        "than repeating the same call.");

    return b.buf;
}

int agent_prompt_bytes(void)
{
    char *p = build_system_prompt();
    int n = p ? (int)strlen(p) : 0;
    free(p);
    return n;
}

static cJSON *build_tools(void)
{
    cJSON *tools = cJSON_CreateArray();
    cJSON *tool  = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");

    cJSON *fn = cJSON_CreateObject();
    cJSON_AddStringToObject(fn, "name", "shell");
    cJSON_AddStringToObject(fn, "description",
        "Run one command on the device and return its output.");

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "type", "object");

    cJSON *props = cJSON_CreateObject();
    cJSON *cmd   = cJSON_CreateObject();
    cJSON_AddStringToObject(cmd, "type", "string");
    cJSON_AddStringToObject(cmd, "description",
        "The command line, e.g. 'snap' or 'gpio set 2 1'.");
    cJSON_AddItemToObject(props, "command", cmd);
    cJSON_AddItemToObject(params, "properties", props);

    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("command"));
    cJSON_AddItemToObject(params, "required", req);

    cJSON_AddItemToObject(fn, "parameters", params);
    cJSON_AddItemToObject(tool, "function", fn);
    cJSON_AddItemToArray(tools, tool);
    return tools;
}

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} resp_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    resp_t *r = evt->user_data;

    if (evt->event_id != HTTP_EVENT_ON_DATA || !r) {
        return ESP_OK;
    }
    if (r->len + evt->data_len + 1 > MAX_RESPONSE) {
        return ESP_OK;
    }
    if (r->len + evt->data_len + 1 > r->cap) {
        size_t want = (r->len + evt->data_len + 1) * 2;
        char *grown = heap_caps_realloc(r->buf, want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!grown) {
            grown = realloc(r->buf, want);
        }
        if (!grown) {
            return ESP_ERR_NO_MEM;
        }
        r->buf = grown;
        r->cap = want;
    }
    memcpy(r->buf + r->len, evt->data, evt->data_len);
    r->len += evt->data_len;
    r->buf[r->len] = '\0';
    return ESP_OK;
}

static cJSON *post_completion(const char *body, int *status, char **err_text)
{
    char url[192];
    const char *base = cfg_get(CFG_API_URL, "https://api.deepseek.com");

    size_t blen = strlen(base);
    bool has_slash = blen && base[blen - 1] == '/';
    snprintf(url, sizeof(url), "%s%schat/completions", base, has_slash ? "" : "/");

    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg_get(CFG_API_KEY, ""));

    resp_t resp = { 0 };
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .event_handler     = on_http_event,
        .user_data         = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 90000,
        .buffer_size       = 2048,
        .buffer_size_tx    = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        *err_text = strdup("could not create the HTTP client");
        return NULL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    *status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        char msg[128];
        snprintf(msg, sizeof(msg), "network: %s", esp_err_to_name(err));
        *err_text = strdup(msg);
        free(resp.buf);
        return NULL;
    }

    if (*status != 200) {

        char msg[320];
        const char *detail = resp.buf ? resp.buf : "(no body)";
        cJSON *j = resp.buf ? cJSON_Parse(resp.buf) : NULL;
        if (j) {
            cJSON *e = cJSON_GetObjectItem(j, "error");
            cJSON *m = e ? cJSON_GetObjectItem(e, "message") : NULL;
            if (cJSON_IsString(m)) {
                detail = m->valuestring;
            }
        }
        snprintf(msg, sizeof(msg), "HTTP %d: %.240s", *status, detail);
        *err_text = strdup(msg);
        if (j) {
            cJSON_Delete(j);
        }
        free(resp.buf);
        return NULL;
    }

    cJSON *parsed = resp.buf ? cJSON_Parse(resp.buf) : NULL;
    if (!parsed) {
        *err_text = strdup("could not parse the reply");
    }
    free(resp.buf);
    return parsed;
}

static int context_chars(void)
{
    if (!s_messages) {
        return 0;
    }
    char *s = cJSON_PrintUnformatted(s_messages);
    int n = s ? strlen(s) : 0;
    free(s);
    return n;
}

int agent_context_chars(void) { return context_chars(); }

static void trim_context(void)
{
    while (context_chars() > MAX_CONTEXT && cJSON_GetArraySize(s_messages) > 3) {

        cJSON_DeleteItemFromArray(s_messages, 1);
        for (;;) {
            cJSON *head = cJSON_GetArrayItem(s_messages, 1);
            cJSON *role = head ? cJSON_GetObjectItem(head, "role") : NULL;
            if (cJSON_IsString(role) && strcmp(role->valuestring, "tool") == 0) {
                cJSON_DeleteItemFromArray(s_messages, 1);
            } else {
                break;
            }
        }
    }
}

static void ensure_conversation(void)
{
    if (s_messages) {
        return;
    }
    s_messages = cJSON_CreateArray();

    char *sys = build_system_prompt();
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "system");
    cJSON_AddStringToObject(m, "content", sys ? sys : "You are sesame.");
    cJSON_AddItemToArray(s_messages, m);
    free(sys);
}

void agent_stop(void)
{
    s_stop = true;

    py_interrupt();
}

void agent_reset(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    cJSON_Delete(s_messages);
    s_messages = NULL;
    s_turns = 0;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void emit(agent_emit_fn fn, void *ctx, agent_ev_t ev, const char *text)
{

    if (ev == AGENT_ERROR) {
        led_set(LED_ERROR);
    } else if (ev == AGENT_DONE) {
        led_set(LED_OK);
    }
    if (fn) {
        fn(ctx, ev, text);
    }
}

esp_err_t agent_run(const char *user_msg, agent_emit_fn fn, void *ctx)
{
    if (!cfg_has(CFG_API_KEY)) {
        emit(fn, ctx, AGENT_ERROR, "No API key set. Use 'cfg set api.key <key>' "
                                   "or the web interface.");
        return ESP_ERR_INVALID_STATE;
    }
    if (net_state() != NET_ONLINE) {
        emit(fn, ctx, AGENT_ERROR, "Not on a network yet.");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        emit(fn, ctx, AGENT_ERROR, "Busy with another request.");
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = true;
    s_stop = false;
    led_set(LED_THINKING);

    ensure_conversation();

    cJSON *um = cJSON_CreateObject();
    cJSON_AddStringToObject(um, "role", "user");
    cJSON_AddStringToObject(um, "content", user_msg);
    cJSON_AddItemToArray(s_messages, um);

    esp_err_t result = ESP_OK;
    cJSON *tools = build_tools();

    for (int hop = 0; hop < MAX_HOPS; hop++) {
        if (s_stop) {
            emit(fn, ctx, AGENT_ERROR, "stopped");
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        trim_context();

        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "model", cfg_get(CFG_API_MODEL, "deepseek-v4-flash"));
        cJSON_AddItemReferenceToObject(req, "messages", s_messages);
        cJSON_AddItemReferenceToObject(req, "tools", tools);
        cJSON_AddStringToObject(req, "tool_choice", "auto");
        cJSON_AddNumberToObject(req, "max_tokens", 2048);

        char *body = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        if (!body) {
            emit(fn, ctx, AGENT_ERROR, "out of memory building the request");
            result = ESP_ERR_NO_MEM;
            break;
        }

        int status = 0;
        char *err_text = NULL;
        ESP_LOGI(TAG, "hop %d, %u bytes out", hop + 1, (unsigned)strlen(body));
        cJSON *reply = post_completion(body, &status, &err_text);
        free(body);

        if (!reply) {
            emit(fn, ctx, AGENT_ERROR, err_text ? err_text : "request failed");
            free(err_text);
            result = ESP_FAIL;
            break;
        }

        cJSON *choices = cJSON_GetObjectItem(reply, "choices");
        cJSON *choice  = cJSON_GetArrayItem(choices, 0);
        cJSON *message = choice ? cJSON_GetObjectItem(choice, "message") : NULL;
        if (!message) {
            emit(fn, ctx, AGENT_ERROR, "reply had no message");
            cJSON_Delete(reply);
            result = ESP_FAIL;
            break;
        }

        cJSON *content    = cJSON_GetObjectItem(message, "content");
        cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");

        if (cJSON_IsString(content) && content->valuestring[0]) {
            emit(fn, ctx, AGENT_TEXT, content->valuestring);
        }

        cJSON_AddItemToArray(s_messages, cJSON_Duplicate(message, true));

        if (!cJSON_IsArray(tool_calls) || cJSON_GetArraySize(tool_calls) == 0) {
            cJSON_Delete(reply);
            s_turns++;
            emit(fn, ctx, AGENT_DONE, NULL);
            break;
        }

        int ncalls = cJSON_GetArraySize(tool_calls);
        for (int i = 0; i < ncalls; i++) {
            cJSON *call = cJSON_GetArrayItem(tool_calls, i);
            cJSON *id   = cJSON_GetObjectItem(call, "id");
            cJSON *func = cJSON_GetObjectItem(call, "function");
            cJSON *args = func ? cJSON_GetObjectItem(func, "arguments") : NULL;

            char *output = NULL;
            if (s_stop) {

                output = strdup("stopped by the user");
            } else if (cJSON_IsString(args)) {

                cJSON *a = cJSON_Parse(args->valuestring);
                cJSON *c = a ? cJSON_GetObjectItem(a, "command") : NULL;
                if (cJSON_IsString(c)) {
                    emit(fn, ctx, AGENT_TOOL_CALL, c->valuestring);
                    int rc = 0;
                    output = sesame_capture(c->valuestring, &rc);
                    emit(fn, ctx, AGENT_TOOL_RESULT, output ? output : "");
                } else {
                    output = strdup("error: no 'command' in the arguments");
                    emit(fn, ctx, AGENT_TOOL_RESULT, output);
                }
                cJSON_Delete(a);
            } else {
                output = strdup("error: malformed tool call");
            }

            cJSON *tm = cJSON_CreateObject();
            cJSON_AddStringToObject(tm, "role", "tool");
            cJSON_AddStringToObject(tm, "tool_call_id",
                                    cJSON_IsString(id) ? id->valuestring : "");

            cJSON_AddStringToObject(tm, "content",
                                    (output && output[0]) ? output : "(no output)");
            cJSON_AddItemToArray(s_messages, tm);
            free(output);
        }

        cJSON_Delete(reply);

        if (hop == MAX_HOPS - 1) {
            emit(fn, ctx, AGENT_ERROR, "stopped: too many tool calls in one turn");
            result = ESP_ERR_TIMEOUT;
        }
    }

    cJSON_Delete(tools);
    s_busy = false;

    if (result == ESP_OK) {
        led_set(LED_OK);
    }
    xSemaphoreGive(s_lock);
    return result;
}

static void console_emit(void *ctx, agent_ev_t ev, const char *text)
{
    sesame_out_t *out = ctx;

    switch (ev) {
    case AGENT_TEXT:
        sesame_printf(out, "%s\n", text);
        break;
    case AGENT_TOOL_CALL:
        sesame_printf(out, "  · %s\n", text);
        break;
    case AGENT_TOOL_RESULT: {

        const char *p = text;
        while (p && *p) {
            const char *nl = strchr(p, '\n');
            int len = nl ? (int)(nl - p) : (int)strlen(p);
            if (len > 0) {
                sesame_printf(out, "    %.*s\n", len, p);
            }
            p = nl ? nl + 1 : NULL;
        }
        break;
    }
    case AGENT_ERROR:
        sesame_printf(out, "error: %s\n", text);
        break;
    case AGENT_DONE:
        break;
    }
}

static int cmd_ask(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) {
        sesame_write(out, "usage: ask <what you want done>\n");
        return 1;
    }

    esp_err_t err = agent_run(argv[1], console_emit, out);
    return err == ESP_OK ? 0 : 1;
}

static int cmd_agent(int argc, char **argv, sesame_out_t *out)
{
    if (argc > 1 && strcmp(argv[1], "reset") == 0) {
        agent_reset();
        sesame_write(out, "conversation cleared\n");
        return 0;
    }

    sesame_printf(out, "model    %s\n", cfg_get(CFG_API_MODEL, "?"));
    sesame_printf(out, "endpoint %s\n", cfg_get(CFG_API_URL, "?"));
    sesame_printf(out, "key      %s\n", cfg_has(CFG_API_KEY) ? "set" : "not set");
    sesame_printf(out, "turns    %d\n", s_turns);
    sesame_printf(out, "context  %d chars\n", context_chars());
    sesame_printf(out, "prompt   %d chars, %d commands exposed\n",
                  agent_prompt_bytes(), sesame_cmd_count());
    sesame_printf(out, "hops     up to %d tool calls per turn\n", MAX_HOPS);
    return 0;
}

static const sesame_cmd_t CMDS[] = {
    { "ask",   "ask <request>",   "ask the agent to do something", cmd_ask, true },
    { "agent", "agent [reset]",   "agent status, or clear it",     cmd_agent },
};

void cmd_agent_register(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
}
