#include "py.h"
#include "cfg.h"
#include "sesame.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "port/micropython_embed.h"
#include "py/mphal.h"
#include "py/runtime.h"

static const char *TAG = "py";

#define DEFAULT_HEAP_KB  192
#define PY_STACK         16384

#define PY_RUN_LIMIT_MS  30000

typedef struct {
    char              *src;
    sesame_out_t      *out;
    SemaphoreHandle_t  done;
} py_job_t;

static QueueHandle_t s_jobs;
static sesame_out_t *s_sink;
static bool s_ready;

void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len)
{
    if (s_sink && s_sink->write) {
        s_sink->write(s_sink, str, len);
    }
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len)
{
    mp_hal_stdout_tx_strn_cooked(str, len);
    return len;
}

void mp_hal_set_interrupt_char(int c) { (void)c; }

mp_uint_t mp_hal_ticks_ms(void)  { return (mp_uint_t)(esp_timer_get_time() / 1000); }
mp_uint_t mp_hal_ticks_us(void)  { return (mp_uint_t)esp_timer_get_time(); }
mp_uint_t mp_hal_ticks_cpu(void) { return mp_hal_ticks_us(); }

void mp_hal_delay_ms(mp_uint_t ms)
{

    vTaskDelay(pdMS_TO_TICKS(ms));
}

void mp_hal_delay_us(mp_uint_t us)
{
    if (us >= 1000) {
        vTaskDelay(pdMS_TO_TICKS(us / 1000));
        us %= 1000;
    }
    if (us) {
        esp_rom_delay_us(us);
    }
}

static void py_task(void *arg)
{
    (void)arg;

    int heap_kb = atoi(cfg_get("py.heap", "0"));
    if (heap_kb <= 0) {
        heap_kb = DEFAULT_HEAP_KB;
    }
    size_t heap_size = (size_t)heap_kb * 1024;

    char *heap = heap_caps_malloc(heap_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!heap) {
        ESP_LOGE(TAG, "no PSRAM for a %dKB GC heap", heap_kb);
        vTaskDelete(NULL);
        return;
    }

    int stack_top;
    mp_embed_init(heap, heap_size, &stack_top);

    s_ready = true;
    ESP_LOGI(TAG, "python ready, %dKB GC heap in PSRAM", heap_kb);

    for (;;) {
        py_job_t job;
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        s_sink = job.out;
        mp_embed_exec_str(job.src);
        s_sink = NULL;
        free(job.src);
        xSemaphoreGive(job.done);
    }
}

void py_init(void)
{
    s_jobs = xQueueCreate(2, sizeof(py_job_t));
    if (!s_jobs) {
        return;
    }
    xTaskCreate(py_task, "python", PY_STACK, NULL, 3, NULL);
}

bool py_ready(void) { return s_ready; }

void py_interrupt(void)
{

    if (s_ready && s_sink) {
        mp_sched_keyboard_interrupt();
    }
}

int py_run(const char *src, sesame_out_t *out)
{
    if (!s_ready) {
        sesame_write(out, "python is not running\n");
        return 1;
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        return 1;
    }

    char *owned = strdup(src);
    if (!owned) {
        vSemaphoreDelete(done);
        return 1;
    }

    py_job_t job = { .src = owned, .out = out, .done = done };
    if (xQueueSend(s_jobs, &job, pdMS_TO_TICKS(1000)) != pdTRUE) {
        sesame_write(out, "python is busy\n");
        free(owned);
        vSemaphoreDelete(done);
        return 1;
    }

    int rc = 0;
    if (xSemaphoreTake(done, pdMS_TO_TICKS(PY_RUN_LIMIT_MS)) != pdTRUE) {
        sesame_write(out, "\npython: over time, interrupting\n");
        mp_sched_keyboard_interrupt();
        if (xSemaphoreTake(done, pdMS_TO_TICKS(5000)) != pdTRUE) {

            sesame_write(out, "python: did not stop; the VM is wedged\n");
            return 1;
        }
        rc = 1;
    }
    vSemaphoreDelete(done);
    return rc;
}

static bool s_repl_wanted;

// A REPL echoes the value of an expression; a script does not. MicroPython's
// embed entry point only execs, so an expression typed at the prompt would
// evaluate and silently discard its result, which is exactly the thing an
// interactive session is for.
//
// So: if the line looks like an expression rather than a statement, bind it and
// print its repr, skipping None the way the real interpreter does. The test is
// a heuristic, but it errs toward running the line unchanged, and a statement
// wrongly treated as an expression would simply be a syntax error the user can
// see rather than a wrong answer they cannot.
static bool looks_like_expression(const char *line)
{
    static const char *stmt[] = {
        "import ", "from ", "def ", "class ", "for ", "while ", "if ", "elif ",
        "else", "try", "except", "finally", "with ", "return", "pass", "break",
        "continue", "del ", "global ", "nonlocal ", "raise", "assert ", "print(",
        "yield", "@", NULL
    };
    while (*line == ' ' || *line == '\t') {
        return false;   // indented: part of a block, never a bare expression
    }
    for (int i = 0; stmt[i]; i++) {
        if (strncmp(line, stmt[i], strlen(stmt[i])) == 0) {
            return false;
        }
    }
    // An assignment is a statement. Skip ==, !=, <=, >= and anything inside
    // brackets or quotes so `d["k"] == 1` is still seen as an expression.
    int depth = 0;
    char quote = 0;
    for (const char *p = line; *p; p++) {
        if (quote) {
            if (*p == quote) quote = 0;
            continue;
        }
        if (*p == '\'' || *p == '"') { quote = *p; continue; }
        if (*p == '(' || *p == '[' || *p == '{') depth++;
        if (*p == ')' || *p == ']' || *p == '}') depth--;
        if (depth == 0 && *p == '=') {
            char prev = p > line ? p[-1] : 0;
            if (prev != '=' && prev != '!' && prev != '<' && prev != '>' && p[1] != '=') {
                return false;
            }
        }
    }
    return line[0] != '\0';
}

int py_run_interactive(const char *src, sesame_out_t *out)
{
    // Multi-line blocks are always statements.
    if (strchr(src, '\n') && strchr(src, '\n') != src + strlen(src) - 1) {
        return py_run(src, out);
    }
    char line[512];
    strlcpy(line, src, sizeof(line));
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';

    if (!looks_like_expression(line)) {
        return py_run(src, out);
    }
    char wrapped[640];
    snprintf(wrapped, sizeof(wrapped),
             "_r = (%s)\nif _r is not None:\n    print(repr(_r))\n", line);
    return py_run(wrapped, out);
}

bool py_take_repl_request(void)
{
    bool w = s_repl_wanted;
    s_repl_wanted = false;
    return w;
}

static int cmd_py(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) {
        // Bare `python` asks the shell for an interactive session, exactly as
        // the real interpreter behaves. The command cannot read the next line
        // itself; that belongs to whichever shell is running.
        // Reads as a greeting in an interactive shell and as help everywhere
        // else, which matters because the HTTP endpoint cannot open a session.
        s_repl_wanted = true;
        sesame_write(out, "MicroPython on ESP32-S3. Computation only: no files, "
                          "no hardware.\n"
                          "Non-interactive use: python <code>, or python -f <path>\n");
        return 0;
    }

    if (strncmp(argv[1], "-f ", 3) == 0) {
        const char *p = argv[1] + 3;
        while (*p == ' ') {
            p++;
        }
        char path[288];
        if (*p == '/') {
            snprintf(path, sizeof(path), "%s", p);
        } else {
            snprintf(path, sizeof(path), "%s/%s", SESAME_MOUNT, p);
        }

        FILE *f = fopen(path, "rb");
        if (!f) {
            sesame_printf(out, "py: cannot open %s\n", path);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n <= 0 || n > 64 * 1024) {
            fclose(f);
            sesame_printf(out, "py: %s is empty or too large\n", path);
            return 1;
        }
        char *src = malloc(n + 1);
        if (!src) {
            fclose(f);
            return 1;
        }
        size_t got = fread(src, 1, n, f);
        fclose(f);
        src[got] = '\0';

        int rc = py_run(src, out);
        free(src);
        return rc;
    }

    return py_run(argv[1], out);
}

static const sesame_cmd_t CMDS[] = {
    { "python", "python [code] | python -f <path>",
      "MicroPython; bare `python` opens an interactive session", cmd_py, true },
    { "py",     "py [code]",
      "short for python", cmd_py, true },
};

void cmd_py_register(void)
{
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
}
