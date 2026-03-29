#pragma once

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
    const char *auth_secret;   /* shared HMAC secret for auth cookies */
    const char *mgmt_secret;   /* service-to-service secret for /_mgmt/ */
    const char *log_server;    /* "host:port" for log forwarding, or NULL */
    raft_handle_t *raft;       /* NULL when Raft is disabled */
} sjs_worker_config_t;

void *sjs_worker_fn(void *arg);
