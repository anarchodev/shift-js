#pragma once

#include "preprocessor.h"
#include <shift_h2.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct raft_handle raft_handle_t;
typedef struct code_db code_db_t;
typedef struct code_store code_store_t;

typedef struct {
    int         worker_id;
    int         num_workers;
    int         worker_core;
    const char *db_path;
    uint16_t    port;
    volatile bool *running; /* points to shared volatile flag */
    bool tls;              /* enable TLS (certs loaded from KV per-worker) */
    const sjs_preprocessor_registry_t *preprocessors;
    raft_handle_t *raft;   /* NULL when Raft is disabled */
    bool no_log;           /* disable logging and replay capture */
    code_db_t    *code_db;    /* shared code database (may be NULL) */
    code_store_t *code_store; /* shared bytecode store (may be NULL) */
    const char   *code_server_host; /* proxy target (NULL = local) */
    uint16_t      code_server_port;
} sjs_worker_config_t;

void *sjs_worker_fn(void *arg);
