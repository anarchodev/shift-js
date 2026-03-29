#pragma once

#include "log_store.h"
#include "../auth.h"

#include <shift.h>
#include <shift_h2.h>

typedef struct {
    log_store_t *ls;
    const char  *auth_secret;
    size_t       auth_secret_len;
} log_server_ctx_t;

void log_server_handle_request(
    log_server_ctx_t *ctx,
    shift_t *sh,
    shift_entity_t e,
    sh2_component_ids_t *comp,
    shift_collection_id_t response_in);
