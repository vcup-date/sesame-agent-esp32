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

static int exec_one(const char *line, sesame_out_t *out)
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


// ── shell operators: |  >  >>  && ────────────────────────────────────────
//
// Not a real shell, and it does not pretend to be one, but three operators
// carry most of the weight and all three are honest to implement here.
//
// There is no stdin on this device: a command takes argv, not a stream. So a
// pipe is emulated by capturing the left side to a temporary file and handing
// that path to the right side as one more argument. That is exactly what the
// filters want, since `grep`, `head`, `tail`, `wc` and `text` all take a path
// as their last argument, so `ls | grep py` becomes `grep py <tmp>` and works.
// It will not help a command that has no file argument, and it says so rather
// than failing strangely.
//
// Raw commands are deliberately exempt. `write` and `py` take the rest of the
// line verbatim, so a `>` inside a file body or a `|` inside Python source must
// stay literal. Parsing operators there would corrupt exactly the payloads that
// are hardest to notice being corrupted.

#define PIPE_A SESAME_MOUNT "/.pipe.a"
#define PIPE_B SESAME_MOUNT "/.pipe.b"

// Find an operator at the top level: outside quotes, and not part of a longer
// token like ">>" when looking for ">".
static char *find_op(char *s, const char *op)
{
    size_t n = strlen(op);
    char quote = 0;
    for (char *p = s; *p; p++) {
        if (quote) {
            if (*p == quote) quote = 0;
            continue;
        }
        if (*p == '"' || *p == '\'') { quote = *p; continue; }
        if (strncmp(p, op, n) == 0) {
            // ">" must not match the first char of ">>"
            if (n == 1 && op[0] == '>' && p[1] == '>') continue;
            return p;
        }
    }
    return NULL;
}

static bool is_raw_command(const char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    char name[24];
    size_t n = 0;
    while (line[n] && !is_sep(line[n]) && n < sizeof(name) - 1) {
        name[n] = line[n];
        n++;
    }
    name[n] = '\0';
    const sesame_cmd_t *c = sesame_lookup(name);
    return c && c->raw;
}

static void write_all(const char *path, const char *text, bool append)
{
    FILE *f = fopen(path, append ? "ab" : "wb");
    if (!f) return;
    if (text) fwrite(text, 1, strlen(text), f);
    fclose(f);
}

// One pipeline: a | b | c, with an optional > or >> on the end.
static int exec_pipeline(char *seg, sesame_out_t *out)
{
    char *redir = find_op(seg, ">>");
    bool append = redir != NULL;
    if (!redir) redir = find_op(seg, ">");

    char *target = NULL;
    if (redir) {
        *redir = '\0';
        target = redir + (append ? 2 : 1);
        while (*target == ' ' || *target == '\t') target++;
        if (!*target) {
            sesame_write(out, "expected a file after > \n");
            return -1;
        }
    }

    // split the stages
    char *stage[6];
    int nstage = 0;
    stage[nstage++] = seg;
    char *bar;
    while (nstage < 6 && (bar = find_op(stage[nstage - 1], "|")) != NULL) {
        *bar = '\0';
        stage[nstage++] = bar + 1;
    }

    int rc = 0;
    const char *carry = NULL;   // temp file holding the previous stage's output
    int slot = 0;               // alternates so a stage never reads the file it writes

    for (int i = 0; i < nstage; i++) {
        char *cmd = stage[i];
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        if (!*cmd) {
            sesame_write(out, "empty pipeline stage\n");
            return -1;
        }

        char joined[512];
        if (carry) {
            snprintf(joined, sizeof(joined), "%s %s", cmd, carry);
            cmd = joined;
        }

        bool last = (i == nstage - 1);
        if (last && !target) {
            rc = exec_one(cmd, out);       // straight to the caller's sink
        } else {
            int r = 0;
            char *captured = sesame_capture(cmd, &r);
            rc = r;
            const char *next = slot ? PIPE_B : PIPE_A;
            slot = !slot;
            if (last) {
                write_all(target, captured, append);
                sesame_printf(out, "%u bytes %s %s\n",
                              (unsigned)(captured ? strlen(captured) : 0),
                              append ? "appended to" : "written to", target);
            } else {
                write_all(next, captured, false);
            }
            free(captured);
            carry = next;
        }
        if (rc != 0 && !last) {
            break;   // a failed stage has nothing useful to hand on
        }
    }

    remove(PIPE_A);
    remove(PIPE_B);
    return rc;
}

int sesame_exec(const char *line, sesame_out_t *out)
{
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#') return 0;

    // A raw command owns the whole line; operators inside it are its data.
    if (is_raw_command(line)) {
        return exec_one(line, out);
    }

    char *work = strdup(line);
    if (!work) {
        sesame_write(out, "out of memory\n");
        return -1;
    }

    int rc = 0;
    char *seg = work;
    for (;;) {
        char *amp = find_op(seg, "&&");
        if (amp) *amp = '\0';
        rc = exec_pipeline(seg, out);
        if (!amp || rc != 0) break;   // && stops at the first failure
        seg = amp + 2;
    }
    free(work);
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
