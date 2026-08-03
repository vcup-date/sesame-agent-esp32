#include "sesame.h"

#include <stdio.h>
#include <string.h>

static const char *EIGHTHS[] = { "", "▏", "▎", "▍", "▌", "▋", "▊", "▉" };

void sesame_bar(sesame_out_t *out, const char *label,
                uint64_t used, uint64_t total, int width)
{
    if (width <= 0) {
        width = 24;
    }

    int pct = total ? (int)((used * 100) / total) : 0;

    uint64_t eighths = total ? (used * (uint64_t)width * 8) / total : 0;
    int full = (int)(eighths / 8);
    int rem  = (int)(eighths % 8);

    if (full > width) {
        full = width;
        rem = 0;
    }

    char buf[256];
    int n = 0;
    for (int i = 0; i < full && n < (int)sizeof(buf) - 8; i++) {
        n += snprintf(buf + n, sizeof(buf) - n, "█");
    }
    if (rem && full < width && n < (int)sizeof(buf) - 8) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s", EIGHTHS[rem]);
        full++;
    }
    for (int i = full; i < width && n < (int)sizeof(buf) - 8; i++) {
        n += snprintf(buf + n, sizeof(buf) - n, "·");
    }
    buf[n] = '\0';

    sesame_printf(out, "%-9s %s %3d%%\n", label ? label : "", buf, pct);
}

static const char *LEVELS[] = { "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };

void sesame_spark(sesame_out_t *out, const int *values, int n)
{
    if (n <= 0) {
        return;
    }

    int lo = values[0], hi = values[0];
    for (int i = 1; i < n; i++) {
        if (values[i] < lo) lo = values[i];
        if (values[i] > hi) hi = values[i];
    }

    int span = hi - lo;
    for (int i = 0; i < n; i++) {

        int level = span ? ((values[i] - lo) * 7) / span : 3;
        sesame_printf(out, "%s", LEVELS[level]);
    }
    sesame_printf(out, "  %d..%d\n", lo, hi);
}

void sesame_banner(sesame_out_t *out, const char *version)
{
    sesame_write(out,
        "\n"
        "   ███████ ███████ ███████  █████  ███    ███ ███████\n"
        "   ██      ██      ██      ██   ██ ████  ████ ██     \n"
        "   ███████ █████   ███████ ███████ ██ ████ ██ █████  \n"
        "        ██ ██           ██ ██   ██ ██  ██  ██ ██     \n"
        "   ███████ ███████ ███████ ██   ██ ██      ██ ███████\n"
        "\n"
        "        an agent that lives on the board\n");
    sesame_printf(out, "            %s\n\n", version ? version : "");
}

