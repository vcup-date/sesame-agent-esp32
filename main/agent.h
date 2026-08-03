#pragma once

#include "sesame.h"

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    AGENT_STATUS,   // progress the user should see, not model output
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

// Run one request and write the whole exchange, tool calls included, to a
// sink. This is what the REPL in each shell calls.
esp_err_t agent_ask(const char *text, struct sesame_out *out);

// `agent` with no arguments asks its shell to switch into conversation mode.
// The command itself cannot do that, because reading the next line belongs to
// whichever shell is running: serial, SSH, or neither.
bool agent_take_repl_request(void);

void cmd_agent_register(void);
