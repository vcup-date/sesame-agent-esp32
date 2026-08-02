#include "sesame.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/mcpwm_cmpr.h"
#include "driver/mcpwm_gen.h"
#include "driver/mcpwm_oper.h"
#include "driver/mcpwm_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "motor";

static bool ok_pin(int pin)
{
    return pin >= 0 && pin <= 48 &&
           !(pin >= 26 && pin <= 32) && !(pin >= 33 && pin <= 37);
}

#define SERVO_MIN_US   500
#define SERVO_MAX_US   2500
#define SERVO_PERIOD   20000

typedef struct {
    int pin;
    mcpwm_timer_handle_t timer;
    mcpwm_oper_handle_t  oper;
    mcpwm_cmpr_handle_t  cmpr;
    mcpwm_gen_handle_t   gen;
} servo_t;

#define MAX_SERVOS 4
static servo_t s_servos[MAX_SERVOS];
static int     s_nservos;

static servo_t *servo_for(int pin, sesame_out_t *out)
{
    for (int i = 0; i < s_nservos; i++) {
        if (s_servos[i].pin == pin) {
            return &s_servos[i];
        }
    }
    if (s_nservos >= MAX_SERVOS) {
        sesame_printf(out, "servo: already driving %d servos\n", MAX_SERVOS);
        return NULL;
    }

    servo_t *s = &s_servos[s_nservos];
    memset(s, 0, sizeof(*s));

    mcpwm_timer_config_t tcfg = {
        .group_id      = 0,
        .clk_src       = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .count_mode    = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks  = SERVO_PERIOD,
    };
    if (mcpwm_new_timer(&tcfg, &s->timer) != ESP_OK) {
        sesame_write(out, "servo: no free MCPWM timer\n");
        return NULL;
    }

    mcpwm_operator_config_t ocfg = { .group_id = 0 };
    if (mcpwm_new_operator(&ocfg, &s->oper) != ESP_OK) {
        mcpwm_del_timer(s->timer);
        sesame_write(out, "servo: no free MCPWM operator\n");
        return NULL;
    }
    mcpwm_operator_connect_timer(s->oper, s->timer);

    mcpwm_comparator_config_t ccfg = { .flags.update_cmp_on_tez = true };
    mcpwm_new_comparator(s->oper, &ccfg, &s->cmpr);

    mcpwm_generator_config_t gcfg = { .gen_gpio_num = pin };
    if (mcpwm_new_generator(s->oper, &gcfg, &s->gen) != ESP_OK) {
        mcpwm_del_operator(s->oper);
        mcpwm_del_timer(s->timer);
        sesame_printf(out, "servo: cannot drive GPIO %d\n", pin);
        return NULL;
    }

    mcpwm_generator_set_action_on_timer_event(s->gen,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                     MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH));
    mcpwm_generator_set_action_on_compare_event(s->gen,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                       s->cmpr, MCPWM_GEN_ACTION_LOW));

    mcpwm_timer_enable(s->timer);
    mcpwm_timer_start_stop(s->timer, MCPWM_TIMER_START_NO_STOP);

    s->pin = pin;
    s_nservos++;
    ESP_LOGI(TAG, "servo channel on GPIO %d", pin);
    return s;
}

static int cmd_servo(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 3) {
        sesame_write(out,
            "usage: servo <pin> <angle 0-180> [min_us] [max_us]\n"
            "       servo <pin> us <microseconds>\n"
            "       servo release <pin>\n"
            "50Hz pulse width, the unit servos are actually specified in.\n"
            "Power a servo from its own 5V supply, not the board's 3V3 —\n"
            "a stalled servo draws more than the regulator here can give.\n");
        return 1;
    }

    if (strcmp(argv[1], "release") == 0) {
        int pin = atoi(argv[2]);
        for (int i = 0; i < s_nservos; i++) {
            if (s_servos[i].pin == pin) {

                mcpwm_timer_start_stop(s_servos[i].timer, MCPWM_TIMER_STOP_EMPTY);
                mcpwm_timer_disable(s_servos[i].timer);
                mcpwm_del_generator(s_servos[i].gen);
                mcpwm_del_comparator(s_servos[i].cmpr);
                mcpwm_del_operator(s_servos[i].oper);
                mcpwm_del_timer(s_servos[i].timer);
                s_servos[i] = s_servos[--s_nservos];
                sesame_printf(out, "servo %d released\n", pin);
                return 0;
            }
        }
        sesame_printf(out, "servo: %d is not driven\n", pin);
        return 1;
    }

    int pin = atoi(argv[1]);
    if (!ok_pin(pin)) {
        sesame_printf(out, "servo: %d is not a usable pin on this chip\n", pin);
        return 1;
    }

    int us;
    if (strcmp(argv[2], "us") == 0) {
        if (argc < 4) {
            sesame_write(out, "usage: servo <pin> us <microseconds>\n");
            return 1;
        }
        us = atoi(argv[3]);

        if (us < 400)  us = 400;
        if (us > 2600) us = 2600;
    } else {
        int angle = atoi(argv[2]);
        if (angle < 0)   angle = 0;
        if (angle > 180) angle = 180;
        int lo = argc > 3 ? atoi(argv[3]) : SERVO_MIN_US;
        int hi = argc > 4 ? atoi(argv[4]) : SERVO_MAX_US;
        if (lo < 400)  lo = 400;
        if (hi > 2600) hi = 2600;
        if (hi <= lo)  { lo = SERVO_MIN_US; hi = SERVO_MAX_US; }
        us = lo + (hi - lo) * angle / 180;
    }

    servo_t *s = servo_for(pin, out);
    if (!s) {
        return 1;
    }
    if (mcpwm_comparator_set_compare_value(s->cmpr, us) != ESP_OK) {
        sesame_write(out, "servo: could not set the pulse width\n");
        return 1;
    }
    sesame_printf(out, "servo %d: %d us (%d deg)\n", pin, us,
                  (us - SERVO_MIN_US) * 180 / (SERVO_MAX_US - SERVO_MIN_US));
    return 0;
}

static const uint8_t HALF_STEP[8][4] = {
    {1,0,0,0}, {1,1,0,0}, {0,1,0,0}, {0,1,1,0},
    {0,0,1,0}, {0,0,1,1}, {0,0,0,1}, {1,0,0,1},
};

static int cmd_stepper(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) {
        sesame_write(out,
            "usage: stepper step <step_pin> <dir_pin> <steps> [rpm]\n"
            "       stepper 4wire <p1> <p2> <p3> <p4> <steps> [rpm]\n"
            "negative steps reverse. 'step' drives an A4988/DRV8825-style\n"
            "driver; '4wire' drives a 28BYJ-48 through a ULN2003.\n"
            "Motors need their own supply — do not run one off the 3V3 pin.\n");
        return 1;
    }

    bool four = strcmp(argv[1], "4wire") == 0;
    if (!four && strcmp(argv[1], "step") != 0) {
        sesame_write(out, "usage: stepper step|4wire ...\n");
        return 1;
    }
    if ((four && argc < 7) || (!four && argc < 5)) {
        sesame_write(out, "stepper: not enough arguments\n");
        return 1;
    }

    int pins[4], npins = four ? 4 : 2;
    for (int i = 0; i < npins; i++) {
        pins[i] = atoi(argv[2 + i]);
        if (!ok_pin(pins[i])) {
            sesame_printf(out, "stepper: %d is not a usable pin on this chip\n", pins[i]);
            return 1;
        }
    }

    int steps = atoi(argv[2 + npins]);
    int rpm   = argc > 3 + npins ? atoi(argv[3 + npins]) : 15;
    bool back = steps < 0;
    if (back) steps = -steps;
    if (steps > 4096) steps = 4096;
    if (rpm < 1)  rpm = 1;
    if (rpm > 300) rpm = 300;

    int steps_per_rev = four ? 4096 : 200;
    int delay_us = 60L * 1000000L / ((long)rpm * steps_per_rev);
    if (delay_us < 800)    delay_us = 800;
    if (delay_us > 20000)  delay_us = 20000;

    for (int i = 0; i < npins; i++) {
        gpio_reset_pin(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(pins[i], 0);
    }

    int64_t total_ms = (int64_t)steps * delay_us / 1000;
    if (total_ms > 25000) {
        sesame_printf(out, "stepper: that move would take %llds; "
                           "reduce steps or raise rpm\n", total_ms / 1000);
        return 1;
    }

    if (four) {
        for (int i = 0; i < steps; i++) {
            const uint8_t *phase = HALF_STEP[back ? (7 - (i % 8)) : (i % 8)];
            for (int p = 0; p < 4; p++) {
                gpio_set_level(pins[p], phase[p]);
            }

            esp_rom_delay_us(delay_us);
        }

        for (int p = 0; p < 4; p++) {
            gpio_set_level(pins[p], 0);
        }
    } else {
        gpio_set_level(pins[1], back ? 1 : 0);
        esp_rom_delay_us(5);
        for (int i = 0; i < steps; i++) {
            gpio_set_level(pins[0], 1);
            esp_rom_delay_us(5);
            gpio_set_level(pins[0], 0);
            esp_rom_delay_us(delay_us);
        }
    }

    sesame_printf(out, "%d steps %s at %d rpm (%lld ms)\n",
                  steps, back ? "reverse" : "forward", rpm, total_ms);
    return 0;
}

static const sesame_cmd_t CMDS[] = {
    { "servo",   "servo <pin> <angle 0-180> | <pin> us <n> | release <pin>",
      "position a hobby servo (precise 50Hz pulse)", cmd_servo },
    { "stepper", "stepper step <step> <dir> <n> [rpm] | 4wire <p1> <p2> <p3> <p4> <n> [rpm]",
      "turn a stepper motor, either driver style", cmd_stepper },
};

void cmd_motor_register(void)
{
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
}
