#include "agent.h"
#include "sesame.h"
#include "cfg.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_vfs_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"

static const char *TAG = "console";

static void completion(const char *buf, linenoiseCompletions *lc)
{
    size_t n = strlen(buf);
    for (int i = 0; i < sesame_cmd_count(); i++) {
        const sesame_cmd_t *c = sesame_cmd_at(i);
        if (n == 0 || strncmp(buf, c->name, n) == 0) {
            linenoiseAddCompletion(lc, c->name);
        }
    }
}

static char *hints(const char *buf, int *color, int *bold)
{
    const sesame_cmd_t *c = sesame_lookup(buf);
    if (!c) {
        return NULL;
    }
    *color = 33;
    *bold = 0;

    const char *tail = c->usage + strlen(c->name);
    return (char *)tail;
}

static void console_task(void *arg)
{
    (void)arg;

    char prompt[48];
    snprintf(prompt, sizeof(prompt), "%s> ", cfg_get(CFG_DEV_NAME, "sesame"));

    if (linenoiseProbe() != 0) {
        linenoiseSetDumbMode(1);
    }

    const esp_app_desc_t *app = esp_app_get_description();
    sesame_banner(sesame_stdout(), app ? app->version : NULL);
    printf("  type 'help' for commands, 'agent' to talk to it\n\n");

    // Conversation mode, the same one SSH offers. `agent` enters it, `exit`
    // leaves it, and in between every line goes to the model rather than the
    // command table.
    bool repl = false;
    char agent_prompt[64];
    snprintf(agent_prompt, sizeof(agent_prompt), "%s ask> ",
             cfg_get(CFG_DEV_NAME, "sesame"));

    while (1) {
        char *line = linenoise(repl ? agent_prompt : prompt);
        if (line == NULL) {

            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (line[0] != '\0') {
            linenoiseHistoryAdd(line);
            if (repl) {
                if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
                    repl = false;
                    printf("left conversation mode\n");
                } else {
                    agent_ask(line, sesame_stdout());
                }
            } else {
                agent_take_repl_request();   // drop anything another transport left
                sesame_exec(line, sesame_stdout());
                if (agent_take_repl_request()) {
                    repl = true;
                    printf("conversation mode. ask in plain language; "
                           "'exit' returns to the shell.\n");
                }
            }
        }
        linenoiseFree(line);
    }
}

void console_start(void)
{

    setvbuf(stdin, NULL, _IONBF, 0);
    uart_vfs_dev_port_set_rx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CRLF);

    const uart_config_t uart_cfg = {
        .baud_rate  = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CONFIG_ESP_CONSOLE_UART_NUM, &uart_cfg));
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);

    linenoiseSetMultiLine(1);
    linenoiseHistorySetMaxLen(32);
    linenoiseSetCompletionCallback(&completion);
    linenoiseSetHintsCallback(&hints);
    linenoiseSetMaxLineLen(512);

    if (xTaskCreate(console_task, "console", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the console task");
    }
}
