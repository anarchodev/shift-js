#define _GNU_SOURCE
#include "js_runtime.h"
#include "js_globals.h"
#include "crypto.h"
#include "kvstore.h"
#include "preprocessor.h"
#include "replay_capture.h"
#include "typescript.h"

#include <quickjs.h>
#include <openssl/rand.h>
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
    sjs_arena_t *a = opaque;
    size_t need = (sizeof(bump_hdr_t) + size + 15) & ~(size_t)15;
    if (a->used + need > SJS_ARENA_SIZE)
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

    sjs_arena_t *a = opaque;
    bump_hdr_t *old_h = (bump_hdr_t *)ptr - 1;
    size_t old_size = old_h->size;

    size_t old_total = (sizeof(bump_hdr_t) + old_size + 15) & ~(size_t)15;
    char *old_end = (char *)old_h + old_total;
    if (old_end == a->data + a->used) {
        size_t new_total = (sizeof(bump_hdr_t) + new_size + 15) & ~(size_t)15;
        size_t delta = new_total - old_total;
        if (a->used + delta <= SJS_ARENA_SIZE) {
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

static const JSMallocFunctions bump_mf = {
    .js_calloc             = bump_js_calloc,
    .js_malloc             = bump_js_malloc,
    .js_free               = bump_js_free,
    .js_realloc            = bump_js_realloc,
    .js_malloc_usable_size = bump_js_usable_size,
};

void arena_reset(sjs_runtime_t *sjs) {
    sjs->arena->used = 0;
}

/* ======================================================================
 * Module loader (compile context only)
 * ====================================================================== */

static JSModuleDef *sjs_module_loader(JSContext *ctx, const char *module_name,
                                      void *opaque) {
    sjs_runtime_t *sjs = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    kvstore_t *kv = sjs ? sjs->kv : NULL;
    if (!kv) return NULL;

    void  *source = NULL;
    size_t source_len = 0;
    const char *resolved_name = module_name;
    char *resolved_alloc = NULL;

    const char *ext = sjs_path_extension(module_name);

    if (ext) {
        char raw_key[256];
        snprintf(raw_key, sizeof(raw_key), "__code/%s", module_name);

        char key_buf[512];
        const char *key = sjs_prefixed_key(sjs->current_prefix, raw_key,
                                            key_buf, sizeof(key_buf));
        if (!key) return NULL;

        if (kv_get(kv, key, &source, &source_len) != 0) return NULL;
    } else {
        resolved_alloc = sjs_resolve_with_extensions(
            sjs->preprocessors, kv, module_name,
            sjs->current_prefix, &source, &source_len);
        if (!resolved_alloc) return NULL;
        resolved_name = resolved_alloc;
        ext = sjs_path_extension(resolved_name);
    }

    char  *compile_source = source;
    size_t compile_len    = source_len;

    const sjs_preprocessor_entry_t *pp = ext
        ? sjs_preprocessor_find(sjs->preprocessors, ext)
        : NULL;

    if (pp) {
        if (pp->transform == sjs_typescript_transform) {
            sjs_ts_binding_t *binding = pp->user_data;
            if (binding && binding->ts_ctx)
                binding->ts_ctx->current_module_path = resolved_name;
        }
        size_t js_len;
        char *js = pp->transform(source, source_len, &js_len, pp->user_data);
        free(source);
        if (!js) { free(resolved_alloc); return NULL; }
        compile_source = js;
        compile_len    = js_len;

        /* Store transpiled JS for browser replay/debugging */
        {
            char js_key[512];
            snprintf(js_key, sizeof(js_key), "__code_js/%s", resolved_name);
            char jk[512];
            const char *jkey = kv_prefixed_key(sjs->current_prefix, js_key,
                                                jk, sizeof(jk));
            if (jkey)
                kv_put(kv, jkey, js, js_len);
        }

        /* Store source map if TypeScript preprocessor produced one */
        if (pp->transform == sjs_typescript_transform) {
            sjs_ts_binding_t *binding = pp->user_data;
            if (binding && binding->last_source_map) {
                char sm_key[512];
                snprintf(sm_key, sizeof(sm_key),
                         "__code_sourcemap/%s", resolved_name);
                char pk[512];
                const char *key = kv_prefixed_key(sjs->current_prefix, sm_key,
                                                   pk, sizeof(pk));
                if (key)
                    kv_put(kv, key, binding->last_source_map,
                           binding->last_source_map_len);
                free(binding->last_source_map);
                binding->last_source_map = NULL;
                binding->last_source_map_len = 0;
            }
        }
    }

    JSValue func = JS_Eval(ctx, compile_source, compile_len, resolved_name,
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(compile_source);

    if (JS_IsException(func)) { free(resolved_alloc); return NULL; }

    size_t bc_len;
    uint8_t *bc = JS_WriteObject(ctx, &bc_len, func,
                                 JS_WRITE_OBJ_BYTECODE);
    if (bc) {
        char raw_cache[256];
        snprintf(raw_cache, sizeof(raw_cache), "__compiled/%s", resolved_name);
        char cache_buf[512];
        const char *ck = sjs_prefixed_key(sjs->current_prefix, raw_cache,
                                          cache_buf, sizeof(cache_buf));
        if (ck)
            kv_put(sjs->kv, ck, bc, bc_len);
        js_free(ctx, bc);
    }

    free(resolved_alloc);

    JSModuleDef *mod = JS_VALUE_GET_PTR(func);
    JS_FreeValue(ctx, func);
    return mod;
}

/* ======================================================================
 * Request-time module loader
 * ====================================================================== */

JSModuleDef *sjs_request_module_loader(JSContext *ctx,
                                       const char *module_name,
                                       void *opaque) {
    sjs_runtime_t *sjs = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    if (!sjs || !sjs->kv) return NULL;

    sjs_request_ctx_t *req = JS_GetContextOpaque(ctx);
    const char *prefix = req ? req->kv_prefix : NULL;

    void  *bc = NULL;
    size_t bc_len = 0;
    char resolved_name[256];

    const char *ext = sjs_path_extension(module_name);
    if (ext) {
        char raw_cache[256];
        snprintf(raw_cache, sizeof(raw_cache), "__compiled/%s", module_name);
        char cache_buf[512];
        const char *ck = sjs_prefixed_key(prefix, raw_cache,
                                          cache_buf, sizeof(cache_buf));
        if (ck)
            kv_get(sjs->kv, ck, &bc, &bc_len);
        snprintf(resolved_name, sizeof(resolved_name), "%s", module_name);
    } else {
        static const char *exts[] = { ".mjs", ".ejs", ".ts", ".tsx" };
        for (int ei = 0; ei < 4; ei++) {
            char raw_cache[256];
            snprintf(raw_cache, sizeof(raw_cache), "__compiled/%s%s",
                     module_name, exts[ei]);
            char cache_buf[512];
            const char *ck = sjs_prefixed_key(prefix, raw_cache,
                                              cache_buf, sizeof(cache_buf));
            if (!ck) continue;
            if (kv_get(sjs->kv, ck, &bc, &bc_len) == 0 && bc) {
                snprintf(resolved_name, sizeof(resolved_name), "%s%s",
                         module_name, exts[ei]);
                break;
            }
        }
    }
    if (!bc) return NULL;

    /* Record imported module in replay capture */
    if (sjs->current_replay_capture) {
        char mod_name[256];
        snprintf(mod_name, sizeof(mod_name), "%s", resolved_name);
        char meta_raw[256];
        snprintf(meta_raw, sizeof(meta_raw), "__code_meta/%s", mod_name);
        char meta_buf[512];
        const char *meta_key = sjs_prefixed_key(prefix, meta_raw,
                                                 meta_buf, sizeof(meta_buf));
        if (meta_key) {
            void *hash_val = NULL;
            size_t hash_len = 0;
            if (kv_get(sjs->kv, meta_key, &hash_val, &hash_len) == 0 && hash_val) {
                char hash_str[65] = {0};
                if (hash_len >= 64) memcpy(hash_str, hash_val, 64);
                replay_capture_module(sjs->current_replay_capture,
                                      mod_name, hash_str);
                free(hash_val);
            }
        }
    }

    JSValue module_val = JS_ReadObject(ctx, bc, bc_len, JS_READ_OBJ_BYTECODE);
    free(bc);

    if (JS_IsException(module_val)) return NULL;

    JSModuleDef *mod = JS_VALUE_GET_PTR(module_val);
    JS_FreeValue(ctx, module_val);
    return mod;
}

/* ======================================================================
 * Date.now / Math.random capture overrides
 * ====================================================================== */

static JSValue js_date_now_capture(JSContext *ctx, JSValue this_val,
                                    int argc, JSValue *argv) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t ms = (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;

    sjs_request_ctx_t *req = JS_GetContextOpaque(ctx);
    if (req && req->replay_capture)
        replay_capture_date_now(req->replay_capture, ms);

    return JS_NewInt64(ctx, ms);
}

static JSValue js_math_random_capture(JSContext *ctx, JSValue this_val,
                                       int argc, JSValue *argv) {
    uint64_t v;
    RAND_bytes((uint8_t *)&v, sizeof(v));

    union { uint64_t u; double d; } u;
    u.u = ((uint64_t)0x3ff << 52) | (v >> 12);
    double result = u.d - 1.0;

    sjs_request_ctx_t *req = JS_GetContextOpaque(ctx);
    if (req && req->replay_capture)
        replay_capture_math_random(req->replay_capture, result);

    return JS_NewFloat64(ctx, result);
}

/* ======================================================================
 * Arena snapshot: freeze a fully-initialized runtime+context into a
 * relocatable image. Restore per-request via memcpy + pointer fixup.
 * ====================================================================== */

static int snapshot_init_runtime(sjs_arena_t *arena,
                                 size_t *out_rt_offset, size_t *out_ctx_offset) {
    arena->used = 0;

    JSRuntime *rt = JS_NewRuntime2(&bump_mf, arena);
    if (!rt) return -1;

    JSContext *ctx = JS_NewContextRaw(rt);
    if (!ctx) return -1;

    JS_AddIntrinsicBaseObjects(ctx);
    JS_AddIntrinsicDate(ctx);
    JS_AddIntrinsicRegExp(ctx);
    JS_AddIntrinsicJSON(ctx);
    JS_AddIntrinsicMapSet(ctx);
    JS_AddIntrinsicTypedArrays(ctx);
    JS_AddIntrinsicPromise(ctx);

    /* Install globals */
    js_install_kv(ctx);
    js_install_request(ctx);
    js_install_response(ctx);
    js_install_session(ctx);
    js_install_crypto(ctx);
    js_install_code(ctx);
    js_install_console(ctx);
    js_install_logs(ctx);

    /* Override Date.now, Date constructor, and Math.random with
     * capture versions that record to the replay tape. */
    {
        JSValue global = JS_GetGlobalObject(ctx);

        JSValue date_obj = JS_GetPropertyStr(ctx, global, "Date");
        JS_SetPropertyStr(ctx, date_obj, "now",
                          JS_NewCFunction(ctx, js_date_now_capture, "now", 0));
        JS_FreeValue(ctx, date_obj);

        static const char date_wrap[] =
            "(function() {"
            "  const _D = Date;"
            "  const _now = Date.now;"
            "  function CDate(...args) {"
            "    if (!new.target) return _D(...args);"
            "    if (args.length === 0) return new _D(_now());"
            "    return new _D(...args);"
            "  }"
            "  CDate.now = _now;"
            "  CDate.parse = _D.parse;"
            "  CDate.UTC = _D.UTC;"
            "  CDate.prototype = _D.prototype;"
            "  return CDate;"
            "})()";
        JSValue wrapped = JS_Eval(ctx, date_wrap, sizeof(date_wrap) - 1,
                                   "<date-wrap>", JS_EVAL_TYPE_GLOBAL);
        if (!JS_IsException(wrapped))
            JS_SetPropertyStr(ctx, global, "Date", wrapped);
        else
            JS_FreeValue(ctx, JS_GetException(ctx));

        JSValue math_obj = JS_GetPropertyStr(ctx, global, "Math");
        JS_SetPropertyStr(ctx, math_obj, "random",
                          JS_NewCFunction(ctx, js_math_random_capture, "random", 0));
        JS_FreeValue(ctx, math_obj);
        JS_FreeValue(ctx, global);
    }

    JS_SetMaxStackSize(rt, 0);

    *out_rt_offset  = (char *)rt  - arena->data;
    *out_ctx_offset = (char *)ctx - arena->data;
    return 0;
}

static int snapshot_create(sjs_runtime_t *sjs, sjs_snapshot_t *snap) {
    sjs_arena_t *arena_a = sjs->arena;

    memset(arena_a->data, 0, SJS_ARENA_SIZE);

    size_t rt_off, ctx_off;
    if (snapshot_init_runtime(arena_a, &rt_off, &ctx_off) != 0)
        return -1;

    size_t used_a = arena_a->used;

    char *data_a = malloc(used_a);
    if (!data_a) return -1;
    memcpy(data_a, arena_a->data, used_a);

    sjs_arena_t *arena_b = malloc(sizeof(sjs_arena_t) + SJS_ARENA_SIZE);
    if (!arena_b) { free(data_a); return -1; }
    memset(arena_b->data, 0, SJS_ARENA_SIZE);

    size_t rt_off_b, ctx_off_b;
    if (snapshot_init_runtime(arena_b, &rt_off_b, &ctx_off_b) != 0) {
        free(arena_b);
        free(data_a);
        return -1;
    }

    size_t used_b = arena_b->used;

    if (used_a != used_b || rt_off != rt_off_b || ctx_off != ctx_off_b) {
        fprintf(stderr, "shift-js: snapshot_create: non-deterministic init "
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

    ptrdiff_t base_delta = arena_b->data - arena_a->data;
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
            if (volatile_count < SJS_SNAPSHOT_MAX_VOLATILE) {
                snap->volatile_offsets[volatile_count++] = byte_off;
                uint64_t zero = 0;
                memcpy(data_a + byte_off, &zero, sizeof(uint64_t));
            } else {
                fprintf(stderr, "shift-js: snapshot_create: too many "
                        "non-deterministic slots (slot %zu, max %d)\n",
                        i, SJS_SNAPSHOT_MAX_VOLATILE);
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
                fprintf(stderr, "shift-js: FATAL: volatile slot at byte "
                        "offset %zu is outside JSContext [%zu, %zu). "
                        "Likely a QuickJS upstream change — audit the "
                        "snapshot system before proceeding.\n",
                        off, ctx_start, ctx_end);
                abort();
            }
        }
        if (volatile_count > 2) {
            fprintf(stderr, "shift-js: FATAL: %zu volatile slots detected "
                    "(expected at most 2: random_state, time_origin). "
                    "Likely a QuickJS upstream change.\n", volatile_count);
            abort();
        }
    }

    snap->data     = data_a;
    snap->used     = used_a;
    snap->old_base = arena_a->data;
    snap->rt_offset  = rt_off;
    snap->ctx_offset = ctx_off;

    fprintf(stderr, "shift-js: snapshot created: %zu bytes, %zu relocations, "
            "%zu volatile slots\n",
            snap->used, reloc_count, volatile_count);

    return 0;
}

static void snapshot_destroy(sjs_snapshot_t *snap) {
    free(snap->data);
    free(snap->bitmap);
    memset(snap, 0, sizeof(*snap));
}

int snapshot_restore(const sjs_snapshot_t *snap, sjs_arena_t *arena,
                     sjs_runtime_t *sjs,
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

    JS_SetRuntimeOpaque(rt, sjs);

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

/* ======================================================================
 * ECS component constructors and destructors
 * ====================================================================== */

static void sjs_resp_status_ctor(shift_t *ctx, shift_collection_id_t col_id,
                                  const shift_entity_t *entities, void *data,
                                  uint32_t offset, uint32_t count,
                                  void *user_data) {
    (void)ctx; (void)col_id; (void)entities; (void)user_data;
    sjs_resp_status_t *p = (sjs_resp_status_t *)data + offset;
    for (uint32_t i = 0; i < count; i++)
        p[i].code = 200;
}

static void sjs_resp_headers_dtor(shift_t *ctx, shift_collection_id_t col_id,
                                   const shift_entity_t *entities, void *data,
                                   uint32_t offset, uint32_t count,
                                   void *user_data) {
    (void)ctx; (void)col_id; (void)entities; (void)user_data;
    sjs_resp_headers_t *p = (sjs_resp_headers_t *)data + offset;
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = 0; j < p[i].count; j++) {
            free(p[i].names[j]);
            free(p[i].values[j]);
        }
        free(p[i].names);
        free(p[i].values);
    }
}

static void sjs_session_dtor(shift_t *ctx, shift_collection_id_t col_id,
                              const shift_entity_t *entities, void *data,
                              uint32_t offset, uint32_t count,
                              void *user_data) {
    (void)ctx; (void)col_id; (void)entities; (void)user_data;
    sjs_session_t *p = (sjs_session_t *)data + offset;
    for (uint32_t i = 0; i < count; i++)
        free(p[i].id);
}

static void sjs_random_tape_dtor(shift_t *ctx, shift_collection_id_t col_id,
                                  const shift_entity_t *entities, void *data,
                                  uint32_t offset, uint32_t count,
                                  void *user_data) {
    (void)ctx; (void)col_id; (void)entities; (void)user_data;
    sjs_random_tape_t *p = (sjs_random_tape_t *)data + offset;
    for (uint32_t i = 0; i < count; i++)
        free(p[i].data);
}

static void sjs_route_info_dtor(shift_t *ctx, shift_collection_id_t col_id,
                                 const shift_entity_t *entities, void *data,
                                 uint32_t offset, uint32_t count,
                                 void *user_data) {
    (void)ctx; (void)col_id; (void)entities; (void)user_data;
    sjs_route_info_t *p = (sjs_route_info_t *)data + offset;
    for (uint32_t i = 0; i < count; i++) {
        free(p[i].module_path);
        free(p[i].func_name);
        free(p[i].query_string);
    }
}

static void sjs_bytecode_dtor(shift_t *ctx, shift_collection_id_t col_id,
                               const shift_entity_t *entities, void *data,
                               uint32_t offset, uint32_t count,
                               void *user_data) {
    (void)ctx; (void)col_id; (void)entities; (void)user_data;
    sjs_bytecode_t *p = (sjs_bytecode_t *)data + offset;
    for (uint32_t i = 0; i < count; i++)
        free(p[i].data);
}

static void sjs_log_record_dtor(shift_t *ctx, shift_collection_id_t col_id,
                                const shift_entity_t *entities, void *data,
                                uint32_t offset, uint32_t count,
                                void *user_data) {
    (void)ctx; (void)col_id; (void)entities; (void)user_data;
    sjs_log_record_t *p = (sjs_log_record_t *)data + offset;
    for (uint32_t i = 0; i < count; i++)
        sjs_log_record_free(&p[i]);
}

int sjs_register_components(shift_t *sh, sjs_component_ids_t *out) {
    shift_result_t r;
    out->resp_headers = shift_component_add_ex(sh, sizeof(sjs_resp_headers_t),
                                                NULL, sjs_resp_headers_dtor, &r);
    if (r != shift_ok) return -1;
    out->session = shift_component_add_ex(sh, sizeof(sjs_session_t),
                                           NULL, sjs_session_dtor, &r);
    if (r != shift_ok) return -1;
    out->random_tape = shift_component_add_ex(sh, sizeof(sjs_random_tape_t),
                                               NULL, sjs_random_tape_dtor, &r);
    if (r != shift_ok) return -1;
    out->route = shift_component_add_ex(sh, sizeof(sjs_route_info_t),
                                         NULL, sjs_route_info_dtor, &r);
    if (r != shift_ok) return -1;
    out->bytecode = shift_component_add_ex(sh, sizeof(sjs_bytecode_t),
                                            NULL, sjs_bytecode_dtor, &r);
    if (r != shift_ok) return -1;
    out->resp_status = shift_component_add_ex(sh, sizeof(sjs_resp_status_t),
                                               sjs_resp_status_ctor, NULL, &r);
    if (r != shift_ok) return -1;
    out->raft_seq = shift_component_add_ex(sh, sizeof(sjs_raft_seq_t),
                                            NULL, NULL, &r);
    if (r != shift_ok) return -1;
    out->log_record = shift_component_add_ex(sh, sizeof(sjs_log_record_t),
                                              NULL, sjs_log_record_dtor, &r);
    if (r != shift_ok) return -1;
    return 0;
}

void sjs_resp_headers_reset(sjs_resp_headers_t *h) {
    for (uint32_t i = 0; i < h->count; i++) {
        free(h->names[i]);
        free(h->values[i]);
    }
    h->count = 0;
}

/* ======================================================================
 * Runtime lifecycle
 * ====================================================================== */

int sjs_runtime_init(sjs_runtime_t *sjs, kvstore_t *kv,
                     const sjs_preprocessor_registry_t *preprocessors) {
    memset(sjs, 0, sizeof(*sjs));
    sjs->kv = kv;

    sjs->preprocessors_local = *preprocessors;
    sjs->preprocessors = &sjs->preprocessors_local;

    sjs->compile_rt = JS_NewRuntime();
    if (!sjs->compile_rt) return -1;

    JS_SetRuntimeOpaque(sjs->compile_rt, sjs);
    JS_SetModuleLoaderFunc(sjs->compile_rt, NULL, sjs_module_loader, NULL);

    sjs->compile_ctx = JS_NewContext(sjs->compile_rt);
    if (!sjs->compile_ctx) {
        JS_FreeRuntime(sjs->compile_rt);
        return -1;
    }

    if (sjs_typescript_init(sjs->compile_ctx, &sjs->ts_ctx) != 0) {
        fprintf(stderr, "shift-js: failed to initialize Sucrase\n");
        JS_FreeContext(sjs->compile_ctx);
        JS_FreeRuntime(sjs->compile_rt);
        return -1;
    }
    for (size_t i = 0; i < sjs->preprocessors_local.count; i++) {
        sjs_preprocessor_entry_t *e = &sjs->preprocessors_local.entries[i];
        if (e->transform != sjs_typescript_transform) continue;
        if (strcmp(e->extension, ".tsx") == 0)
            e->user_data = &sjs->ts_ctx.tsx_binding;
        else
            e->user_data = &sjs->ts_ctx.ts_binding;
    }

    sjs->arena = malloc(sizeof(sjs_arena_t) + SJS_ARENA_SIZE);
    if (!sjs->arena) {
        JS_FreeContext(sjs->compile_ctx);
        JS_FreeRuntime(sjs->compile_rt);
        return -1;
    }
    sjs->arena->used = 0;

    if (snapshot_create(sjs, &sjs->snapshot) != 0) {
        fprintf(stderr, "shift-js: snapshot_create failed\n");
        free(sjs->arena);
        JS_FreeContext(sjs->compile_ctx);
        JS_FreeRuntime(sjs->compile_rt);
        return -1;
    }

    return 0;
}

void sjs_runtime_free(sjs_runtime_t *sjs) {
    snapshot_destroy(&sjs->snapshot);
    sjs_typescript_free(&sjs->ts_ctx);
    if (sjs->compile_ctx) {
        JS_FreeContext(sjs->compile_ctx);
        sjs->compile_ctx = NULL;
    }
    if (sjs->compile_rt) {
        JS_FreeRuntime(sjs->compile_rt);
        sjs->compile_rt = NULL;
    }
    free(sjs->arena);
    sjs->arena = NULL;
}
