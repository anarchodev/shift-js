#pragma once

#include "code_store.h"
#include <stdbool.h>
#include <stdint.h>

/* Code client — dedicated thread that syncs bytecode from a code server.
 * Pulls full snapshot on startup, then polls for deploy changes. */

typedef struct code_client code_client_t;

typedef struct {
    const char   *host;
    uint16_t      port;
    code_store_t *store;         /* shared store to populate */
    volatile bool *running;      /* shutdown flag */
} code_client_config_t;

/* Create and start the code client thread.
 * Blocks until initial snapshot is pulled (or fails).
 * Returns 0 on success (code_store populated), -1 on error. */
int code_client_start(const code_client_config_t *cfg,
                      code_client_t **out);

/* Stop the code client thread and free resources. */
void code_client_stop(code_client_t *client);
