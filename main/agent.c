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

// How many model round trips one request may take. This was 12, which was far
// too mean: a physical task is naturally many small steps, and asking the LED
// to walk through a colour sweep one command at a time hit the ceiling and had
// the turn killed mid-way. The limit exists only to stop a model looping
// forever, so it belongs well above anything a real task needs, and it is
// configurable for the cases that need more.
#define DEFAULT_HOPS  64

static int max_hops(void)
{
    int h = atoi(cfg_get("agent.hops", "64"));
    if (h < 1)   h = 1;
    if (h > 500) h = 500;
    return h;
}

#define MAX_RESPONSE  (96 * 1024)

// When the conversation passes this, the oldest half is summarised rather than
// discarded. Configurable: the model's own window is far larger than this, and
// the real cost here is that every hop re-uploads the whole conversation over
// WiFi, so the right value depends on how good your link is.
// Measured, not guessed. Round trip time was flat from 0 to 24k characters of
// context on this device (3.5s, 3.6s, 3.2s, 3.9s), so transfer is not the
// constraint at any sane size; the model's own thinking time dominates.
//
// What does constrain it is PSRAM. Peak use is roughly 3.3x the encoded size:
// the cJSON structure, plus the request body, plus whatever the HTTP layer
// holds. With ~6MB free that puts the hard ceiling near 1.8MB of conversation,
// With the body streamed rather than built, only the tree remains, so the
// ceiling moved from ~650k tokens to roughly 1.1M and the maximum here is
// 4.5M characters, about 1.1M tokens.
//
// The previous default was 48KB, which was a guess of mine and about twenty
// times too small.
#define DEFAULT_CONTEXT (256 * 1024)

static int max_context(void)
{
    int c = atoi(cfg_get("agent.ctx", "262144"));
    if (c < 4096)      c = 4096;
    if (c > 4500000) c = 4500000;
    return c;
}

static cJSON            *s_messages;
static SemaphoreHandle_t s_lock;
static int               s_turns;
static bool              s_busy;
static volatile bool     s_stop;
// Prefix caching is the single biggest lever on this device: an unchanged
// prefix comes back ~99% cached, which skips re-processing it. Worth showing,
// because it also means compaction is not free: rewriting the older half
// changes the prefix and throws the cache away.
static uint32_t s_cache_hit, s_cache_miss;

// Compaction happens deep inside the hop loop, well below the caller that owns
// the output sink. Without this it runs silently, and since it can take minutes
// on a large conversation the only visible symptom is the device appearing to
// hang. Progress is not a nicety here, it is the difference between "working"
// and "broken" from the outside.
static agent_emit_fn s_emit_fn;
static void         *s_emit_ctx;

static void emit_status(const char *fmt, ...)
{
    if (!s_emit_fn) {
        return;
    }
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    s_emit_fn(s_emit_ctx, AGENT_STATUS, msg);
}

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

    // Facts only. Everything here is something that cannot be worked out from
    // the command table: a quirk of the parser, a capability that is missing,
    // or how the hardware is wired. Style, tone, brevity and language are the
    // model's own business and it does them better unprompted.
    pappend(&b,
        "\nHow the tool behaves:\n\n"
        "- No shell syntax: no pipes, redirection, globbing or &&. One command "
        "per call, spelled as above.\n"
        "- Each call is a network round trip. `seq` runs a whole list in one "
        "call: `seq rgb 255 0 0; wait 150; rgb 0 255 0; wait 150`, and "
        "`seq xN <list>` repeats it. Timing inside `seq` is the device's.\n"
        "- `write` and `py` take the rest of the line verbatim, newlines and "
        "quotes included, so send real multi-line text rather than escaping it.\n"
        "- `py` is MicroPython for computation only. No `open()`, no `os`, no "
        "importing your own files, and no hardware access at all: `machine` and "
        "`neopixel` do not exist. `time` does. Hardware is reached only through "
        "the commands above.\n"
        "- `snap` writes a JPEG and reports its path. The image itself is not "
        "returned to you.\n"
        "- The colour LED is `rgb` and `led` on GPIO48. The red LED is wired "
        "across the power rail with no GPIO, and the blue one is the UART "
        "transmit line, so neither can be switched.\n"
        "- `skill list` names saved procedures and `skill show <name>` reads "
        "one. Check it when a task sounds like something done before, and "
        "write a skill when you work something out worth keeping.\n"
        "- `cron` schedules a command to repeat. Jobs expire after 7 days "
        "unless created with -k, and only survive a reboot with -g.\n");

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




// Send the request without ever holding it whole.
//
// The old path built the entire body with cJSON_PrintUnformatted and handed
// that string to the HTTP client, so at the moment of sending the conversation
// existed twice: once as the cJSON tree, once as one enormous string. That
// second copy is what set the ceiling, because the peak was about 2.3x the
// conversation and 6MB of PSRAM therefore ran out around 650k tokens.
//
// Here each message is serialised on its own, written to the socket, and freed
// before the next one. Peak is the tree plus a single message. Content-Length
// has to be known before the first byte goes out, so the messages are measured
// in one pass and written in a second: twice the CPU, a fraction of the memory,
// and no dependency on the server accepting chunked encoding.
static cJSON *post_stream(cJSON *messages, cJSON *tools, int max_tokens,
                          int *status, char **err_text)
{
    char url[192];
    const char *base = cfg_get(CFG_API_URL, "https://api.deepseek.com");
    size_t blen = strlen(base);
    bool has_slash = blen && base[blen - 1] == '/';
    snprintf(url, sizeof(url), "%s%schat/completions", base, has_slash ? "" : "/");

    char auth[400];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg_get(CFG_API_KEY, ""));

    char head[256];
    int head_len = snprintf(head, sizeof(head),
        "{\"model\":\"%s\",\"max_tokens\":%d,\"messages\":[",
        cfg_get(CFG_API_MODEL, "deepseek-v4-flash"), max_tokens);

    char *tools_json = NULL;
    int tail_len;
    char tail[64];
    if (tools) {
        tools_json = cJSON_PrintUnformatted(tools);
        if (!tools_json) {
            *err_text = strdup("out of memory");
            return NULL;
        }
        tail_len = snprintf(tail, sizeof(tail), "],\"tool_choice\":\"auto\",\"tools\":");
    } else {
        tail_len = snprintf(tail, sizeof(tail), "]");
    }

    // Pass one: how long is the whole thing.
    int total = head_len + tail_len + 1;   // +1 for the closing brace
    if (tools_json) {
        total += strlen(tools_json);
    }
    int n = cJSON_GetArraySize(messages);
    for (int i = 0; i < n; i++) {
        char *m = cJSON_PrintUnformatted(cJSON_GetArrayItem(messages, i));
        if (!m) {
            free(tools_json);
            *err_text = strdup("out of memory measuring the request");
            return NULL;
        }
        total += strlen(m) + (i ? 1 : 0);   // comma between messages
        free(m);
    }

    // No event handler here: the body is read explicitly below. Leaving one
    // attached would buffer the whole response a second time.
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 180000,
        .buffer_size       = 2048,
        .buffer_size_tx    = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(tools_json);
        *err_text = strdup("could not create the HTTP client");
        return NULL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);

    esp_err_t err = esp_http_client_open(client, total);
    if (err != ESP_OK) {
        free(tools_json);
        esp_http_client_cleanup(client);
        char msg[128];
        snprintf(msg, sizeof(msg), "network: %s", esp_err_to_name(err));
        *err_text = strdup(msg);
        return NULL;
    }

    // Pass two: write it out a message at a time.
    int sent = 0;
    bool ok = esp_http_client_write(client, head, head_len) == head_len;
    sent += head_len;
    for (int i = 0; ok && i < n; i++) {
        if (i) {
            ok = esp_http_client_write(client, ",", 1) == 1;
            sent += 1;
        }
        char *m = ok ? cJSON_PrintUnformatted(cJSON_GetArrayItem(messages, i)) : NULL;
        if (!m) {
            ok = false;
            break;
        }
        int len = strlen(m);
        ok = esp_http_client_write(client, m, len) == len;
        sent += len;
        free(m);
    }
    if (ok) {
        ok = esp_http_client_write(client, tail, tail_len) == tail_len;
        sent += tail_len;
    }
    if (ok && tools_json) {
        int len = strlen(tools_json);
        ok = esp_http_client_write(client, tools_json, len) == len;
        sent += len;
    }
    if (ok) {
        ok = esp_http_client_write(client, "}", 1) == 1;
        sent += 1;
    }
    free(tools_json);

    // The two passes must agree exactly. If they ever do not, the server is
    // left waiting for bytes that never come and the request hangs until it
    // times out, which is a miserable thing to debug from a symptom.
    if (ok && sent != total) {
        ESP_LOGE(TAG, "content-length %d but wrote %d", total, sent);
    }

    if (!ok) {
        esp_http_client_cleanup(client);
        *err_text = strdup("connection dropped while sending");
        return NULL;
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        esp_http_client_cleanup(client);
        *err_text = strdup("no response headers");
        return NULL;
    }
    *status = esp_http_client_get_status_code(client);

    // The event handler is not used on this path, so read the body directly.
    int cap = 4096, got = 0;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(cap);
    while (buf) {
        if (got + 1024 > cap) {
            int want = cap * 2;
            if (want > MAX_RESPONSE) { break; }
            char *g = heap_caps_realloc(buf, want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!g) g = realloc(buf, want);
            if (!g) break;
            buf = g; cap = want;
        }
        int r = esp_http_client_read(client, buf + got, cap - got - 1);
        if (r <= 0) break;
        got += r;
    }
    if (buf) buf[got] = '\0';
    esp_http_client_cleanup(client);

    if (*status != 200) {
        char msg[320];
        const char *detail = buf && buf[0] ? buf : "(no body)";
        cJSON *j = buf ? cJSON_Parse(buf) : NULL;
        if (j) {
            cJSON *e = cJSON_GetObjectItem(j, "error");
            cJSON *m = e ? cJSON_GetObjectItem(e, "message") : NULL;
            if (cJSON_IsString(m)) detail = m->valuestring;
        }
        snprintf(msg, sizeof(msg), "HTTP %d: %.240s", *status, detail);
        *err_text = strdup(msg);
        if (j) cJSON_Delete(j);
        free(buf);
        return NULL;
    }

    cJSON *parsed = buf ? cJSON_Parse(buf) : NULL;
    if (!parsed) {
        *err_text = strdup("could not parse the reply");
    }
    free(buf);
    return parsed;
}


// Sum the strings rather than serialising the whole conversation.
//
// This used to call cJSON_PrintUnformatted purely to measure a length, which
// allocates and fills a complete copy of the conversation on every call, and
// trim_context calls it inside a loop. That is quadratic, and at a megabyte of
// context it would dominate everything else the device does. Walking the tree
// costs no allocation at all. The figure is an estimate of the encoded size,
// which is all a threshold needs.
static int context_chars(void)
{
    if (!s_messages) {
        return 0;
    }
    int total = 0;
    cJSON *m = NULL;
    cJSON_ArrayForEach(m, s_messages) {
        cJSON *f = NULL;
        cJSON_ArrayForEach(f, m) {
            if (cJSON_IsString(f) && f->valuestring) {
                total += strlen(f->valuestring);
            } else if (cJSON_IsArray(f) || cJSON_IsObject(f)) {
                // tool_calls: rare and small next to content, so a rough
                // allowance beats another full serialisation.
                total += 200;
            }
            total += f->string ? strlen(f->string) + 6 : 6;
        }
        total += 4;
    }
    return total;
}

int agent_context_chars(void) { return context_chars(); }

// Ask the model to summarise the oldest part of the conversation, and put that
// summary back in place of it.
//
// The previous behaviour simply deleted the oldest messages, which is why a
// long session would suddenly forget what it had been doing: the earlier work
// was not condensed, it was destroyed. Summarising costs one extra API call at
// the moment the conversation gets long, and in exchange the device keeps the
// substance of everything that came before in a few hundred characters.
//
// Returns false if it could not summarise, in which case the caller falls back
// to dropping messages, because a conversation that cannot be sent at all is
// worse than one that lost its oldest turns.
static bool compact_context(void)
{
    int n = cJSON_GetArraySize(s_messages);
    if (n < 6) {
        return false;   // nothing worth summarising yet
    }

    // Summarise the older half. Index 0 is the system prompt and stays.
    int cut = 1 + (n - 1) / 2;
    // Never cut so that a tool result becomes the first message: it would have
    // no matching tool call in front of it, and the API rejects that outright.
    while (cut < n) {
        cJSON *m = cJSON_GetArrayItem(s_messages, cut);
        cJSON *role = m ? cJSON_GetObjectItem(m, "role") : NULL;
        if (cJSON_IsString(role) && strcmp(role->valuestring, "tool") == 0) {
            cut++;
        } else {
            break;
        }
    }
    if (cut <= 1 || cut >= n) {
        return false;
    }

    cJSON *msgs = cJSON_CreateArray();
    for (int i = 1; i < cut; i++) {
        cJSON_AddItemToArray(msgs, cJSON_Duplicate(cJSON_GetArrayItem(s_messages, i), true));
    }
    cJSON *ask = cJSON_CreateObject();
    cJSON_AddStringToObject(ask, "role", "user");
    cJSON_AddStringToObject(ask, "content",
        "Summarise the conversation above in under 200 words. Keep what still "
        "matters for continuing: what was asked, what was done, file paths "
        "written, device state changed, and anything that failed and why. Write "
        "it as notes, not prose. Do not call any tools.");
    cJSON_AddItemToArray(msgs, ask);

    int status = 0;
    char *err = NULL;
    ESP_LOGI(TAG, "compacting %d messages", cut - 1);
    emit_status("compacting %d earlier messages (%d KB) to keep the conversation "
           "in memory, this can take a while", cut - 1, context_chars() / 1024);
    cJSON *reply = post_stream(msgs, NULL, 400, &status, &err);
    cJSON_Delete(msgs);
    free(err);
    if (!reply) {
        return false;
    }

    cJSON *choice  = cJSON_GetArrayItem(cJSON_GetObjectItem(reply, "choices"), 0);
    cJSON *message = choice ? cJSON_GetObjectItem(choice, "message") : NULL;
    cJSON *content = message ? cJSON_GetObjectItem(message, "content") : NULL;
    if (!cJSON_IsString(content) || !content->valuestring[0]) {
        cJSON_Delete(reply);
        return false;
    }

    char *summary = malloc(strlen(content->valuestring) + 64);
    if (!summary) {
        cJSON_Delete(reply);
        return false;
    }
    sprintf(summary, "Earlier in this session:\n%s", content->valuestring);
    cJSON_Delete(reply);

    // Swap the summarised span for a single note, oldest first so the indices
    // stay put while deleting.
    for (int i = 1; i < cut; i++) {
        cJSON_DeleteItemFromArray(s_messages, 1);
    }
    cJSON *note = cJSON_CreateObject();
    cJSON_AddStringToObject(note, "role", "user");
    cJSON_AddStringToObject(note, "content", summary);
    cJSON_InsertItemInArray(s_messages, 1, note);
    free(summary);

    ESP_LOGI(TAG, "compacted to %d chars", context_chars());
    emit_status("compacted to %d KB", context_chars() / 1024);
    return true;
}

static void trim_context(void)
{
    if (context_chars() > max_context()) {
        compact_context();
    }

    // Fallback: if summarising failed or did not free enough, drop the oldest
    // messages as before. Losing history beats a request the API will refuse.
    while (context_chars() > max_context() && cJSON_GetArraySize(s_messages) > 3) {

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
    s_emit_fn = fn;
    s_emit_ctx = ctx;
    led_set(LED_THINKING);

    ensure_conversation();

    cJSON *um = cJSON_CreateObject();
    cJSON_AddStringToObject(um, "role", "user");
    cJSON_AddStringToObject(um, "content", user_msg);
    cJSON_AddItemToArray(s_messages, um);

    esp_err_t result = ESP_OK;
    cJSON *tools = build_tools();

    int hops = max_hops();
    for (int hop = 0; hop < hops; hop++) {
        if (s_stop) {
            emit(fn, ctx, AGENT_ERROR, "stopped");
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        trim_context();

        int status = 0;
        char *err_text = NULL;
        int ctx_now = context_chars();
        ESP_LOGI(TAG, "hop %d, ~%d chars of context", hop + 1, ctx_now);
        if (ctx_now > 200000) {
            emit_status("sending %d KB of conversation, about %ds just to upload",
                   ctx_now / 1024, ctx_now / 95000);
        }
        cJSON *reply = post_stream(s_messages, tools, 2048, &status, &err_text);

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

        cJSON *usage = cJSON_GetObjectItem(reply, "usage");
        if (usage) {
            cJSON *h = cJSON_GetObjectItem(usage, "prompt_cache_hit_tokens");
            cJSON *m = cJSON_GetObjectItem(usage, "prompt_cache_miss_tokens");
            if (cJSON_IsNumber(h)) s_cache_hit  += (uint32_t)h->valuedouble;
            if (cJSON_IsNumber(m)) s_cache_miss += (uint32_t)m->valuedouble;
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

        if (hop == hops - 1) {
            emit(fn, ctx, AGENT_ERROR, "stopped: hit the per-turn tool call limit. Use `seq` to do a "
                                 "repetitive job in one call, or raise it with "
                                 "`cfg set agent.hops <n>`");
            result = ESP_ERR_TIMEOUT;
        }
    }

    cJSON_Delete(tools);
    s_emit_fn = NULL;
    s_emit_ctx = NULL;
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
    case AGENT_STATUS:
        sesame_printf(out, "  ... %s\n", text);
        break;
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
    sesame_printf(out, "hops     up to %d tool calls per turn (cfg set agent.hops N)\n", max_hops());
    sesame_printf(out, "compact  at %d chars (cfg set agent.ctx N)\n", max_context());
    uint32_t tot = s_cache_hit + s_cache_miss;
    sesame_printf(out, "prefix   %u of %u prompt tokens served from cache%s\n",
                  (unsigned)s_cache_hit, (unsigned)tot,
                  tot ? "" : " (nothing sent yet)");
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
