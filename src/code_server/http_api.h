#pragma once

#include "code_db.h"
#include "compile.h"
#include "push.h"
#include "../auth.h"

#include <shift.h>
#include <shift_h2.h>

/* Code server context — shared state for all request handlers. */
typedef struct {
    code_db_t     *db;
    compile_ctx_t *cc;
    const char    *auth_secret;
    size_t         auth_secret_len;
} code_server_ctx_t;

/* Handle one HTTP/2 request. Reads method/path/body from the shift entity,
 * routes to the appropriate handler, and sets status/headers/body on the
 * response components. */
void code_server_handle_request(
    code_server_ctx_t *sctx,
    shift_t *sh,
    shift_entity_t e,
    sh2_component_ids_t *comp,
    shift_collection_id_t response_in);
