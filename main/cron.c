// Scheduled and repeating work.
//
// Two lifetimes, because they answer different needs:
//
//   session  lives in RAM and dies with the next reboot. Right for "watch this
//            pin while I am debugging" and for anything the agent sets up on a
//            whim, since a whim should not outlive the power cycle.
//   global   written to the filesystem and reloaded at boot. Right for "photograph
//            the doorway every ten minutes", which is the whole point of leaving
//            a device plugged in.
//
// Everything expires after seven days unless it is explicitly told not to. A
// board left running for a year should not still be executing something set up
// on a Tuesday and forgotten, and an agent that can create jobs can create ones
// nobody remembers asking for. Opting out is one flag, so the durable case is
// still easy; it is just deliberate.

#include "sesame.h"
#include "cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cron";

#define MAX_JOBS     12
#define CMD_MAX      160
#define STORE_PATH   SESAME_MOUNT "/cron.json"
#define DEFAULT_DAYS 7

typedef struct {
    int      id;
    int      every_s;      // 0 means run once
    int64_t  next_us;
    int64_t  expires_us;   // 0 means never
    bool     global;
    bool     used;
    int      runs;
    int      last_rc;
    char     cmd[CMD_MAX];
} job_t;

static job_t            s_jobs[MAX_JOBS];
static int              s_next_id = 1;
static SemaphoreHandle_t s_lock;
static bool             s_running = true;

static int64_t now_us(void) { return esp_timer_get_time(); }

// ── persistence ──────────────────────────────────────────────────────────
// Only global jobs are written. Expiry is stored as seconds from now rather
// than an absolute time, because this device has no battery-backed clock and
// its idea of "now" restarts at zero on every boot.
static void save_global(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!s_jobs[i].used || !s_jobs[i].global) {
            continue;
        }
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "cmd", s_jobs[i].cmd);
        cJSON_AddNumberToObject(j, "every", s_jobs[i].every_s);
        cJSON_AddNumberToObject(j, "ttl",
            s_jobs[i].expires_us ? (double)((s_jobs[i].expires_us - now_us()) / 1000000) : 0);
        cJSON_AddItemToArray(arr, j);
    }
    char *text = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!text) {
        return;
    }
    FILE *f = fopen(STORE_PATH, "wb");
    if (f) {
        fwrite(text, 1, strlen(text), f);
        fclose(f);
    }
    free(text);
}

static int add_job(const char *cmd, int every_s, int ttl_s, bool global);

static void load_global(void)
{
    FILE *f = fopen(STORE_PATH, "rb");
    if (!f) {
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 8192) {
        fclose(f);
        return;
    }
    char *text = malloc(n + 1);
    if (!text) {
        fclose(f);
        return;
    }
    fread(text, 1, n, f);
    text[n] = '\0';
    fclose(f);

    cJSON *arr = cJSON_Parse(text);
    free(text);
    if (!arr) {
        return;
    }
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        cJSON *c = cJSON_GetObjectItem(it, "cmd");
        cJSON *e = cJSON_GetObjectItem(it, "every");
        cJSON *t = cJSON_GetObjectItem(it, "ttl");
        if (!cJSON_IsString(c)) {
            continue;
        }
        int ttl = cJSON_IsNumber(t) ? (int)t->valuedouble : 0;
        // A job whose remaining life ran out while the device was off stays off.
        if (cJSON_IsNumber(t) && t->valuedouble < 0) {
            continue;
        }
        add_job(c->valuestring, cJSON_IsNumber(e) ? (int)e->valuedouble : 0, ttl, true);
    }
    cJSON_Delete(arr);
    ESP_LOGI(TAG, "loaded saved jobs");
}

// ── the scheduler ────────────────────────────────────────────────────────
static int add_job(const char *cmd, int every_s, int ttl_s, bool global)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!s_jobs[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        xSemaphoreGive(s_lock);
        return -1;
    }
    job_t *j = &s_jobs[slot];
    memset(j, 0, sizeof(*j));
    j->id = s_next_id++;
    j->every_s = every_s;
    j->next_us = now_us() + (int64_t)(every_s > 0 ? every_s : 1) * 1000000;
    j->expires_us = ttl_s > 0 ? now_us() + (int64_t)ttl_s * 1000000 : 0;
    j->global = global;
    j->used = true;
    strlcpy(j->cmd, cmd, CMD_MAX);
    xSemaphoreGive(s_lock);
    return j->id;
}

static void cron_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!s_running) {
            continue;
        }

        for (int i = 0; i < MAX_JOBS; i++) {
            char cmd[CMD_MAX];
            bool due = false, expired = false, once = false;

            xSemaphoreTake(s_lock, portMAX_DELAY);
            job_t *j = &s_jobs[i];
            if (j->used) {
                if (j->expires_us && now_us() > j->expires_us) {
                    expired = true;
                } else if (now_us() >= j->next_us) {
                    due = true;
                    once = (j->every_s <= 0);
                    strlcpy(cmd, j->cmd, CMD_MAX);
                    j->next_us = now_us() + (int64_t)(j->every_s > 0 ? j->every_s : 1) * 1000000;
                }
            }
            if (expired) {
                ESP_LOGI(TAG, "job %d expired: %s", j->id, j->cmd);
                bool was_global = j->global;
                j->used = false;
                xSemaphoreGive(s_lock);
                if (was_global) {
                    save_global();
                }
                continue;
            }
            xSemaphoreGive(s_lock);

            if (!due) {
                continue;
            }

            // Run outside the lock: a job can take seconds, and holding the
            // lock would block `cron list` and every other job behind it.
            int rc = 0;
            char *out = sesame_capture(cmd, &rc);
            free(out);

            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_jobs[i].used) {
                s_jobs[i].runs++;
                s_jobs[i].last_rc = rc;
                if (once) {
                    s_jobs[i].used = false;
                }
            }
            xSemaphoreGive(s_lock);
        }
    }
}

// ── command ──────────────────────────────────────────────────────────────
static void human_time(int64_t us, char *buf, size_t n)
{
    if (us <= 0) {
        snprintf(buf, n, "never");
        return;
    }
    int s = (int)(us / 1000000);
    if (s < 90)         snprintf(buf, n, "%ds", s);
    else if (s < 5400)  snprintf(buf, n, "%dm", s / 60);
    else if (s < 172800) snprintf(buf, n, "%dh", s / 3600);
    else                snprintf(buf, n, "%dd", s / 86400);
}

static int cmd_cron(int argc, char **argv, sesame_out_t *out)
{
    const char *sub = argc > 1 ? argv[1] : "list";

    if (strcmp(sub, "list") == 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int n = 0;
        sesame_printf(out, "%-4s %-8s %-8s %-7s %-6s %s\n",
                      "id", "every", "expires", "scope", "runs", "command");
        for (int i = 0; i < MAX_JOBS; i++) {
            if (!s_jobs[i].used) {
                continue;
            }
            char ev[16], ex[16];
            if (s_jobs[i].every_s > 0) {
                human_time((int64_t)s_jobs[i].every_s * 1000000, ev, sizeof(ev));
            } else {
                snprintf(ev, sizeof(ev), "once");
            }
            human_time(s_jobs[i].expires_us ? s_jobs[i].expires_us - now_us() : 0, ex, sizeof(ex));
            sesame_printf(out, "%-4d %-8s %-8s %-7s %-6d %s\n",
                          s_jobs[i].id, ev, ex,
                          s_jobs[i].global ? "global" : "session",
                          s_jobs[i].runs, s_jobs[i].cmd);
            n++;
        }
        xSemaphoreGive(s_lock);
        if (!n) {
            sesame_write(out, "(no jobs)\n");
        }
        sesame_printf(out, "\nscheduler %s\n", s_running ? "running" : "paused");
        return 0;
    }

    if (strcmp(sub, "add") == 0) {
        // cron add [-g] [-k] <seconds|once> <command...>
        bool global = false, keep = false;
        int a = 2;
        while (a < argc && argv[a][0] == '-') {
            if (strchr(argv[a], 'g')) global = true;
            if (strchr(argv[a], 'k')) keep = true;
            a++;
        }
        if (argc < a + 2) {
            sesame_write(out,
                "usage: cron add [-g] [-k] <seconds|once> <command>\n"
                "  -g  global: saved to flash and restored at boot\n"
                "  -k  keep: no expiry (default is 7 days)\n"
                "example: cron add -g 600 snap\n");
            return 1;
        }
        int every = strcmp(argv[a], "once") == 0 ? 0 : atoi(argv[a]);
        if (every < 0)      every = 0;
        if (every && every < 5) every = 5;   // below this it starves the console

        char cmd[CMD_MAX] = { 0 };
        for (int i = a + 1; i < argc; i++) {
            strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 2);
            if (i + 1 < argc) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        }
        // Reject a typo now rather than letting it fail silently every
        // interval for the next seven days.
        char first[32] = { 0 };
        sscanf(cmd, "%31s", first);
        if (!sesame_lookup(first)) {
            sesame_printf(out, "cron: no such command '%s'\n", first);
            return 1;
        }

        int ttl = keep ? 0 : DEFAULT_DAYS * 86400;
        int id = add_job(cmd, every, ttl, global);
        if (id < 0) {
            sesame_printf(out, "cron: no free slots (max %d)\n", MAX_JOBS);
            return 1;
        }
        if (global) {
            save_global();
        }
        char ex[16];
        human_time(ttl ? (int64_t)ttl * 1000000 : 0, ex, sizeof(ex));
        sesame_printf(out, "job %d: %s every %s%s, expires %s, %s\n",
                      id, cmd, every ? argv[a] : "once", every ? "s" : "", ex,
                      global ? "survives reboot" : "this session only");
        return 0;
    }

    if (strcmp(sub, "del") == 0 || strcmp(sub, "delete") == 0) {
        if (argc < 3) {
            sesame_write(out, "usage: cron del <id|all>\n");
            return 1;
        }
        bool all = strcmp(argv[2], "all") == 0;
        int want = all ? -1 : atoi(argv[2]);
        int n = 0;
        bool touched_global = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < MAX_JOBS; i++) {
            if (s_jobs[i].used && (all || s_jobs[i].id == want)) {
                touched_global |= s_jobs[i].global;
                s_jobs[i].used = false;
                n++;
            }
        }
        xSemaphoreGive(s_lock);
        if (touched_global) {
            save_global();
        }
        sesame_printf(out, "%d job%s removed\n", n, n == 1 ? "" : "s");
        return n ? 0 : 1;
    }

    if (strcmp(sub, "pause") == 0 || strcmp(sub, "resume") == 0) {
        s_running = strcmp(sub, "resume") == 0;
        sesame_printf(out, "scheduler %s\n", s_running ? "running" : "paused");
        return 0;
    }

    sesame_write(out, "usage: cron [list|add [-g] [-k] <secs|once> <cmd>|del <id|all>|pause|resume]\n");
    return 1;
}

static const sesame_cmd_t CMDS[] = {
    { "cron", "cron [list|add [-g] [-k] <secs|once> <cmd>|del <id>|pause|resume]",
      "run a command on a schedule; -g survives reboot, -k never expires", cmd_cron },
};

void cmd_cron_register(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
}

void cron_start(void)
{
    load_global();
    xTaskCreate(cron_task, "cron", 6144, NULL, 4, NULL);
}
