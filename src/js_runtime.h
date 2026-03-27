#pragma once

#include "kvstore.h"
#include "log_db.h"
#include "preprocessor.h"
#include "replay_capture.h"
#include "raft_thread.h"
#include <shift.h>
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

#define SJS_SNAPSHOT_MAX_VOLATILE 8  /* max non-deterministic slots to track */

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
    /* Byte offsets of non-deterministic data slots (e.g. random_state,
     * time_origin, stack_top).  Zeroed in the snapshot, re-initialized
     * after each restore. */
    size_t    volatile_offsets[SJS_SNAPSHOT_MAX_VOLATILE];
    size_t    volatile_count;
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

    /* Set during dispatch for replay capture (module tree recording). */
    sjs_replay_capture_t *current_replay_capture;

    /* Preprocessor registry (shared, read-only). */
    const sjs_preprocessor_registry_t *preprocessors;
} sjs_runtime_t;

/* ======================================================================
 * ECS component types — each manages one concern with its own lifecycle.
 * Registered with shift via sjs_register_components().
 * ====================================================================== */

/* Response headers (destructor frees all strings + arrays) */
typedef struct {
    char    **names;
    char    **values;
    uint32_t  count;
    uint32_t  cap;
} sjs_resp_headers_t;

/* Session state (destructor frees id) */
typedef struct {
    char *id;
    bool  is_new;
    bool  is_dirty;
} sjs_session_t;

/* Random byte capture/replay (destructor frees data) */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    size_t   pos;
} sjs_random_tape_t;

/* Route info (destructor frees all strings) */
typedef struct {
    char *module_path;
    char *func_name;
    char *query_string;
} sjs_route_info_t;

/* Compiled bytecode (destructor frees data) */
typedef struct {
    void  *data;
    size_t len;
} sjs_bytecode_t;

/* Response status (constructor sets code = 200) */
typedef struct {
    uint16_t code;
} sjs_resp_status_t;

/* Raft sequence number — tags an entity with its proposal seq for
 * the pending response collection. */
typedef struct {
    uint64_t seq;
} sjs_raft_seq_t;

/* Component IDs for sjs ECS components */
typedef struct {
    shift_component_id_t resp_headers;
    shift_component_id_t session;
    shift_component_id_t random_tape;
    shift_component_id_t route;
    shift_component_id_t bytecode;
    shift_component_id_t resp_status;
    shift_component_id_t raft_seq;
} sjs_component_ids_t;

/* Per-request view — thin struct of pointers into ECS components.
 * Passed to JS globals via JS_SetContextOpaque(). */
typedef struct sjs_request_ctx {
    /* Borrowed from sh2 components (read-only request data) */
    const char               *method;
    const char               *path;
    const sh2_header_field_t *headers;
    uint32_t                  header_count;
    const void               *body;
    uint32_t                  body_len;

    /* Tenant KV prefix (e.g. "tenants/acme/"). NULL for system domain. */
    const char *kv_prefix;

    /* Pointers into sjs ECS components (mutable) */
    sjs_resp_headers_t  *resp_hdrs;
    sjs_resp_status_t   *resp_st;
    sjs_session_t       *session;
    sjs_random_tape_t   *tape;

    /* Raft write-set tracking (NULL when Raft disabled).
     * Writes are also applied to local SQLite immediately for
     * read-your-own-writes; the write-set is sent to the Raft thread
     * for replication after the handler completes. */
    raft_write_set_t    *write_set;
    raft_handle_t       *raft;
    uint64_t             raft_seq;  /* kv_seq for this request, 0 = not yet assigned */

    /* Logging */
    log_db_t            *log_db;       /* per-worker log DB handle */
    log_batch_t         *log_batch;    /* per-request pending entries */
    uint64_t             request_id;   /* monotonic per-worker counter */

    /* Replay capture */
    sjs_replay_capture_t *replay_capture;
} sjs_request_ctx_t;

/* Create/destroy the per-worker runtime. */
int  sjs_runtime_init(sjs_runtime_t *sjs, kvstore_t *kv,
                      const sjs_preprocessor_registry_t *preprocessors);
void sjs_runtime_free(sjs_runtime_t *sjs);

/* Register sjs ECS components with the shift context.
 * Call after sh2_register_components(). */
int sjs_register_components(shift_t *sh, sjs_component_ids_t *out);

/* Reset response headers for transaction retry (frees strings, keeps arrays). */
void sjs_resp_headers_reset(sjs_resp_headers_t *h);

/* Execute a module's handler for the given HTTP verb.
 * Route and bytecode components are populated by the caller or by this function.
 * Returns a malloc'd response body string (caller frees), or NULL on error.
 * out_len receives the body length. */
char *sjs_dispatch(sjs_runtime_t *sjs, sjs_request_ctx_t *req,
                   sjs_route_info_t *route, sjs_bytecode_t *bc,
                   uint32_t *out_len);
