#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    AGENT_TEXT,
    AGENT_TOOL_CALL,
    AGENT_TOOL_RESULT,
    AGENT_ERROR,
    AGENT_DONE,
} agent_ev_t;

typedef void (*agent_emit_fn)(void *ctx, agent_ev_t ev, const char *text);

esp_err_t agent_run(const char *user_msg, agent_emit_fn emit, void *ctx);

void agent_reset(void);

void agent_stop(void);

int agent_turns(void);
int agent_context_chars(void);

int agent_prompt_bytes(void);

bool agent_busy(void);

void cmd_agent_register(void);
