#pragma once

#include <stdbool.h>

typedef enum {
    LED_OFF,
    LED_IDLE,
    LED_THINKING,
    LED_OK,
    LED_ERROR,
    LED_MANUAL,
} led_mode_t;

void led_init(void);
void led_set(led_mode_t mode);

bool led_rgb(int r, int g, int b, int pin);

void cmd_led_register(void);
