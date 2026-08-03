#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SESAME_MOUNT     "/sesame"

#define SESAME_MAX_CMDS  96
#define SESAME_MAX_ARGV  16

#define SESAME_MAX_OUT   (48 * 1024)

typedef struct sesame_out {
    void (*write)(struct sesame_out *o, const char *data, size_t len);
    void *ctx;
} sesame_out_t;

void sesame_write(sesame_out_t *o, const char *s);
void sesame_printf(sesame_out_t *o, const char *fmt, ...);

typedef int (*sesame_fn)(int argc, char **argv, sesame_out_t *out);

typedef struct {
    const char *name;
    const char *usage;
    const char *help;
    sesame_fn   fn;

    bool        raw;
} sesame_cmd_t;

void               sesame_register(const sesame_cmd_t *cmd);
const sesame_cmd_t *sesame_lookup(const char *name);
const sesame_cmd_t *sesame_cmd_at(int i);
int                sesame_cmd_count(void);

int sesame_exec(const char *line, sesame_out_t *out);

char *sesame_capture(const char *line, int *rc);

sesame_out_t *sesame_stdout(void);

void sesame_bar(sesame_out_t *out, const char *label,
                uint64_t used, uint64_t total, int width);
void sesame_spark(sesame_out_t *out, const int *values, int n);
void sesame_banner(sesame_out_t *out, const char *version);

void cmd_sys_register(void);
void cmd_fs_register(void);
void cmd_hw_register(void);
void cmd_periph_register(void);
void cmd_pins_register(void);
void cmd_ble_register(void);
void cmd_extra_register(void);
void cmd_file_register(void);
void cmd_netcmds_register(void);
void cmd_peer_register(void);
void cmd_motor_register(void);
void cmd_sniff_register(void);
