#include "sesame.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_vfs_fat.h"

static void resolve(const char *in, char *out, size_t n)
{
    if (!in || !in[0] || strcmp(in, ".") == 0) {
        snprintf(out, n, "%s", SESAME_MOUNT);
    } else if (in[0] == '/') {

        if (strncmp(in, SESAME_MOUNT, strlen(SESAME_MOUNT)) == 0) {
            snprintf(out, n, "%s", in);
        } else {
            snprintf(out, n, "%s%s", SESAME_MOUNT, in);
        }
    } else {
        snprintf(out, n, "%s/%s", SESAME_MOUNT, in);
    }
}

static int cmd_ls(int argc, char **argv, sesame_out_t *out)
{
    char path[256];
    resolve(argc > 1 ? argv[1] : NULL, path, sizeof(path));

    DIR *d = opendir(path);
    if (!d) {
        sesame_printf(out, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }

    int files = 0;
    long total = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);

        struct stat st;
        if (stat(full, &st) != 0) {
            sesame_printf(out, "%-28s ?\n", e->d_name);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            sesame_printf(out, "%-28s   dir\n", e->d_name);
        } else {
            sesame_printf(out, "%-28s %5ld\n", e->d_name, (long)st.st_size);
            total += st.st_size;
            files++;
        }
    }
    closedir(d);
    sesame_printf(out, "%d file%s, %ld bytes\n", files, files == 1 ? "" : "s", total);
    return 0;
}

static int cmd_cat(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) {
        sesame_write(out, "usage: cat <path>\n");
        return 1;
    }

    char path[256];
    resolve(argv[1], path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) {
        sesame_printf(out, "cat: %s: %s\n", path, strerror(errno));
        return 1;
    }

    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (out && out->write) {
            out->write(out, buf, n);
        }
    }
    fclose(f);
    return 0;
}

static int cmd_write(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2 || !argv[1][0]) {
        sesame_write(out, "usage: write <path> <text>\n"
                          "the text may span lines and is stored exactly as given\n");
        return 1;
    }

    const char *p = argv[1];
    const char *e = p;
    while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r') {
        e++;
    }

    char raw_path[256];
    size_t plen = (size_t)(e - p);
    if (plen == 0 || plen >= sizeof(raw_path)) {
        sesame_write(out, "write: bad path\n");
        return 1;
    }
    memcpy(raw_path, p, plen);
    raw_path[plen] = '\0';

    while (*e == ' ' || *e == '\t') {
        e++;
    }
    if (*e == '\r') {
        e++;
    }
    if (*e == '\n') {
        e++;
    }

    char path[256];
    resolve(raw_path, path, sizeof(path));

    FILE *f = fopen(path, "wb");
    if (!f) {
        sesame_printf(out, "write: %s: %s\n", path, strerror(errno));
        return 1;
    }

    size_t len = strlen(e);
    size_t written = len ? fwrite(e, 1, len, f) : 0;
    fclose(f);

    if (written != len) {
        sesame_printf(out, "write: %s: short write, filesystem full?\n", path);
        return 1;
    }
    sesame_printf(out, "wrote %u bytes to %s\n", (unsigned)written, path);
    return 0;
}

static int cmd_rm(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) {
        sesame_write(out, "usage: rm <path>\n");
        return 1;
    }

    char path[256];
    resolve(argv[1], path, sizeof(path));

    if (unlink(path) != 0) {
        sesame_printf(out, "rm: %s: %s\n", path, strerror(errno));
        return 1;
    }
    sesame_printf(out, "removed %s\n", path);
    return 0;
}

static int cmd_mkdir(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) {
        sesame_write(out, "usage: mkdir <path>\n");
        return 1;
    }

    char path[256];
    resolve(argv[1], path, sizeof(path));

    if (mkdir(path, 0777) != 0) {
        sesame_printf(out, "mkdir: %s: %s\n", path, strerror(errno));
        return 1;
    }
    sesame_printf(out, "created %s\n", path);
    return 0;
}

static int cmd_df(int argc, char **argv, sesame_out_t *out)
{
    (void)argc; (void)argv;

    uint64_t total = 0, free_bytes = 0;
    esp_err_t err = esp_vfs_fat_info(SESAME_MOUNT, &total, &free_bytes);
    if (err != ESP_OK) {
        sesame_write(out, "df: filesystem not mounted\n");
        return 1;
    }

    sesame_bar(out, "sesame", total - free_bytes, total, 28);
    sesame_printf(out, "%9s %llu KB free of %llu KB\n",
                  "", free_bytes / 1024, total / 1024);
    return 0;
}

static const sesame_cmd_t CMDS[] = {
    { "ls",    "ls [path]",           "list a directory",              cmd_ls },
    { "cat",   "cat <path>",          "print a file",                  cmd_cat },
    { "write", "write <path> <text>", "write text to a file (may span lines)",
      cmd_write, true },
    { "rm",    "rm <path>",           "delete a file",                 cmd_rm },
    { "mkdir", "mkdir <path>",        "create a directory",            cmd_mkdir },
    { "df",    "df",                  "free space on the filesystem",  cmd_df },
};

void cmd_fs_register(void)
{
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
}
