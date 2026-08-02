#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

i2c_master_bus_handle_t camera_i2c_bus(void);

int camera_pins(int *out, int max);

esp_err_t camera_init(void);
bool      camera_ready(void);

esp_err_t camera_snap_to_file(const char *path, size_t *size, int *w, int *h);

void cmd_cam_register(void);
