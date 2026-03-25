#pragma once

#include "kvstore.h"
#include "preprocessor.h"
#include <shift_h2.h>
#include <quickjs.h>
#include <stddef.h>

/* Fixed-size arena for per-request JS runtimes.
 * Allocated once up front, never reallocated (no pointer invalidation).
 * Managed in a free list per worker. */
#define SJS_ARENA_SIZE (256 * 1024)  /* 256 KB per arena */
#define SJS_ARENA_POOL  16           /* pre-allocated arenas per worker */

typedef struct sjs_arena {
    struct sjs_arena *next;  /* free list link */
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

    /* Free list of pre-allocated fixed-size arenas. */
    sjs_arena_t *arena_free;
    /* Backing allocation for the pool (one contiguous block). */
    void *arena_pool;

    kvstore_t *kv;

    /* Set during compilation for module loader prefix. */
    const char *current_prefix;

    /* Preprocessor registry (shared, read-only). */
    const sjs_preprocessor_registry_t *preprocessors;
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

    /* Tenant KV prefix (e.g. "tenants/acme/"). NULL for system domain. */
    const char *kv_prefix;

    /* Response state (written by JS, read by C to build sh2 response) */
    uint16_t  resp_status;
    char    **resp_header_names;
    char    **resp_header_values;
    uint32_t  resp_header_count;
    uint32_t  resp_header_cap;
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
