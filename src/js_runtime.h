#pragma once

#include "kvstore.h"
#include <shift_h2.h>
#include <quickjs.h>

/* Per-worker JS runtime — one per thread, long-lived. */
typedef struct {
    JSRuntime *rt;
    kvstore_t *kv;
} sjs_runtime_t;

/* Per-request context passed through to JS globals. */
typedef struct {
    /* Request info (read from sh2 components) */
    const char             *method;
    const char             *path;
    const sh2_header_field_t *headers;
    uint32_t                header_count;
    const void             *body;
    uint32_t                body_len;

    /* Response state (written by JS, read by C to build sh2 response) */
    uint16_t  resp_status;
    char    **resp_header_names;
    char    **resp_header_values;
    uint32_t  resp_header_count;
    uint32_t  resp_header_cap;
} sjs_request_ctx_t;

/* Create/destroy the per-worker runtime. */
int  sjs_runtime_init(sjs_runtime_t *sjs, kvstore_t *kv);
void sjs_runtime_free(sjs_runtime_t *sjs);

/* Execute a module's handler for the given HTTP verb.
 * Returns a malloc'd response body string (caller frees), or NULL on error.
 * out_len receives the body length. */
char *sjs_dispatch(sjs_runtime_t *sjs, sjs_request_ctx_t *req,
                   uint32_t *out_len);
