#pragma once

#include <quickjs.h>
#include <stddef.h>
#include <stdint.h>

#define QJS_SNAP_ARENA_SIZE (10 * 1024 * 1024)  /* 10 MB per arena */
#define QJS_SNAP_MAX_VOLATILE 8  /* max non-deterministic slots to track */

typedef struct qjs_snap_arena {
    size_t used;
    char   data[];          /* flexible array — QJS_SNAP_ARENA_SIZE bytes */
} qjs_snap_arena_t;

/* Frozen snapshot of a JS runtime+context.
 * Created once, restored per-request via memcpy + pointer relocation. */
typedef struct {
    void     *data;         /* saved arena content */
    size_t    used;         /* bytes used in arena */
    uint64_t *bitmap;       /* relocation bitmap: 1 bit per 8-byte slot */
    size_t    bitmap_words; /* number of uint64_t words in bitmap */
    char     *old_base;     /* arena data base when snapshot was taken */
    size_t    rt_offset;    /* JSRuntime* offset within arena data */
    size_t    ctx_offset;   /* JSContext* offset within arena data */
    size_t    volatile_offsets[QJS_SNAP_MAX_VOLATILE];
    size_t    volatile_count;
} qjs_snap_snapshot_t;

/* Bump allocator — pass to JS_NewRuntime2(&qjs_snap_bump_mf, arena). */
extern const JSMallocFunctions qjs_snap_bump_mf;

/* Arena lifecycle. */
qjs_snap_arena_t *qjs_snap_arena_alloc(void);
void              qjs_snap_arena_reset(qjs_snap_arena_t *arena);
void              qjs_snap_arena_free(qjs_snap_arena_t *arena);

/* Callback for runtime initialization inside an arena.
 * Must create JSRuntime via JS_NewRuntime2(&qjs_snap_bump_mf, arena),
 * add desired intrinsics/globals, and report rt/ctx offsets.
 * Return 0 on success, -1 on failure. */
typedef int (*qjs_snap_init_fn)(qjs_snap_arena_t *arena,
                                size_t *out_rt_offset,
                                size_t *out_ctx_offset,
                                void *user_data);

/* Create snapshot by running init_fn twice for determinism verification. */
int  qjs_snap_create(qjs_snap_arena_t *arena, qjs_snap_init_fn init_fn,
                     void *user_data, qjs_snap_snapshot_t *out);

/* Restore snapshot into arena. Sets rt_opaque via JS_SetRuntimeOpaque. */
int  qjs_snap_restore(const qjs_snap_snapshot_t *snap,
                      qjs_snap_arena_t *arena, void *rt_opaque,
                      JSRuntime **out_rt, JSContext **out_ctx);

/* Free snapshot data. */
void qjs_snap_destroy(qjs_snap_snapshot_t *snap);
