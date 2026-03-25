#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int         worker_id;
    int         worker_core;
    const char *db_path;
    uint16_t    port;
    volatile bool *running; /* points to shared volatile flag */
} sjs_worker_config_t;

void *sjs_worker_fn(void *arg);
