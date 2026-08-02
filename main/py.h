#pragma once

#include <stdbool.h>
#include "sesame.h"

void py_init(void);
bool py_ready(void);

int  py_run(const char *src, sesame_out_t *out);

void py_interrupt(void);

void cmd_py_register(void);
