#include "sesame.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void resolve(const char *in, char *out, size_t n)
{
    if (!in || !in[0]) {
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

static int copy_bytes(const char *src, const char *dst, sesame_out_t *out)
{
    FILE *in = fopen(src, "rb");
    if (!in) {
        sesame_printf(out, "cp: %s: %s\n", src, strerror(errno));
        return -1;
    }
    FILE *o = fopen(dst, "wb");
    if (!o) {
        fclose(in);
        sesame_printf(out, "cp: %s: %s\n", dst, strerror(errno));
        return -1;
    }

    char buf[1024];
    size_t n, total = 0;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, o) != n) { ok = false; break; }
        total += n;
    }
    fclose(in);
    fclose(o);
    if (!ok) {
        sesame_write(out, "cp: short write, filesystem full?\n");
        return -1;
    }
    return (int)total;
}

static int cmd_cp(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 3) { sesame_write(out, "usage: cp <src> <dst>\n"); return 1; }
    char s[256], d[256];
    resolve(argv[1], s, sizeof(s));
    resolve(argv[2], d, sizeof(d));
    int n = copy_bytes(s, d, out);
    if (n < 0) return 1;
    sesame_printf(out, "copied %d bytes to %s\n", n, d);
    return 0;
}

static int cmd_mv(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 3) { sesame_write(out, "usage: mv <src> <dst>\n"); return 1; }
    char s[256], d[256];
    resolve(argv[1], s, sizeof(s));
    resolve(argv[2], d, sizeof(d));
    if (rename(s, d) == 0) {
        sesame_printf(out, "moved to %s\n", d);
        return 0;
    }

    int n = copy_bytes(s, d, out);
    if (n < 0) return 1;
    unlink(s);
    sesame_printf(out, "moved %d bytes to %s\n", n, d);
    return 0;
}

static int cmd_head(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) { sesame_write(out, "usage: head <path> [lines]\n"); return 1; }
    int want = argc > 2 ? atoi(argv[2]) : 10;
    if (want < 1) want = 10;

    char p[256];
    resolve(argv[1], p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f) { sesame_printf(out, "head: %s: %s\n", p, strerror(errno)); return 1; }

    char line[512];
    int n = 0;
    while (n < want && fgets(line, sizeof(line), f)) {
        sesame_write(out, line);
        n++;
    }
    fclose(f);
    if (n && line[strlen(line)-1] != '\n') sesame_write(out, "\n");
    return 0;
}

static int cmd_tail(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) { sesame_write(out, "usage: tail <path> [lines]\n"); return 1; }
    int want = argc > 2 ? atoi(argv[2]) : 10;
    if (want < 1)  want = 10;
    if (want > 60) want = 60;

    char p[256];
    resolve(argv[1], p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f) { sesame_printf(out, "tail: %s: %s\n", p, strerror(errno)); return 1; }

    char **ring = calloc(want, sizeof(char *));
    if (!ring) { fclose(f); return 1; }
    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        int slot = count % want;
        free(ring[slot]);
        ring[slot] = strdup(line);
        count++;
    }
    fclose(f);

    int start = count > want ? count - want : 0;
    for (int i = start; i < count; i++) {
        char *s = ring[i % want];
        if (s) sesame_write(out, s);
    }
    for (int i = 0; i < want; i++) free(ring[i]);
    free(ring);
    return 0;
}

static int cmd_grep(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 3) {
        sesame_write(out, "usage: grep <text> <path>   (plain substring, case sensitive)\n");
        return 1;
    }
    char p[256];
    resolve(argv[2], p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f) { sesame_printf(out, "grep: %s: %s\n", p, strerror(errno)); return 1; }

    char line[512];
    int lineno = 0, hits = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        if (strstr(line, argv[1])) {
            hits++;

            if (hits <= 40) {
                sesame_printf(out, "%d: %s", lineno, line);
                if (line[strlen(line)-1] != '\n') sesame_write(out, "\n");
            }
        }
    }
    fclose(f);
    if (hits > 40) sesame_printf(out, "... %d more matches not shown\n", hits - 40);
    sesame_printf(out, "%d match%s in %d lines\n", hits, hits == 1 ? "" : "es", lineno);
    return 0;
}

static int cmd_wc(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) { sesame_write(out, "usage: wc <path>\n"); return 1; }
    char p[256];
    resolve(argv[1], p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f) { sesame_printf(out, "wc: %s: %s\n", p, strerror(errno)); return 1; }

    long bytes = 0, lines = 0, words = 0;
    bool inword = false;
    int c;
    while ((c = fgetc(f)) != EOF) {
        bytes++;
        if (c == '\n') lines++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            inword = false;
        } else if (!inword) {
            inword = true; words++;
        }
    }
    fclose(f);
    sesame_printf(out, "%ld lines  %ld words  %ld bytes  %s\n", lines, words, bytes, p);
    return 0;
}

static int cmd_touch(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) { sesame_write(out, "usage: touch <path>\n"); return 1; }
    char p[256];
    resolve(argv[1], p, sizeof(p));
    FILE *f = fopen(p, "ab");
    if (!f) { sesame_printf(out, "touch: %s: %s\n", p, strerror(errno)); return 1; }
    fclose(f);
    sesame_printf(out, "%s\n", p);
    return 0;
}

static int cmd_rmdir(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) { sesame_write(out, "usage: rmdir <path>\n"); return 1; }
    char p[256];
    resolve(argv[1], p, sizeof(p));
    if (rmdir(p) != 0) {
        sesame_printf(out, "rmdir: %s: %s (must be empty)\n", p, strerror(errno));
        return 1;
    }
    sesame_printf(out, "removed %s\n", p);
    return 0;
}

static void walk(const char *dir, int depth, sesame_out_t *out, int *files, long *bytes)
{
    if (depth > 3) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char full[320];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        for (int i = 0; i < depth; i++) sesame_write(out, "  ");
        if (S_ISDIR(st.st_mode)) {
            sesame_printf(out, "%s/\n", e->d_name);
            walk(full, depth + 1, out, files, bytes);
        } else {
            sesame_printf(out, "%-24s %6ld\n", e->d_name, (long)st.st_size);
            (*files)++; *bytes += st.st_size;
        }
    }
    closedir(d);
}

static int cmd_tree(int argc, char **argv, sesame_out_t *out)
{
    char p[256];
    resolve(argc > 1 ? argv[1] : NULL, p, sizeof(p));
    int files = 0; long bytes = 0;
    sesame_printf(out, "%s\n", p);
    walk(p, 1, out, &files, &bytes);
    sesame_printf(out, "\n%d file%s, %ld bytes\n", files, files == 1 ? "" : "s", bytes);
    return 0;
}

static int cmd_text(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) {
        sesame_write(out, "usage: text <path> [max_chars]\n"
                          "strips HTML tags and whitespace from a fetched page\n");
        return 1;
    }
    long limit = argc > 2 ? atol(argv[2]) : 4000;
    if (limit < 200)   limit = 200;
    if (limit > 12000) limit = 12000;

    char p[256];
    resolve(argv[1], p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f) { sesame_printf(out, "text: %s: %s\n", p, strerror(errno)); return 1; }

    char line[256];
    int li = 0;
    long emitted = 0;
    bool in_tag = false, space_pending = false;
    int skip_depth = 0;
    char tagbuf[16]; int tl = 0;
    int c;

    while ((c = fgetc(f)) != EOF && emitted < limit) {
        if (c == '<') {
            in_tag = true; tl = 0;
            continue;
        }
        if (in_tag) {
            if (tl < (int)sizeof(tagbuf) - 1 && c != '>' && c != ' ') {
                tagbuf[tl++] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
            }
            if (c == '>') {
                in_tag = false;
                tagbuf[tl] = '\0';
                if (!strcmp(tagbuf,"script") || !strcmp(tagbuf,"style")) skip_depth++;
                else if (!strcmp(tagbuf,"/script") || !strcmp(tagbuf,"/style")) {
                    if (skip_depth) skip_depth--;
                }

                else if (!strcmp(tagbuf,"p") || !strcmp(tagbuf,"br") ||
                         !strcmp(tagbuf,"/p") || !strcmp(tagbuf,"/div") ||
                         !strcmp(tagbuf,"/li") || !strcmp(tagbuf,"/tr") ||
                         (tagbuf[0]=='h' && tagbuf[1]>='1' && tagbuf[1]<='6')) {
                    if (li) { line[li]='\0'; sesame_printf(out,"%s\n",line); emitted+=li+1; li=0; }
                    space_pending = false;
                }
            }
            continue;
        }
        if (skip_depth) continue;

        if (c == '&') {
            char ent[10]; int e = 0;
            while ((c = fgetc(f)) != EOF && c != ';' && e < 9) ent[e++] = c;
            ent[e] = '\0';
            if      (!strcmp(ent,"amp"))  c = '&';
            else if (!strcmp(ent,"lt"))   c = '<';
            else if (!strcmp(ent,"gt"))   c = '>';
            else if (!strcmp(ent,"quot")) c = '"';
            else if (!strcmp(ent,"#39") || !strcmp(ent,"apos")) c = '\'';
            else if (!strcmp(ent,"nbsp")) c = ' ';
            else continue;
        }

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space_pending = (li > 0);
            continue;
        }
        if (space_pending && li < (int)sizeof(line) - 2) { line[li++] = ' '; space_pending = false; }
        if (li < (int)sizeof(line) - 2) {
            line[li++] = (c >= 32 && c < 127) ? c : ' ';
        } else {
            line[li] = '\0'; sesame_printf(out, "%s\n", line); emitted += li + 1; li = 0;
        }
    }
    if (li) { line[li] = '\0'; sesame_printf(out, "%s\n", line); emitted += li; }
    bool more = (c != EOF);
    fclose(f);

    sesame_printf(out, "\n[%ld chars of text%s]\n", emitted,
                  more ? ", truncated — raise the limit or grep instead" : "");
    return 0;
}

static const sesame_cmd_t CMDS[] = {
    { "text",  "text <path> [max_chars]", "HTML to readable text",                cmd_text },
    { "cp",    "cp <src> <dst>",        "copy a file, bytes intact (binary safe)", cmd_cp },
    { "mv",    "mv <src> <dst>",        "rename or move a file",                   cmd_mv },
    { "head",  "head <path> [lines]",   "first lines of a file",                   cmd_head },
    { "tail",  "tail <path> [lines]",   "last lines of a file",                    cmd_tail },
    { "grep",  "grep <text> <path>",    "find lines containing text",              cmd_grep },
    { "wc",    "wc <path>",             "count lines, words and bytes",            cmd_wc },
    { "touch", "touch <path>",          "create an empty file",                    cmd_touch },
    { "rmdir", "rmdir <path>",          "remove an empty directory",               cmd_rmdir },
    { "tree",  "tree [path]",           "list files recursively with sizes",       cmd_tree },
};

void cmd_file_register(void)
{
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
}
