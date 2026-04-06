#include "qjs_snap.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/time.h>

/* ======================================================================
 * Fixed-size bump allocator for per-request JS runtimes
 * ====================================================================== */

typedef struct { size_t size; } bump_hdr_t;

static void *bump_js_malloc(void *opaque, size_t size) {
    qjs_snap_arena_t *a = opaque;
    size_t need = (sizeof(bump_hdr_t) + size + 15) & ~(size_t)15;
    if (a->used + need > QJS_SNAP_ARENA_SIZE)
        return NULL;
    bump_hdr_t *h = (bump_hdr_t *)(a->data + a->used);
    a->used += need;
    h->size = size;
    return h + 1;
}

static void *bump_js_calloc(void *opaque, size_t count, size_t size) {
    size_t total = count * size;
    void *p = bump_js_malloc(opaque, total);
    if (p) memset(p, 0, total);
    return p;
}

static void bump_js_free(void *opaque, void *ptr) {
    (void)opaque; (void)ptr;
}

static void *bump_js_realloc(void *opaque, void *ptr, size_t new_size) {
    if (!ptr) return bump_js_malloc(opaque, new_size);
    if (new_size == 0) return NULL;

    qjs_snap_arena_t *a = opaque;
    bump_hdr_t *old_h = (bump_hdr_t *)ptr - 1;
    size_t old_size = old_h->size;

    size_t old_total = (sizeof(bump_hdr_t) + old_size + 15) & ~(size_t)15;
    char *old_end = (char *)old_h + old_total;
    if (old_end == a->data + a->used) {
        size_t new_total = (sizeof(bump_hdr_t) + new_size + 15) & ~(size_t)15;
        size_t delta = new_total - old_total;
        if (a->used + delta <= QJS_SNAP_ARENA_SIZE) {
            a->used += delta;
            old_h->size = new_size;
            return ptr;
        }
        return NULL;
    }

    void *new_ptr = bump_js_malloc(opaque, new_size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    return new_ptr;
}

static size_t bump_js_usable_size(const void *ptr) {
    const bump_hdr_t *h = (const bump_hdr_t *)ptr - 1;
    return h->size;
}

const JSMallocFunctions qjs_snap_bump_mf = {
    .js_calloc             = bump_js_calloc,
    .js_malloc             = bump_js_malloc,
    .js_free               = bump_js_free,
    .js_realloc            = bump_js_realloc,
    .js_malloc_usable_size = bump_js_usable_size,
};

/* ======================================================================
 * Arena lifecycle
 * ====================================================================== */

qjs_snap_arena_t *qjs_snap_arena_alloc(void) {
    qjs_snap_arena_t *a = malloc(sizeof(qjs_snap_arena_t) + QJS_SNAP_ARENA_SIZE);
    if (a) a->used = 0;
    return a;
}

void qjs_snap_arena_reset(qjs_snap_arena_t *arena) {
    arena->used = 0;
}

void qjs_snap_arena_free(qjs_snap_arena_t *arena) {
    free(arena);
}

/* ======================================================================
 * Snapshot creation: freeze a fully-initialized runtime+context into a
 * relocatable image via two-pass determinism verification.
 * ====================================================================== */

int qjs_snap_create(qjs_snap_arena_t *arena, qjs_snap_init_fn init_fn,
                    void *user_data, qjs_snap_snapshot_t *snap) {
    memset(arena->data, 0, QJS_SNAP_ARENA_SIZE);

    size_t rt_off, ctx_off;
    if (init_fn(arena, &rt_off, &ctx_off, user_data) != 0)
        return -1;

    size_t used_a = arena->used;

    char *data_a = malloc(used_a);
    if (!data_a) return -1;
    memcpy(data_a, arena->data, used_a);

    qjs_snap_arena_t *arena_b = malloc(sizeof(qjs_snap_arena_t) + QJS_SNAP_ARENA_SIZE);
    if (!arena_b) { free(data_a); return -1; }
    memset(arena_b->data, 0, QJS_SNAP_ARENA_SIZE);

    size_t rt_off_b, ctx_off_b;
    if (init_fn(arena_b, &rt_off_b, &ctx_off_b, user_data) != 0) {
        free(arena_b);
        free(data_a);
        return -1;
    }

    size_t used_b = arena_b->used;

    if (used_a != used_b || rt_off != rt_off_b || ctx_off != ctx_off_b) {
        fprintf(stderr, "qjs_snap: snapshot_create: non-deterministic init "
                "(used %zu vs %zu, rt_off %zu vs %zu, ctx_off %zu vs %zu)\n",
                used_a, used_b, rt_off, rt_off_b, ctx_off, ctx_off_b);
        free(arena_b);
        free(data_a);
        return -1;
    }

    /* Build relocation bitmap by diffing the two copies. */
    size_t num_slots = used_a / sizeof(void *);
    snap->bitmap_words = (num_slots + 63) / 64;
    snap->bitmap = calloc(snap->bitmap_words, sizeof(uint64_t));
    if (!snap->bitmap) { free(arena_b); free(data_a); return -1; }

    ptrdiff_t base_delta = arena_b->data - arena->data;
    size_t reloc_count = 0;
    size_t volatile_count = 0;
    size_t error_count = 0;

    for (size_t i = 0; i < num_slots; i++) {
        uint64_t val_a, val_b;
        memcpy(&val_a, data_a        + i * sizeof(void *), sizeof(uint64_t));
        memcpy(&val_b, arena_b->data + i * sizeof(void *), sizeof(uint64_t));

        if (val_a == val_b)
            continue;

        if ((int64_t)(val_b - val_a) == base_delta) {
            snap->bitmap[i / 64] |= (uint64_t)1 << (i % 64);
            reloc_count++;
        } else {
            size_t byte_off = i * sizeof(void *);
            if (volatile_count < QJS_SNAP_MAX_VOLATILE) {
                snap->volatile_offsets[volatile_count++] = byte_off;
                uint64_t zero = 0;
                memcpy(data_a + byte_off, &zero, sizeof(uint64_t));
            } else {
                fprintf(stderr, "qjs_snap: snapshot_create: too many "
                        "non-deterministic slots (slot %zu, max %d)\n",
                        i, QJS_SNAP_MAX_VOLATILE);
                error_count++;
            }
        }
    }

    snap->volatile_count = volatile_count;
    free(arena_b);

    if (error_count > 0) {
        free(snap->bitmap);
        free(data_a);
        return -1;
    }

    /* Safety checks for volatile slots */
    {
        size_t ctx_start = ctx_off;
        size_t ctx_end   = ctx_off + 1024;
        if (ctx_end > used_a) ctx_end = used_a;

        for (size_t i = 0; i < volatile_count; i++) {
            size_t off = snap->volatile_offsets[i];
            if (off < ctx_start || off >= ctx_end) {
                fprintf(stderr, "qjs_snap: FATAL: volatile slot at byte "
                        "offset %zu is outside JSContext [%zu, %zu). "
                        "Likely a QuickJS upstream change — audit the "
                        "snapshot system before proceeding.\n",
                        off, ctx_start, ctx_end);
                abort();
            }
        }
        if (volatile_count > 2) {
            fprintf(stderr, "qjs_snap: FATAL: %zu volatile slots detected "
                    "(expected at most 2: random_state, time_origin). "
                    "Likely a QuickJS upstream change.\n", volatile_count);
            abort();
        }
    }

    snap->data     = data_a;
    snap->used     = used_a;
    snap->old_base = arena->data;
    snap->rt_offset  = rt_off;
    snap->ctx_offset = ctx_off;

    fprintf(stderr, "qjs_snap: snapshot created: %zu bytes, %zu relocations, "
            "%zu volatile slots\n",
            snap->used, reloc_count, volatile_count);

    return 0;
}

/* ======================================================================
 * Snapshot restore: memcpy + bitmap-driven pointer relocation
 * ====================================================================== */

int qjs_snap_restore(const qjs_snap_snapshot_t *snap,
                     qjs_snap_arena_t *arena, void *rt_opaque,
                     JSRuntime **out_rt, JSContext **out_ctx) {
    memcpy(arena->data, snap->data, snap->used);
    arena->used = snap->used;

    ptrdiff_t delta = arena->data - snap->old_base;

    if (delta != 0) {
        for (size_t i = 0; i < snap->bitmap_words; i++) {
            uint64_t word = snap->bitmap[i];
            while (word) {
                int bit = __builtin_ctzll(word);
                size_t offset = ((size_t)i * 64 + (size_t)bit) * sizeof(void *);
                char **slot = (char **)(arena->data + offset);
                *slot += delta;
                word &= word - 1;
            }
        }
    }

    JSRuntime *rt  = (JSRuntime *)(arena->data + snap->rt_offset);
    JSContext *ctx  = (JSContext *)(arena->data + snap->ctx_offset);

    JS_SetRuntimeOpaque(rt, rt_opaque);

    JS_UpdateStackTop(rt);
    JS_SetMaxStackSize(rt, JS_DEFAULT_STACK_SIZE);

    for (size_t i = 0; i < snap->volatile_count; i++) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        uint64_t seed = (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;
        if (seed == 0) seed = 1;
        memcpy(arena->data + snap->volatile_offsets[i], &seed, sizeof(seed));
    }

    *out_rt = rt;
    *out_ctx = ctx;
    return 0;
}

void qjs_snap_destroy(qjs_snap_snapshot_t *snap) {
    free(snap->data);
    free(snap->bitmap);
    memset(snap, 0, sizeof(*snap));
}
