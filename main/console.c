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
    printf("  type 'help' for commands, 'ask <request>' for the agent\n\n");

    while (1) {
        char *line = linenoise(prompt);
        if (line == NULL) {

            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (line[0] != '\0') {
            linenoiseHistoryAdd(line);
            sesame_exec(line, sesame_stdout());
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
