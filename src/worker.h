#pragma once

#include "preprocessor.h"
#include <shift_h2.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct raft_handle raft_handle_t;

typedef struct {
    int         worker_id;
    int         worker_core;
    const char *db_path;
    uint16_t    port;
    volatile bool *running; /* points to shared volatile flag */
    bool tls;              /* enable TLS (certs loaded from KV per-worker) */
    const sjs_preprocessor_registry_t *preprocessors;
    raft_handle_t *raft;   /* NULL when Raft is disabled */
} sjs_worker_config_t;

void *sjs_worker_fn(void *arg);
