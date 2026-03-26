#pragma once

#include "kvstore.h"
#include "preprocessor.h"
#include <shift_h2.h>
#include <quickjs.h>
#include <stddef.h>
#include <stdbool.h>

/* Fixed-size arena for per-request JS runtimes.
 * One arena per worker — reset between requests. */
#define SJS_ARENA_SIZE (10 * 1024 * 1024)  /* 10 MB per arena */

typedef struct sjs_arena {
    size_t  used;
    char    data[];          /* flexible array — SJS_ARENA_SIZE bytes */
} sjs_arena_t;

/* Frozen snapshot of a JS runtime+context with intrinsics set up.
 * Created once at worker init. Restored per-request via memcpy + relocation. */
typedef struct {
    void     *data;         /* saved arena content */
    size_t    used;         /* bytes used in arena */
    uint64_t *bitmap;       /* relocation bitmap: 1 bit per 8-byte slot */
    size_t    bitmap_words; /* number of uint64_t words in bitmap */
    char     *old_base;     /* arena data base when snapshot was taken */
    /* Offsets of JSRuntime* and JSContext* within the arena data,
     * so we can find them after restore without searching. */
    size_t    rt_offset;
    size_t    ctx_offset;
} sjs_snapshot_t;

/* Per-worker JS state — one per thread, long-lived. */
typedef struct {
    /* Long-lived compiler runtime (standard allocator). */
    JSRuntime *compile_rt;
    JSContext *compile_ctx;

    /* Frozen snapshot for fast per-request context creation. */
    sjs_snapshot_t snapshot;

    /* Single pre-allocated arena, reset between requests. */
    sjs_arena_t *arena;

    kvstore_t *kv;

    /* Set during compilation for module loader prefix. */
    const char *current_prefix;

    /* Preprocessor registry (shared, read-only). */
    const sjs_preprocessor_registry_t *preprocessors;
} sjs_runtime_t;

/* Per-request context passed through to JS globals. */
typedef struct sjs_request_ctx {
    /* Request info (read from sh2 components) */
    const char             *method;
    const char             *path;
    const sh2_header_field_t *headers;
    uint32_t                header_count;
    const void             *body;
    uint32_t                body_len;

    /* Tenant KV prefix (e.g. "tenants/acme/"). NULL for system domain. */
    const char *kv_prefix;

    /* Response state (written by JS, read by C to build sh2 response) */
    uint16_t  resp_status;
    char    **resp_header_names;
    char    **resp_header_values;
    uint32_t  resp_header_count;
    uint32_t  resp_header_cap;

    /* Session state */
    char   *session_id;       /* heap-allocated session ID, or NULL */
    bool    session_new;      /* true if we generated a new session ID */
    bool    session_dirty;    /* true if session data was modified by JS */

    /* Random byte capture/replay for deterministic replay */
    uint8_t  *random_tape;        /* malloc'd buffer */
    size_t    random_tape_len;    /* bytes written (capture) or total (replay) */
    size_t    random_tape_cap;    /* allocated capacity */
    size_t    random_tape_pos;    /* read position (replay mode) */
    bool      random_tape_replay; /* true = replay from tape, false = capture */
} sjs_request_ctx_t;

/* Create/destroy the per-worker runtime. */
int  sjs_runtime_init(sjs_runtime_t *sjs, kvstore_t *kv,
                      const sjs_preprocessor_registry_t *preprocessors);
void sjs_runtime_free(sjs_runtime_t *sjs);

/* Execute a module's handler for the given HTTP verb.
 * Returns a malloc'd response body string (caller frees), or NULL on error.
 * out_len receives the body length. */
char *sjs_dispatch(sjs_runtime_t *sjs, sjs_request_ctx_t *req,
                   uint32_t *out_len);
