#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t display_init(void);
bool      display_ready(void);
void      display_clear(void);
void      display_text(int row, const char *text);
esp_err_t display_flush(void);

void cmd_disp_register(void);
