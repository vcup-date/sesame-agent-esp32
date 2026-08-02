#include "sesame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "registry";

static const sesame_cmd_t *s_cmds[SESAME_MAX_CMDS];
static int s_count;

void sesame_register(const sesame_cmd_t *cmd)
{
    if (s_count >= SESAME_MAX_CMDS) {
        ESP_LOGE(TAG, "command table full, dropping '%s'", cmd->name);
        return;
    }
    s_cmds[s_count++] = cmd;
}

const sesame_cmd_t *sesame_lookup(const char *name)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_cmds[i]->name, name) == 0) {
            return s_cmds[i];
        }
    }
    return NULL;
}

const sesame_cmd_t *sesame_cmd_at(int i)
{
    return (i >= 0 && i < s_count) ? s_cmds[i] : NULL;
}

int sesame_cmd_count(void) { return s_count; }

void sesame_write(sesame_out_t *o, const char *s)
{
    if (o && o->write) {
        o->write(o, s, strlen(s));
    }
}

void sesame_printf(sesame_out_t *o, const char *fmt, ...)
{
    char stackbuf[256];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
    va_end(ap);

    if (n < 0) {
        return;
    }
    if (n < (int)sizeof(stackbuf)) {
        if (o && o->write) {
            o->write(o, stackbuf, n);
        }
        return;
    }

    char *heap = malloc(n + 1);
    if (!heap) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(heap, n + 1, fmt, ap);
    va_end(ap);
    if (o && o->write) {
        o->write(o, heap, n);
    }
    free(heap);
}

static void stdout_write(sesame_out_t *o, const char *data, size_t len)
{
    (void)o;
    fwrite(data, 1, len, stdout);
    fflush(stdout);
}

static sesame_out_t s_stdout = { .write = stdout_write, .ctx = NULL };

sesame_out_t *sesame_stdout(void) { return &s_stdout; }

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    bool   truncated;
} capture_t;

static void capture_write(sesame_out_t *o, const char *data, size_t len)
{
    capture_t *c = o->ctx;

    if (c->truncated) {
        return;
    }
    if (c->len + len > SESAME_MAX_OUT) {
        len = SESAME_MAX_OUT - c->len;
        c->truncated = true;
    }
    if (c->len + len + 1 > c->cap) {
        size_t want = (c->len + len + 1) * 2;
        char *grown = heap_caps_realloc(c->buf, want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!grown) {
            grown = realloc(c->buf, want);
        }
        if (!grown) {
            c->truncated = true;
            return;
        }
        c->buf = grown;
        c->cap = want;
    }
    memcpy(c->buf + c->len, data, len);
    c->len += len;
    c->buf[c->len] = '\0';
}

static bool is_sep(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int tokenize(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;

    while (*p && argc < max) {
        while (is_sep(*p)) {
            p++;
        }
        if (!*p) {
            break;
        }

        char *w = p;
        argv[argc++] = p;
        char quote = 0;

        while (*p) {
            if (quote) {
                if (*p == quote) {
                    quote = 0;
                    p++;
                    continue;
                }
            } else if (*p == '"' || *p == '\'') {
                quote = *p++;
                continue;
            } else if (is_sep(*p)) {
                break;
            }

            if (*p == '\\' && p[1]) {
                p++;
                switch (*p) {
                case 'n': *w++ = '\n'; p++; continue;
                case 't': *w++ = '\t'; p++; continue;
                case 'r': *w++ = '\r'; p++; continue;
                default: break;
                }
            }
            *w++ = *p++;
        }

        bool end = (*p == '\0');
        p++;
        *w = '\0';
        if (end) {
            break;
        }
    }
    return argc;
}

int sesame_exec(const char *line, sesame_out_t *out)
{
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (!*line || *line == '#') {
        return 0;
    }

    {
        const char *e = line;
        while (*e && !is_sep(*e)) {
            e++;
        }
        char name[24];
        size_t n = (size_t)(e - line);
        if (n < sizeof(name)) {
            memcpy(name, line, n);
            name[n] = '\0';

            const sesame_cmd_t *cmd = sesame_lookup(name);
            if (cmd && cmd->raw) {

                while (is_sep(*e)) {
                    e++;
                }

                char *arg = strdup(e);
                if (!arg) {
                    sesame_write(out, "out of memory\n");
                    return -1;
                }
                char *argv[2] = { name, arg };
                int rc = cmd->fn(arg[0] ? 2 : 1, argv, out);
                free(arg);
                return rc;
            }
        }
    }

    char *copy = strdup(line);
    if (!copy) {
        sesame_write(out, "out of memory\n");
        return -1;
    }

    char *argv[SESAME_MAX_ARGV];
    int argc = tokenize(copy, argv, SESAME_MAX_ARGV);
    if (argc == 0) {
        free(copy);
        return 0;
    }

    const sesame_cmd_t *cmd = sesame_lookup(argv[0]);
    if (!cmd) {
        sesame_printf(out, "%s: no such command. try 'help'\n", argv[0]);
        free(copy);
        return -1;
    }

    int rc = cmd->fn(argc, argv, out);
    free(copy);
    return rc;
}

char *sesame_capture(const char *line, int *rc)
{
    capture_t c = { 0 };
    sesame_out_t sink = { .write = capture_write, .ctx = &c };

    int r = sesame_exec(line, &sink);
    if (rc) {
        *rc = r;
    }

    if (!c.buf) {
        return strdup("");
    }
    if (c.truncated) {

        const char *mark = "\n... [output truncated]\n";
        size_t need = c.len + strlen(mark) + 1;
        char *grown = realloc(c.buf, need);
        if (grown) {
            c.buf = grown;
            strcpy(c.buf + c.len, mark);
        }
    }
    return c.buf;
}
