// Skills: procedures the device remembers, written in plain language.
//
// A skill is a text file under /sesame/skills. The first line is its one-line
// description; the rest is whatever the agent needs to be told to do the job
// properly, usually the exact commands and the order they go in.
//
// The point is that they are NOT in the system prompt. Everything the model is
// told on every single request costs tokens on every single request, and most
// knowledge is only needed occasionally. So the prompt carries one line saying
// skills exist, `skill list` shows what is available, and `skill show` pulls one
// in only when it is actually relevant. That keeps the standing cost at a
// sentence while the library can grow to whatever the filesystem holds.
//
// This is also how a device teaches itself: the agent can write a skill after
// working something out, and find it again months later.

#include "sesame.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SKILL_DIR SESAME_MOUNT "/skills"

static void skill_path(char *buf, size_t n, const char *name)
{
    // Names only, never paths: a skill called "../../etc" should not be able
    // to read or write outside the skills directory.
    const char *base = name;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    snprintf(buf, n, "%s/%.40s", SKILL_DIR, base);
}

static void ensure_dir(void)
{
    struct stat st;
    if (stat(SKILL_DIR, &st) != 0) {
        mkdir(SKILL_DIR, 0777);
    }
}

static int cmd_skill(int argc, char **argv, sesame_out_t *out)
{
    ensure_dir();
    const char *sub = argc > 1 ? argv[1] : "list";

    if (strcmp(sub, "list") == 0) {
        DIR *d = opendir(SKILL_DIR);
        if (!d) {
            sesame_write(out, "(no skills yet)\n");
            return 0;
        }
        struct dirent *e;
        int n = 0;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') {
                continue;
            }
            char path[288];
            snprintf(path, sizeof(path), "%s/%s", SKILL_DIR, e->d_name);
            char first[120] = "";
            FILE *f = fopen(path, "rb");
            if (f) {
                if (fgets(first, sizeof(first), f)) {
                    char *nl = strchr(first, '\n');
                    if (nl) *nl = '\0';
                }
                fclose(f);
            }
            sesame_printf(out, "  %-22s %s\n", e->d_name, first);
            n++;
        }
        closedir(d);
        if (!n) {
            sesame_write(out, "(no skills yet)\n");
        } else {
            sesame_printf(out, "\n%d skill%s. `skill show <name>` to read one.\n",
                          n, n == 1 ? "" : "s");
        }
        return 0;
    }

    if (strcmp(sub, "show") == 0 || strcmp(sub, "read") == 0) {
        if (argc < 3) {
            sesame_write(out, "usage: skill show <name>\n");
            return 1;
        }
        char path[288];
        skill_path(path, sizeof(path), argv[2]);
        FILE *f = fopen(path, "rb");
        if (!f) {
            sesame_printf(out, "skill: no such skill '%s'\n", argv[2]);
            return 1;
        }
        char buf[512];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf) - 1, f)) > 0) {
            buf[got] = '\0';
            sesame_write(out, buf);
        }
        fclose(f);
        sesame_write(out, "\n");
        return 0;
    }

    if (strcmp(sub, "rm") == 0 || strcmp(sub, "delete") == 0) {
        if (argc < 3) {
            sesame_write(out, "usage: skill rm <name>\n");
            return 1;
        }
        char path[288];
        skill_path(path, sizeof(path), argv[2]);
        if (remove(path) != 0) {
            sesame_printf(out, "skill: could not remove '%s'\n", argv[2]);
            return 1;
        }
        sesame_printf(out, "removed %s\n", argv[2]);
        return 0;
    }

    sesame_write(out,
        "usage: skill [list|show <name>|rm <name>]\n"
        "A skill is a file in " SKILL_DIR ". Write one with:\n"
        "  write " SKILL_DIR "/<name> <first line is the description>\n"
        "  <then the steps, commands included>\n");
    return 1;
}

static const sesame_cmd_t CMDS[] = {
    { "skill", "skill [list|show <name>|rm <name>]",
      "saved procedures; read one only when it is relevant", cmd_skill },
};

void cmd_skill_register(void)
{
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
}
