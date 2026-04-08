#define _GNU_SOURCE
#include "js_runtime.h"
#include "js_globals.h"
#include "code_store.h"
#include "crypto.h"
#include "kvstore.h"
#include "preprocessor.h"
#include "replay_capture.h"
#include "typescript.h"

/* Note: preprocessor.h and typescript.h are still needed for
 * sjs_runtime_init (TypeScript/Sucrase initialization). */

#include <quickjs.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/time.h>

void arena_reset(sjs_runtime_t *sjs) {
    qjs_snap_arena_reset(sjs->arena);
}

/* ======================================================================
 * Request-time module loader (loads pre-compiled bytecode from code_store)
 * ====================================================================== */

JSModuleDef *sjs_request_module_loader(JSContext *ctx,
                                       const char *module_name,
                                       void *opaque) {
    sjs_runtime_t *sjs = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    if (!sjs || !sjs->code_store) return NULL;

    void  *bc = NULL;
    size_t bc_len = 0;
    char resolved_name[256];
    char replay_hash[65] = {0};

    const char *ext = sjs_path_extension(module_name);
    if (ext) {
        code_entry_t entry;
        if (code_store_get(sjs->code_store, module_name, &entry) == 0) {
            bc = malloc(entry.bytecode_len);
            memcpy(bc, entry.bytecode, entry.bytecode_len);
            bc_len = entry.bytecode_len;
            snprintf(resolved_name, sizeof(resolved_name), "%s", module_name);
            memcpy(replay_hash, entry.source_hash, 41);
        }
    } else {
        static const char *exts[] = { ".mjs", ".ejs", ".ts", ".tsx" };
        for (int ei = 0; ei < 4; ei++) {
            char name[256];
            snprintf(name, sizeof(name), "%s%s", module_name, exts[ei]);
            code_entry_t entry;
            if (code_store_get(sjs->code_store, name, &entry) == 0) {
                bc = malloc(entry.bytecode_len);
                memcpy(bc, entry.bytecode, entry.bytecode_len);
                bc_len = entry.bytecode_len;
                snprintf(resolved_name, sizeof(resolved_name), "%s", name);
                memcpy(replay_hash, entry.source_hash, 41);
                break;
            }
        }
    }

    if (!bc) return NULL;

    /* Record imported module in replay capture */
    if (sjs->current_replay_capture && replay_hash[0])
        replay_capture_module(sjs->current_replay_capture,
                              resolved_name, replay_hash);

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

static int snapshot_init_runtime(qjs_snap_arena_t *arena,
                                 size_t *out_rt_offset,
                                 size_t *out_ctx_offset,
                                 void *user_data) {
    (void)user_data;
    arena->used = 0;

    JSRuntime *rt = JS_NewRuntime2(&qjs_snap_bump_mf, arena);
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

int snapshot_restore(const qjs_snap_snapshot_t *snap, qjs_snap_arena_t *arena,
                     sjs_runtime_t *sjs,
                     JSRuntime **out_rt, JSContext **out_ctx) {
    return qjs_snap_restore(snap, arena, sjs, out_rt, out_ctx);
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

    sjs->arena = qjs_snap_arena_alloc();
    if (!sjs->arena) {
        JS_FreeContext(sjs->compile_ctx);
        JS_FreeRuntime(sjs->compile_rt);
        return -1;
    }

    if (qjs_snap_create(sjs->arena, snapshot_init_runtime, NULL, &sjs->snapshot) != 0) {
        fprintf(stderr, "shift-js: snapshot_create failed\n");
        qjs_snap_arena_free(sjs->arena);
        JS_FreeContext(sjs->compile_ctx);
        JS_FreeRuntime(sjs->compile_rt);
        return -1;
    }

    return 0;
}

void sjs_runtime_free(sjs_runtime_t *sjs) {
    qjs_snap_destroy(&sjs->snapshot);
    sjs_typescript_free(&sjs->ts_ctx);
    if (sjs->compile_ctx) {
        JS_FreeContext(sjs->compile_ctx);
        sjs->compile_ctx = NULL;
    }
    if (sjs->compile_rt) {
        JS_FreeRuntime(sjs->compile_rt);
        sjs->compile_rt = NULL;
    }
    qjs_snap_arena_free(sjs->arena);
    sjs->arena = NULL;
}
