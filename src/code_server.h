#pragma once

#include "code_store.h"
#include "code_db.h"
#include <stdint.h>

/* Code server configuration. */
typedef struct {
    const char *db_path;     /* code_db SQLite path */
    const char *database_id; /* deployment namespace (default: "default") */
    uint16_t    port;        /* HTTP/2 listen port */
    code_store_t *store;     /* shared bytecode store (updated on deploy) */
} code_server_config_t;

/* Run the code server (blocks until shutdown).
 * Returns 0 on clean shutdown. */
int code_server_run(const code_server_config_t *cfg);

/* Deploy: snapshot working tree, compile all modules, populate code_store.
 * Can be called from the code server or inline from sjs serve.
 * Returns 0 on success. */
int code_server_deploy(code_db_t *cdb, const char *database_id,
                       code_store_t *store);
