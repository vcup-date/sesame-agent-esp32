#pragma once

#include <stdbool.h>
#include "sesame.h"

void py_init(void);
bool py_ready(void);

int  py_run(const char *src, sesame_out_t *out);

void py_interrupt(void);

// Bare `python` asks the shell to open an interactive session.
bool py_take_repl_request(void);

// Like py_run, but echoes the value of a bare expression, as a REPL does.
int py_run_interactive(const char *src, struct sesame_out *out);

void cmd_py_register(void);
