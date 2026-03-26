#define _GNU_SOURCE
#include "js_runtime.h"
#include "kvstore.h"
#include "preprocessor.h"
#include "router.h"
#include "session.h"

#include <quickjs.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ======================================================================
 * Fixed-size bump allocator for per-request JS runtimes
 *
 * Each arena is a fixed block (SJS_ARENA_SIZE). No realloc ever happens,
 * so no pointers are invalidated. Arenas are pre-allocated in a pool
 * and managed via a free list. After a request completes we skip all
 * JS teardown and just return the arena to the free list.
 * ====================================================================== */

/* Header prepended to every bump allocation so realloc can find the size. */
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

    /* If this is the last allocation, try to extend in place */
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
        return NULL;  /* arena full */
    }

    /* Otherwise: allocate new, copy, abandon old */
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

/* ---- Arena pool management ---- */

static void arena_reset(sjs_runtime_t *sjs) {
    sjs->arena->used = 0;
}

/* ======================================================================
 * JS error extraction — falls back to arena stats when OOM prevents
 * QuickJS from allocating an exception object.
 * ====================================================================== */

static const char *js_err_string(JSContext *ctx, sjs_arena_t *arena) {
    JSValue exc = JS_GetException(ctx);
    if (!JS_IsNull(exc) && !JS_IsUndefined(exc)) {
        const char *str = JS_ToCString(ctx, exc);
        if (str) return str;
    }
    /* Exception is null — almost certainly arena exhaustion.
     * Return a static string (no arena allocation needed). */
    static _Thread_local char oom_buf[128];
    snprintf(oom_buf, sizeof(oom_buf),
             "arena exhausted (%zu / %d bytes used)",
             arena->used, SJS_ARENA_SIZE);
    return oom_buf;
}

/* ======================================================================
 * kv global
 * ====================================================================== */

static kvstore_t *js_get_kv(JSContext *ctx) {
    sjs_runtime_t *sjs = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    return sjs ? sjs->kv : NULL;
}

/* Prepend prefix to key into caller-provided buf.
 * Returns key as-is when prefix is NULL/empty. */
/* Use the shared kv_prefixed_key from kvstore.h */
#define sjs_prefixed_key kv_prefixed_key

static const char *js_get_prefix(JSContext *ctx) {
    sjs_request_ctx_t *req = JS_GetContextOpaque(ctx);
    return req ? req->kv_prefix : NULL;
}

static JSValue js_kv_get(JSContext *ctx, JSValue this_val,
                         int argc, JSValue *argv) {
    kvstore_t *kv = js_get_kv(ctx);
    if (!kv) return JS_ThrowInternalError(ctx, "kv store not available");

    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    char pfx_buf[512];
    const char *actual_key = sjs_prefixed_key(js_get_prefix(ctx), key,
                                               pfx_buf, sizeof(pfx_buf));
    if (!actual_key) {
        JS_FreeCString(ctx, key);
        return JS_ThrowRangeError(ctx, "kv key too long");
    }

    void  *value = NULL;
    size_t vlen  = 0;
    int rc = kv_get(kv, actual_key, &value, &vlen);
    JS_FreeCString(ctx, key);

    if (rc == -1) return JS_NULL;
    if (rc < 0)   return JS_ThrowInternalError(ctx, "kv.get failed (rc=%d)", rc);

    JSValue result = JS_NewStringLen(ctx, value, vlen);
    free(value);
    return result;
}

static JSValue js_kv_put(JSContext *ctx, JSValue this_val,
                         int argc, JSValue *argv) {
    kvstore_t *kv = js_get_kv(ctx);
    if (!kv) return JS_ThrowInternalError(ctx, "kv store not available");

    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    char pfx_buf[512];
    const char *actual_key = sjs_prefixed_key(js_get_prefix(ctx), key,
                                               pfx_buf, sizeof(pfx_buf));
    if (!actual_key) {
        JS_FreeCString(ctx, key);
        return JS_ThrowRangeError(ctx, "kv key too long");
    }

    size_t vlen;
    const char *val = JS_ToCStringLen(ctx, &vlen, argv[1]);
    if (!val) { JS_FreeCString(ctx, key); return JS_EXCEPTION; }

    int rc = kv_put(kv, actual_key, val, vlen);
    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, val);

    if (rc != 0) return JS_ThrowInternalError(ctx, "kv.put failed (rc=%d)", rc);
    return JS_TRUE;
}

static JSValue js_kv_delete(JSContext *ctx, JSValue this_val,
                            int argc, JSValue *argv) {
    kvstore_t *kv = js_get_kv(ctx);
    if (!kv) return JS_ThrowInternalError(ctx, "kv store not available");

    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    char pfx_buf[512];
    const char *actual_key = sjs_prefixed_key(js_get_prefix(ctx), key,
                                               pfx_buf, sizeof(pfx_buf));
    if (!actual_key) {
        JS_FreeCString(ctx, key);
        return JS_ThrowRangeError(ctx, "kv key too long");
    }

    int rc = kv_delete(kv, actual_key);
    JS_FreeCString(ctx, key);

    if (rc != 0) return JS_ThrowInternalError(ctx, "kv.delete failed (rc=%d)", rc);
    return JS_TRUE;
}

static JSValue js_kv_range(JSContext *ctx, JSValue this_val,
                           int argc, JSValue *argv) {
    kvstore_t *kv = js_get_kv(ctx);
    if (!kv) return JS_ThrowInternalError(ctx, "kv store not available");

    const char *start = JS_ToCString(ctx, argv[0]);
    if (!start) return JS_EXCEPTION;

    const char *end = JS_ToCString(ctx, argv[1]);
    if (!end) { JS_FreeCString(ctx, start); return JS_EXCEPTION; }

    const char *prefix = js_get_prefix(ctx);
    char pfx_start[512], pfx_end[512];
    const char *actual_start = sjs_prefixed_key(prefix, start,
                                                 pfx_start, sizeof(pfx_start));
    const char *actual_end = sjs_prefixed_key(prefix, end,
                                               pfx_end, sizeof(pfx_end));
    if (!actual_start || !actual_end) {
        JS_FreeCString(ctx, start);
        JS_FreeCString(ctx, end);
        return JS_ThrowRangeError(ctx, "kv range key too long");
    }

    int64_t count = 100;
    if (argc > 2) JS_ToInt64(ctx, &count, argv[2]);

    kv_range_result_t result;
    int rc = kv_range(kv, actual_start, actual_end, (size_t)count, &result);
    JS_FreeCString(ctx, start);
    JS_FreeCString(ctx, end);

    if (rc < 0) return JS_ThrowInternalError(ctx, "kv.range failed (rc=%d)", rc);

    size_t prefix_len = (prefix && prefix[0]) ? strlen(prefix) : 0;

    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < result.count; i++) {
        JSValue entry = JS_NewObject(ctx);
        if (JS_IsException(entry)) {
            kv_range_free(&result);
            return JS_ThrowInternalError(ctx, "kv.range: arena exhausted building results");
        }
        const char *visible_key = result.entries[i].key + prefix_len;
        JS_SetPropertyStr(ctx, entry, "key",
                          JS_NewString(ctx, visible_key));
        JS_SetPropertyStr(ctx, entry, "value",
                          JS_NewStringLen(ctx, result.entries[i].value,
                                          result.entries[i].value_len));
        JS_SetPropertyStr(ctx, entry, "value_size",
                          JS_NewInt64(ctx, (int64_t)result.entries[i].value_len));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, entry);
    }

    kv_range_free(&result);
    return arr;
}

static void js_install_kv(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue kv_obj = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, kv_obj, "get",
                      JS_NewCFunction(ctx, js_kv_get, "get", 1));
    JS_SetPropertyStr(ctx, kv_obj, "put",
                      JS_NewCFunction(ctx, js_kv_put, "put", 2));
    JS_SetPropertyStr(ctx, kv_obj, "delete",
                      JS_NewCFunction(ctx, js_kv_delete, "delete", 1));
    JS_SetPropertyStr(ctx, kv_obj, "range",
                      JS_NewCFunction(ctx, js_kv_range, "range", 3));

    JS_SetPropertyStr(ctx, global, "kv", kv_obj);
    JS_FreeValue(ctx, global);
}

/* ======================================================================
 * request global (read-only per-request)
 * ====================================================================== */

static sjs_request_ctx_t *js_get_req_ctx(JSContext *ctx) {
    return JS_GetContextOpaque(ctx);
}

static JSValue js_request_get(JSContext *ctx, JSValue this_val,
                              int magic) {
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (!req) return JS_ThrowInternalError(ctx, "no request context");

    switch (magic) {
    case 0: return JS_NewString(ctx, req->method);
    case 1: return JS_NewString(ctx, req->path);
    case 2: {
        if (req->body && req->body_len > 0)
            return JS_NewStringLen(ctx, req->body, req->body_len);
        return JS_NULL;
    }
    case 3: {
        JSValue obj = JS_NewObject(ctx);
        for (uint32_t i = 0; i < req->header_count; i++) {
            char name[256];
            size_t nlen = req->headers[i].name_len;
            if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
            memcpy(name, req->headers[i].name, nlen);
            name[nlen] = '\0';
            JS_SetPropertyStr(ctx, obj, name,
                              JS_NewStringLen(ctx, req->headers[i].value,
                                              req->headers[i].value_len));
        }
        return obj;
    }
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_request_props[] = {
    JS_CGETSET_MAGIC_DEF("method",  js_request_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("path",    js_request_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("body",    js_request_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("headers", js_request_get, NULL, 3),
};

static void js_install_request(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue req = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, req, js_request_props,
                               sizeof(js_request_props) / sizeof(js_request_props[0]));
    JS_SetPropertyStr(ctx, global, "request", req);
    JS_FreeValue(ctx, global);
}

/* ======================================================================
 * response global (status() and header() methods)
 * ====================================================================== */

static JSValue js_response_status(JSContext *ctx, JSValue this_val,
                                  int argc, JSValue *argv) {
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (!req) return JS_ThrowInternalError(ctx, "no request context");

    int32_t code;
    if (JS_ToInt32(ctx, &code, argv[0])) return JS_EXCEPTION;
    req->resp_status = (uint16_t)code;
    return JS_UNDEFINED;
}

static JSValue js_response_header(JSContext *ctx, JSValue this_val,
                                  int argc, JSValue *argv) {
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (!req) return JS_ThrowInternalError(ctx, "no request context");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    const char *value = JS_ToCString(ctx, argv[1]);
    if (!value) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }

    if (req->resp_header_count == req->resp_header_cap) {
        uint32_t new_cap = req->resp_header_cap ? req->resp_header_cap * 2 : 8;
        char **nn = realloc(req->resp_header_names, new_cap * sizeof(char *));
        char **nv = realloc(req->resp_header_values, new_cap * sizeof(char *));
        if (!nn || !nv) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, value);
            return JS_ThrowInternalError(ctx, "response header allocation failed");
        }
        req->resp_header_names = nn;
        req->resp_header_values = nv;
        req->resp_header_cap = new_cap;
    }

    req->resp_header_names[req->resp_header_count] = strdup(name);
    req->resp_header_values[req->resp_header_count] = strdup(value);
    req->resp_header_count++;

    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, value);
    return JS_UNDEFINED;
}

static void js_install_response(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue resp = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, resp, "status",
                      JS_NewCFunction(ctx, js_response_status, "status", 1));
    JS_SetPropertyStr(ctx, resp, "header",
                      JS_NewCFunction(ctx, js_response_header, "header", 2));

    JS_SetPropertyStr(ctx, global, "response", resp);
    JS_FreeValue(ctx, global);
}

/* ======================================================================
 * session global
 * ====================================================================== */

/* session.id — read-only getter returning the session ID */
static JSValue js_session_id_get(JSContext *ctx, JSValue this_val, int magic) {
    (void)magic;
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (!req || !req->session_id) return JS_NULL;
    return JS_NewString(ctx, req->session_id);
}

/* session.get(key) — read from the __data property */
static JSValue js_session_get(JSContext *ctx, JSValue this_val,
                              int argc, JSValue *argv) {
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    JSValue data = JS_GetPropertyStr(ctx, this_val, "__data");
    JSValue val = JS_GetPropertyStr(ctx, data, key);
    JS_FreeValue(ctx, data);
    JS_FreeCString(ctx, key);
    return val;
}

/* session.set(key, value) — write to __data and mark dirty */
static JSValue js_session_set(JSContext *ctx, JSValue this_val,
                              int argc, JSValue *argv) {
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (!req) return JS_ThrowInternalError(ctx, "no request context");

    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    JSValue data = JS_GetPropertyStr(ctx, this_val, "__data");
    JS_SetPropertyStr(ctx, data, key, JS_DupValue(ctx, argv[1]));
    JS_FreeValue(ctx, data);
    JS_FreeCString(ctx, key);

    req->session_dirty = true;
    return JS_UNDEFINED;
}

/* session.delete(key) — delete from __data and mark dirty */
static JSValue js_session_delete(JSContext *ctx, JSValue this_val,
                                 int argc, JSValue *argv) {
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (!req) return JS_ThrowInternalError(ctx, "no request context");

    JSAtom atom = JS_ValueToAtom(ctx, argv[0]);
    if (atom == JS_ATOM_NULL) return JS_EXCEPTION;

    JSValue data = JS_GetPropertyStr(ctx, this_val, "__data");
    JS_DeleteProperty(ctx, data, atom, 0);
    JS_FreeValue(ctx, data);
    JS_FreeAtom(ctx, atom);

    req->session_dirty = true;
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_session_props[] = {
    JS_CGETSET_MAGIC_DEF("id", js_session_id_get, NULL, 0),
};

static void js_install_session(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue sess = JS_NewObject(ctx);

    JS_SetPropertyFunctionList(ctx, sess, js_session_props,
                               sizeof(js_session_props) / sizeof(js_session_props[0]));
    JS_SetPropertyStr(ctx, sess, "get",
                      JS_NewCFunction(ctx, js_session_get, "get", 1));
    JS_SetPropertyStr(ctx, sess, "set",
                      JS_NewCFunction(ctx, js_session_set, "set", 2));
    JS_SetPropertyStr(ctx, sess, "delete",
                      JS_NewCFunction(ctx, js_session_delete, "delete", 1));

    /* __data starts as empty object — populated per-request from KV */
    JS_SetPropertyStr(ctx, sess, "__data", JS_NewObject(ctx));

    JS_SetPropertyStr(ctx, global, "session", sess);
    JS_FreeValue(ctx, global);
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

    /* Check if the module name has a known extension */
    const char *ext = sjs_path_extension(module_name);

    if (ext) {
        /* Explicit extension — fetch directly */
        char raw_key[256];
        snprintf(raw_key, sizeof(raw_key), "__code/%s", module_name);

        char key_buf[512];
        const char *key = sjs_prefixed_key(sjs->current_prefix, raw_key,
                                            key_buf, sizeof(key_buf));
        if (!key) return NULL;

        if (kv_get(kv, key, &source, &source_len) != 0) return NULL;
    } else {
        /* No extension — probe with each registered extension */
        resolved_alloc = sjs_resolve_with_extensions(
            sjs->preprocessors, kv, module_name,
            sjs->current_prefix, &source, &source_len);
        if (!resolved_alloc) return NULL;
        resolved_name = resolved_alloc;
        ext = sjs_path_extension(resolved_name);
    }

    /* Apply preprocessor if extension has one registered */
    char  *compile_source = source;
    size_t compile_len    = source_len;

    sjs_preprocess_fn transform = ext
        ? sjs_preprocessor_find(sjs->preprocessors, ext)
        : NULL;

    if (transform) {
        size_t js_len;
        char *js = transform(source, source_len, &js_len);
        free(source);
        if (!js) { free(resolved_alloc); return NULL; }
        compile_source = js;
        compile_len    = js_len;
    }

    JSValue func = JS_Eval(ctx, compile_source, compile_len, resolved_name,
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(compile_source);
    free(resolved_alloc);

    if (JS_IsException(func)) return NULL;

    JSModuleDef *mod = JS_VALUE_GET_PTR(func);
    JS_FreeValue(ctx, func);
    return mod;
}

/* ======================================================================
 * Arena snapshot: freeze a fully-initialized runtime+context into a
 * relocatable image. Restore per-request via memcpy + pointer fixup.
 * ====================================================================== */

static int snapshot_create(sjs_runtime_t *sjs, sjs_snapshot_t *snap) {
    /* Use the worker's arena to build the template */
    sjs_arena_t *arena = sjs->arena;
    arena->used = 0;

    JSRuntime *rt = JS_NewRuntime2(&bump_mf, arena);
    if (!rt) return -1;

    JS_SetRuntimeOpaque(rt, sjs);

    JSContext *ctx = JS_NewContextRaw(rt);
    if (!ctx) return -1;

    JS_AddIntrinsicBaseObjects(ctx);
    JS_AddIntrinsicDate(ctx);
    JS_AddIntrinsicRegExp(ctx);
    JS_AddIntrinsicJSON(ctx);
    JS_AddIntrinsicMapSet(ctx);
    JS_AddIntrinsicTypedArrays(ctx);
    JS_AddIntrinsicPromise(ctx);

    /* Install globals with NULL request context — just the structure */
    js_install_kv(ctx);
    js_install_request(ctx);
    js_install_response(ctx);
    js_install_session(ctx);

    /* Save the arena content */
    snap->used = arena->used;
    snap->data = malloc(snap->used);
    if (!snap->data) return -1;
    memcpy(snap->data, arena->data, snap->used);
    snap->old_base = arena->data;

    /* Record offsets of rt and ctx within the arena so we can find them
     * after restore. They are the first two allocations. */
    snap->rt_offset  = (char *)rt  - arena->data;
    snap->ctx_offset = (char *)ctx - arena->data;

    /* Build relocation bitmap: 1 bit per 8-byte-aligned slot.
     * A bit is set if the 8-byte value at that offset is a pointer
     * into the arena range [base, base+used). */
    size_t num_slots = snap->used / sizeof(void *);
    snap->bitmap_words = (num_slots + 63) / 64;
    snap->bitmap = calloc(snap->bitmap_words, sizeof(uint64_t));
    if (!snap->bitmap) { free(snap->data); return -1; }

    char *base = arena->data;
    char *end  = base + arena->used;
    size_t reloc_count = 0;

    for (size_t i = 0; i < num_slots; i++) {
        void *val;
        memcpy(&val, base + i * sizeof(void *), sizeof(void *));
        if ((char *)val >= base && (char *)val < end) {
            snap->bitmap[i / 64] |= (uint64_t)1 << (i % 64);
            reloc_count++;
        }
    }

    fprintf(stderr, "shift-js: snapshot created: %zu bytes, %zu relocations\n",
            snap->used, reloc_count);

    /* Arena is reused for requests — snapshot is in snap->data */
    return 0;
}

static void snapshot_destroy(sjs_snapshot_t *snap) {
    free(snap->data);
    free(snap->bitmap);
    memset(snap, 0, sizeof(*snap));
}

/* Restore a snapshot into a fresh arena. Returns the JSRuntime* and
 * JSContext* at their new addresses. The arena is ready for use —
 * additional allocations (bytecode load, handler execution) go after
 * the restored content. */
static int snapshot_restore(const sjs_snapshot_t *snap, sjs_arena_t *arena,
                            sjs_runtime_t *sjs,
                            JSRuntime **out_rt, JSContext **out_ctx) {
    memcpy(arena->data, snap->data, snap->used);
    arena->used = snap->used;

    ptrdiff_t delta = arena->data - snap->old_base;

    if (delta != 0) {
        /* Walk the bitmap and relocate every marked pointer */
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

    /* The JSRuntime and JSContext are at known offsets */
    JSRuntime *rt  = (JSRuntime *)(arena->data + snap->rt_offset);
    JSContext *ctx  = (JSContext *)(arena->data + snap->ctx_offset);

    /* Patch the malloc state opaque to point to the new arena,
     * and the runtime opaque to point to our sjs_runtime_t */
    JS_SetRuntimeOpaque(rt, sjs);

    /* The JSMallocState.opaque is an external pointer (points to sjs_arena_t,
     * not inside the arena data). We need to set it to the new arena.
     * JS_SetRuntimeOpaque doesn't do this — we need to find JSMallocState.
     * It's stored inside JSRuntime. The custom allocator's opaque was the
     * arena pointer. After memcpy it still points to the old arena.
     * Unfortunately there's no public API for this, but JSMallocState is
     * at a fixed offset within JSRuntime. We can use a workaround:
     * the bump_mf functions receive opaque as the arena pointer. After
     * restore, any new allocation will use the wrong arena. We fix this
     * by knowing that JSRuntime stores JSMallocState which has .opaque.
     *
     * Since JSRuntime is opaque to us, we use a trick: create a tiny
     * test allocation, and if it lands in our arena, the opaque is correct.
     * Actually, simpler: the opaque pointer was set to &arena (the
     * sjs_arena_t*) when we called JS_NewRuntime2. But the sjs_arena_t
     * struct lives OUTSIDE the arena data (it's the header before data[]).
     * So it was NOT relocated by the bitmap (it's external). But after
     * restore, we're using a different sjs_arena_t. We need to patch it.
     *
     * The cleanest approach: scan for the old arena pointer in the restored
     * data and replace it. The old opaque was snap->old_base - offsetof(sjs_arena_t, data).
     * Actually, the opaque passed to JS_NewRuntime2 was the sjs_arena_t*
     * itself, which equals (arena_header*). Let's just find and patch it.
     */

    /* The JSMallocState.opaque field holds the sjs_arena_t pointer.
     * When we created the snapshot, opaque pointed to the original arena.
     * We need it to point to the new arena. The original arena pointer was:
     *   old_arena = container_of(snap->old_base, sjs_arena_t, data)
     * The new arena pointer is just `arena`.
     * We scan the JSRuntime region for this value and patch it. */
    {
        sjs_arena_t *old_arena = (sjs_arena_t *)(snap->old_base - offsetof(sjs_arena_t, data));
        /* JSMallocState is near the beginning of JSRuntime.
         * Scan first 256 bytes — the opaque field is early in the struct. */
        char *rt_bytes = (char *)rt;
        size_t scan_len = 256;
        if (snap->rt_offset + scan_len > arena->used)
            scan_len = arena->used - snap->rt_offset;
        for (size_t i = 0; i <= scan_len - sizeof(void *); i += sizeof(void *)) {
            void *val;
            memcpy(&val, rt_bytes + i, sizeof(void *));
            if (val == old_arena) {
                void *new_val = arena;
                memcpy(rt_bytes + i, &new_val, sizeof(void *));
                break;
            }
        }
    }

    *out_rt = rt;
    *out_ctx = ctx;
    return 0;
}

/* ======================================================================
 * Runtime lifecycle
 * ====================================================================== */

int sjs_runtime_init(sjs_runtime_t *sjs, kvstore_t *kv,
                     const sjs_preprocessor_registry_t *preprocessors) {
    memset(sjs, 0, sizeof(*sjs));
    sjs->kv = kv;
    sjs->preprocessors = preprocessors;

    /* Long-lived compiler runtime (standard malloc). */
    sjs->compile_rt = JS_NewRuntime();
    if (!sjs->compile_rt) return -1;

    JS_SetRuntimeOpaque(sjs->compile_rt, sjs);
    JS_SetModuleLoaderFunc(sjs->compile_rt, NULL, sjs_module_loader, NULL);

    sjs->compile_ctx = JS_NewContext(sjs->compile_rt);
    if (!sjs->compile_ctx) {
        JS_FreeRuntime(sjs->compile_rt);
        return -1;
    }

    /* Allocate single arena per worker — reset between requests. */
    sjs->arena = malloc(sizeof(sjs_arena_t) + SJS_ARENA_SIZE);
    if (!sjs->arena) {
        JS_FreeContext(sjs->compile_ctx);
        JS_FreeRuntime(sjs->compile_rt);
        return -1;
    }
    sjs->arena->used = 0;

    /* Create the frozen snapshot of a fully-initialized runtime+context.
     * This is the template that gets memcpy'd + relocated per request. */
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

/* ======================================================================
 * Request dispatch
 * ====================================================================== */

/* ---- helpers: response header manipulation ---- */

static void resp_add_header(sjs_request_ctx_t *req,
                            const char *name, const char *value) {
    if (req->resp_header_count == req->resp_header_cap) {
        uint32_t nc = req->resp_header_cap ? req->resp_header_cap * 2 : 8;
        req->resp_header_names  = realloc(req->resp_header_names,  nc * sizeof(char *));
        req->resp_header_values = realloc(req->resp_header_values, nc * sizeof(char *));
        req->resp_header_cap = nc;
    }
    req->resp_header_names[req->resp_header_count]  = strdup(name);
    req->resp_header_values[req->resp_header_count] = strdup(value);
    req->resp_header_count++;
}

static bool resp_has_header(const sjs_request_ctx_t *req, const char *name) {
    for (uint32_t i = 0; i < req->resp_header_count; i++)
        if (!strcasecmp(req->resp_header_names[i], name)) return true;
    return false;
}

/* ---- helpers: query string parsing ---- */

/* Simple URL-decode in place. Returns decoded length. */
static size_t url_decode(char *s, size_t len) {
    size_t j = 0;
    for (size_t i = 0; i < len; ) {
        if (s[i] == '%' && i + 2 < len) {
            char hex[3] = { s[i+1], s[i+2], '\0' };
            char *end;
            long val = strtol(hex, &end, 16);
            if (end == hex + 2) {
                s[j++] = (char)val;
                i += 3;
                continue;
            }
        }
        if (s[i] == '+') { s[j++] = ' '; i++; }
        else              { s[j++] = s[i++]; }
    }
    s[j] = '\0';
    return j;
}

/* Parse query string into a JS object: { key: value, ... }
 * Duplicate keys: last value wins. */
static JSValue parse_query_string(JSContext *ctx, const char *qs) {
    JSValue obj = JS_NewObject(ctx);
    if (!qs || !*qs) return obj;

    char *buf = strdup(qs);
    char *p = buf;
    while (p && *p) {
        char *amp = strchr(p, '&');
        if (amp) *amp = '\0';

        char *eq = strchr(p, '=');
        if (eq) {
            *eq = '\0';
            char *key = p;
            char *val = eq + 1;
            url_decode(key, strlen(key));
            size_t vlen = url_decode(val, strlen(val));
            JS_SetPropertyStr(ctx, obj, key,
                              JS_NewStringLen(ctx, val, vlen));
        } else {
            url_decode(p, strlen(p));
            JS_SetPropertyStr(ctx, obj, p, JS_NewString(ctx, ""));
        }

        p = amp ? amp + 1 : NULL;
    }
    free(buf);
    return obj;
}

/* ---- helpers: compile and cache a module ---- */

/* Try to resolve, preprocess, compile, and cache a module at base_path.
 * Returns 0 on success (bytecode/bc_len filled), -1 on not found,
 * positive on error (resp_status and body filled). */
static int compile_module(sjs_runtime_t *sjs, sjs_request_ctx_t *req,
                          const char *base_path, const char *cache_key,
                          void **bytecode, size_t *bc_len,
                          char **out_module_path,
                          char **err_body, uint32_t *err_len) {
    void  *source = NULL;
    size_t source_len = 0;
    char *module_path = sjs_resolve_with_extensions(
        sjs->preprocessors, sjs->kv, base_path,
        req->kv_prefix, &source, &source_len);

    if (!module_path) return -1;   /* not found */

    const char *ext = sjs_path_extension(module_path);
    sjs_preprocess_fn transform = ext
        ? sjs_preprocessor_find(sjs->preprocessors, ext) : NULL;

    char  *compile_source = source;
    size_t compile_len    = source_len;

    if (transform) {
        size_t js_len;
        char *js = transform(source, source_len, &js_len);
        free(source);
        if (!js) {
            free(module_path);
            req->resp_status = 500;
            *err_body = strdup("Preprocessor Error");
            *err_len = (uint32_t)strlen(*err_body);
            return 1;
        }
        compile_source = js;
        compile_len    = js_len;
    }

    sjs->current_prefix = req->kv_prefix;
    JSContext *cc = sjs->compile_ctx;
    JSValue module_val = JS_Eval(cc, compile_source, compile_len,
                                 module_path,
                                 JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(compile_source);
    sjs->current_prefix = NULL;

    if (JS_IsException(module_val)) {
        JSValue exc = JS_GetException(cc);
        const char *err = JS_ToCString(cc, exc);
        fprintf(stderr, "shift-js: compile error in %s: %s\n",
                module_path, err ? err : "(unknown)");
        asprintf(err_body, "compile error in %s: %s",
                 module_path, err ? err : "(unknown)");
        JS_FreeCString(cc, err);
        JS_FreeValue(cc, exc);
        free(module_path);
        req->resp_status = 500;
        *err_len = (uint32_t)strlen(*err_body);
        return 1;
    }

    size_t out_bc_len;
    uint8_t *bc = JS_WriteObject(cc, &out_bc_len, module_val,
                                 JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(cc, module_val);

    if (!bc) {
        free(module_path);
        req->resp_status = 500;
        *err_body = strdup("Bytecode Serialization Error");
        *err_len = (uint32_t)strlen(*err_body);
        return 1;
    }

    kv_put(sjs->kv, cache_key, bc, out_bc_len);
    *bytecode = bc;
    *bc_len = out_bc_len;
    *out_module_path = module_path;
    return 0;
}

/* ---- helpers: body extraction (return value → libc heap string) ---- */

static char *extract_body_string(JSContext *ctx, sjs_arena_t *arena,
                                 sjs_request_ctx_t *req,
                                 JSValue ret, const char *module_path,
                                 const char *func_name,
                                 uint32_t *out_len,
                                 char **err_msg) {
    char *body = NULL;

    if (JS_IsString(ret)) {
        size_t len;
        const char *str = JS_ToCStringLen(ctx, &len, ret);
        body = malloc(len);
        if (body) {
            memcpy(body, str, len);
            *out_len = (uint32_t)len;
        }
        return body;
    }

    if (JS_IsUndefined(ret) || JS_IsNull(ret))
        return NULL;

    /* Object/array — JSON.stringify */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue json = JS_GetPropertyStr(ctx, global, "JSON");
    JSValue stringify = JS_GetPropertyStr(ctx, json, "stringify");

    JSValue json_str = JS_Call(ctx, stringify, json, 1, &ret);
    if (JS_IsException(json_str)) {
        const char *err = js_err_string(ctx, arena);
        fprintf(stderr, "shift-js: JSON.stringify error in %s.%s: %s\n",
                module_path, func_name, err);
        asprintf(err_msg, "JSON.stringify error in %s.%s: %s",
                 module_path, func_name, err);
        return NULL;
    }
    if (JS_IsString(json_str)) {
        size_t len;
        const char *str = JS_ToCStringLen(ctx, &len, json_str);
        body = malloc(len);
        if (body) {
            memcpy(body, str, len);
            *out_len = (uint32_t)len;
        }
        if (!resp_has_header(req, "content-type"))
            resp_add_header(req, "content-type", "application/json");
    }
    return body;
}

/* ---- main dispatch ---- */

char *sjs_dispatch(sjs_runtime_t *sjs, sjs_request_ctx_t *req,
                   uint32_t *out_len) {
    req->resp_status = 200;
    req->resp_header_names = NULL;
    req->resp_header_values = NULL;
    req->resp_header_count = 0;
    req->resp_header_cap = 0;

    /* ---- Route resolution ----
     * Primary: full URL path as module (e.g. /foo/bar → foo/bar/index)
     * Fallback: peel last segment as function name (e.g. /foo/bar → foo/index + "bar")
     */
    sjs_route_t route;
    sjs_resolve_route(req->path, &route);

    char *base_path = route.module_path;
    char *url_func  = NULL;
    char *query_str = route.query_string;

    /* Detach from route so we can free independently */
    route.module_path = NULL;
    route.query_string = NULL;
    sjs_route_free(&route);

    if (!base_path) {
        req->resp_status = 500;
        char *body = strdup("Internal Server Error");
        *out_len = (uint32_t)strlen(body);
        return body;
    }

    /* ---- Phase 1: Ensure bytecode exists in KV cache ---- */
    char raw_cache[256];
    snprintf(raw_cache, sizeof(raw_cache), "__compiled/%s", base_path);

    char cache_key_buf[512];
    const char *ck = sjs_prefixed_key(req->kv_prefix, raw_cache,
                                       cache_key_buf, sizeof(cache_key_buf));
    if (!ck) {
        free(base_path); free(query_str);
        req->resp_status = 500;
        char *body = strdup("Key Too Long");
        *out_len = (uint32_t)strlen(body);
        return body;
    }

    char *module_path = NULL;
    void  *bytecode = NULL;
    size_t bc_len = 0;

    if (kv_get(sjs->kv, ck, &bytecode, &bc_len) != 0) {
        /* Cache miss — try to compile */
        char *err_body = NULL;
        uint32_t err_len = 0;
        int rc = compile_module(sjs, req, base_path, ck,
                                &bytecode, &bc_len, &module_path,
                                &err_body, &err_len);
        if (rc < 0) {
            /* Primary not found — try fallback route */
            free(base_path);
            sjs_route_t fb;
            sjs_resolve_route_fallback(req->path, &fb);
            base_path = fb.module_path;
            url_func  = fb.func_name;
            fb.module_path = NULL;
            fb.func_name = NULL;
            sjs_route_free(&fb);

            if (!base_path) {
                free(query_str);
                req->resp_status = 404;
                char *body = strdup("Not Found");
                *out_len = (uint32_t)strlen(body);
                return body;
            }

            snprintf(raw_cache, sizeof(raw_cache), "__compiled/%s", base_path);
            ck = sjs_prefixed_key(req->kv_prefix, raw_cache,
                                   cache_key_buf, sizeof(cache_key_buf));
            if (!ck) {
                free(base_path); free(url_func); free(query_str);
                req->resp_status = 500;
                char *body = strdup("Key Too Long");
                *out_len = (uint32_t)strlen(body);
                return body;
            }

            if (kv_get(sjs->kv, ck, &bytecode, &bc_len) != 0) {
                rc = compile_module(sjs, req, base_path, ck,
                                    &bytecode, &bc_len, &module_path,
                                    &err_body, &err_len);
                if (rc < 0) {
                    free(base_path); free(url_func); free(query_str);
                    req->resp_status = 404;
                    char *body = strdup("Not Found");
                    *out_len = (uint32_t)strlen(body);
                    return body;
                }
                if (rc > 0) {
                    free(base_path); free(url_func); free(query_str);
                    *out_len = err_len;
                    return err_body;
                }
            }
        } else if (rc > 0) {
            free(base_path); free(query_str);
            *out_len = err_len;
            return err_body;
        }
    }

    if (!module_path) module_path = strdup(base_path);
    free(base_path);

    /* ---- Session: extract session ID from Cookie header ---- */
    req->session_id = NULL;
    req->session_new = false;
    req->session_dirty = false;

    for (uint32_t i = 0; i < req->header_count; i++) {
        if (req->headers[i].name_len == 6 &&
            memcmp(req->headers[i].name, "cookie", 6) == 0) {
            req->session_id = sjs_session_parse_cookie(
                req->headers[i].value, req->headers[i].value_len);
            break;
        }
    }
    if (!req->session_id) {
        char id_buf[SJS_SESSION_ID_LEN + 1];
        if (sjs_session_generate_id(id_buf))
            req->session_id = strdup(id_buf);
        req->session_new = true;
    }

    /* ---- Phases 2-4 with transaction retry on conflict ---- */
    #define MAX_TXN_RETRIES 3

    char *err_msg = NULL;

    for (int attempt = 0; attempt <= MAX_TXN_RETRIES; attempt++) {

        /* Reset response state on retry */
        if (attempt > 0) {
            for (uint32_t i = 0; i < req->resp_header_count; i++) {
                free(req->resp_header_names[i]);
                free(req->resp_header_values[i]);
            }
            free(req->resp_header_names);
            free(req->resp_header_values);
            req->resp_status = 200;
            req->resp_header_names = NULL;
            req->resp_header_values = NULL;
            req->resp_header_count = 0;
            req->resp_header_cap = 0;

            free(bytecode);
            bytecode = NULL;
            if (kv_get(sjs->kv, ck, &bytecode, &bc_len) != 0) {
                free(module_path); free(url_func); free(query_str);
                free(req->session_id); req->session_id = NULL;
                req->resp_status = 500;
                char *body = strdup("Internal Server Error");
                *out_len = (uint32_t)strlen(body);
                return body;
            }
        }

        /* Phase 2: Restore snapshot into a fresh arena */
        sjs_arena_t *arena = sjs->arena;

        JSRuntime *rt = NULL;
        JSContext *ctx = NULL;
        if (snapshot_restore(&sjs->snapshot, arena, sjs, &rt, &ctx) != 0) {
            free(bytecode); free(module_path); free(url_func); free(query_str);
            free(req->session_id); req->session_id = NULL;
            req->resp_status = 500;
            char *body = strdup("Internal Server Error");
            *out_len = (uint32_t)strlen(body);
            return body;
        }

        JS_SetContextOpaque(ctx, req);

        /* Phase 3: Begin transaction, load bytecode, evaluate module */
        if (kv_begin(sjs->kv) == KV_CONFLICT) {
            arena_reset(sjs);
            continue;
        }

        /* Load session data from KV into session.__data */
        req->session_dirty = false;
        if (req->session_id && !req->session_new) {
            char sess_key[80];
            snprintf(sess_key, sizeof(sess_key), "sessions/%s", req->session_id);

            char pfx_buf[512];
            const char *actual_key = sjs_prefixed_key(req->kv_prefix, sess_key,
                                                       pfx_buf, sizeof(pfx_buf));
            if (actual_key) {
                void  *sdata = NULL;
                size_t sdata_len = 0;
                if (kv_get(sjs->kv, actual_key, &sdata, &sdata_len) == 0 && sdata) {
                    /* Parse JSON into session.__data */
                    JSValue global = JS_GetGlobalObject(ctx);
                    JSValue json_obj = JS_GetPropertyStr(ctx, global, "JSON");
                    JSValue parse_fn = JS_GetPropertyStr(ctx, json_obj, "parse");
                    JSValue json_str = JS_NewStringLen(ctx, sdata, sdata_len);
                    JSValue parsed = JS_Call(ctx, parse_fn, json_obj, 1, &json_str);

                    if (!JS_IsException(parsed)) {
                        JSValue sess = JS_GetPropertyStr(ctx, global, "session");
                        JS_SetPropertyStr(ctx, sess, "__data", parsed);
                        JS_FreeValue(ctx, sess);
                    } else {
                        JS_GetException(ctx); /* clear */
                    }

                    JS_FreeValue(ctx, global);
                    free(sdata);
                }
            }
        }

        JSValue module_val = JS_ReadObject(ctx, bytecode, bc_len,
                                           JS_READ_OBJ_BYTECODE);
        free(bytecode);
        bytecode = NULL;

        if (JS_IsException(module_val)) {
            const char *err = js_err_string(ctx, arena);
            fprintf(stderr, "shift-js: bytecode load error in %s: %s\n",
                    module_path, err);
            char *body;
            asprintf(&body, "bytecode load error in %s: %s",
                     module_path, err);
            kv_rollback(sjs->kv);
            arena_reset(sjs);
            free(module_path); free(url_func); free(query_str);
            free(req->session_id); req->session_id = NULL;
            req->resp_status = 500;
            *out_len = (uint32_t)strlen(body);
            return body;
        }

        JSModuleDef *mod_def = JS_VALUE_GET_PTR(module_val);

        JSValue result = JS_EvalFunction(ctx, module_val);
        if (JS_IsException(result)) {
            const char *err = js_err_string(ctx, arena);
            fprintf(stderr, "shift-js: module eval error in %s: %s\n",
                    module_path, err);
            asprintf(&err_msg, "module eval error in %s: %s",
                     module_path, err);
            goto txn_fail;
        }

        /* Pump microtask queue — module eval returns a promise */
        {
            JSContext *pctx;
            while (JS_IsJobPending(rt))
                JS_ExecutePendingJob(rt, &pctx);
        }

        if (JS_PromiseState(ctx, result) == JS_PROMISE_REJECTED) {
            JSValue reason = JS_PromiseResult(ctx, result);
            const char *err = JS_ToCString(ctx, reason);
            if (!err) err = "(unknown)";
            fprintf(stderr, "shift-js: module rejected in %s: %s\n",
                    module_path, err);
            asprintf(&err_msg, "module rejected in %s: %s",
                     module_path, err);
            goto txn_fail;
        }

        /* ---- Dispatch: check for __render to choose mode ---- */
        JSValue ns = JS_GetModuleNamespace(ctx, mod_def);
        JSValue render_fn = JS_GetPropertyStr(ctx, ns, "__render");
        bool is_render = JS_IsFunction(ctx, render_fn);

        JSValue ret;
        const char *called_func = NULL;

        if (is_render) {
            /* ---- Render mode (EJS pages) ----
             * Call __render() for any HTTP method.
             * Auto-set content-type: text/html. */
            called_func = "__render";
            if (!resp_has_header(req, "content-type"))
                resp_add_header(req, "content-type", "text/html");

            ret = JS_Call(ctx, render_fn, JS_UNDEFINED, 0, NULL);
        } else {
            /* ---- API mode (.mjs modules) ----
             * Function name from URL path segment, or "index" as default.
             * GET:  args from query string
             * POST/PUT/PATCH/DELETE: args from JSON body */
            const char *fn_name = (url_func && url_func[0]) ? url_func : "index";
            called_func = fn_name;

            JSValue handler = JS_GetPropertyStr(ctx, ns, fn_name);
            if (!JS_IsFunction(ctx, handler)) {
                kv_rollback(sjs->kv);
                arena_reset(sjs);
                req->resp_status = 404;
                char *body;
                asprintf(&body, "function \"%s\" not found", fn_name);
                free(module_path); free(url_func); free(query_str);
                free(req->session_id); req->session_id = NULL;
                *out_len = (uint32_t)strlen(body);
                return body;
            }

            /* Build args object from query string (GET) or JSON body (POST etc.) */
            JSValue args;
            if (!strcmp(req->method, "GET") || !strcmp(req->method, "HEAD")) {
                args = parse_query_string(ctx, query_str);
            } else if (req->body && req->body_len > 0) {
                /* Parse JSON body */
                JSValue global = JS_GetGlobalObject(ctx);
                JSValue json_obj = JS_GetPropertyStr(ctx, global, "JSON");
                JSValue parse_fn = JS_GetPropertyStr(ctx, json_obj, "parse");
                JSValue body_str = JS_NewStringLen(ctx, req->body, req->body_len);
                args = JS_Call(ctx, parse_fn, json_obj, 1, &body_str);
                if (JS_IsException(args)) {
                    /* Body is not valid JSON — pass as string */
                    JS_GetException(ctx);  /* clear */
                    args = JS_NewObject(ctx);
                    JS_SetPropertyStr(ctx, args, "body",
                                      JS_NewStringLen(ctx, req->body, req->body_len));
                }
            } else {
                args = JS_NewObject(ctx);
            }

            ret = JS_Call(ctx, handler, JS_UNDEFINED, 1, &args);
        }

        /* Capture exception immediately */
        if (JS_IsException(ret)) {
            const char *err = js_err_string(ctx, arena);
            fprintf(stderr, "shift-js: handler error in %s.%s: %s\n",
                    module_path, called_func, err);
            asprintf(&err_msg, "handler error in %s.%s: %s",
                     module_path, called_func, err);
            goto txn_fail;
        }

        /* Pump microtasks after handler call */
        {
            JSContext *pctx;
            while (JS_IsJobPending(rt))
                JS_ExecutePendingJob(rt, &pctx);
        }

        /* Phase 4: Extract response body to libc heap */
        char *body = extract_body_string(ctx, arena, req, ret,
                                         module_path, called_func,
                                         out_len, &err_msg);
        if (err_msg) goto txn_fail;

        /* JSONP wrapping for API mode GET requests */
        if (!is_render && body &&
            (!strcmp(req->method, "GET") || !strcmp(req->method, "HEAD"))) {
            /* Look for "callback" in query string */
            const char *cb = NULL;
            char *cb_buf = NULL;
            if (query_str) {
                /* Quick scan for callback= param */
                char *qs_copy = strdup(query_str);
                char *p = qs_copy;
                while (p && *p) {
                    char *amp = strchr(p, '&');
                    if (amp) *amp = '\0';
                    if (strncmp(p, "callback=", 9) == 0) {
                        cb_buf = strdup(p + 9);
                        url_decode(cb_buf, strlen(cb_buf));
                        cb = cb_buf;
                        break;
                    }
                    p = amp ? amp + 1 : NULL;
                }
                free(qs_copy);
            }
            if (cb && cb[0]) {
                /* Validate callback name (alphanumeric + _ + . only) */
                bool valid = true;
                for (const char *c = cb; *c; c++) {
                    if (!((*c >= 'a' && *c <= 'z') ||
                          (*c >= 'A' && *c <= 'Z') ||
                          (*c >= '0' && *c <= '9') ||
                          *c == '_' || *c == '.')) {
                        valid = false;
                        break;
                    }
                }
                if (valid) {
                    /* Wrap: callback(body); */
                    size_t cb_len = strlen(cb);
                    size_t body_len = *out_len;
                    size_t wrapped_len = cb_len + 1 + body_len + 2; /* cb(body);\n */
                    char *wrapped = malloc(wrapped_len);
                    if (wrapped) {
                        memcpy(wrapped, cb, cb_len);
                        wrapped[cb_len] = '(';
                        memcpy(wrapped + cb_len + 1, body, body_len);
                        wrapped[cb_len + 1 + body_len] = ')';
                        wrapped[cb_len + 1 + body_len + 1] = ';';
                        free(body);
                        body = wrapped;
                        *out_len = (uint32_t)wrapped_len;

                        /* Override content-type for JSONP */
                        for (uint32_t i = 0; i < req->resp_header_count; i++) {
                            if (!strcasecmp(req->resp_header_names[i], "content-type")) {
                                free(req->resp_header_values[i]);
                                req->resp_header_values[i] = strdup("application/javascript");
                                break;
                            }
                        }
                    }
                }
            }
            free(cb_buf);
        }

        /* ---- Session: persist if dirty, set cookie if new ---- */
        if (req->session_id && (req->session_dirty || req->session_new)) {
            if (req->session_dirty) {
                /* Serialize session.__data to JSON and write to KV */
                JSValue global = JS_GetGlobalObject(ctx);
                JSValue sess = JS_GetPropertyStr(ctx, global, "session");
                JSValue data = JS_GetPropertyStr(ctx, sess, "__data");
                JSValue json_obj = JS_GetPropertyStr(ctx, global, "JSON");
                JSValue stringify = JS_GetPropertyStr(ctx, json_obj, "stringify");
                JSValue json_val = JS_Call(ctx, stringify, json_obj, 1, &data);

                if (JS_IsString(json_val)) {
                    size_t jlen;
                    const char *jstr = JS_ToCStringLen(ctx, &jlen, json_val);
                    if (jstr) {
                        char sess_key[80];
                        snprintf(sess_key, sizeof(sess_key),
                                 "sessions/%s", req->session_id);
                        char pfx_buf[512];
                        const char *actual_key = sjs_prefixed_key(
                            req->kv_prefix, sess_key, pfx_buf, sizeof(pfx_buf));
                        if (actual_key)
                            kv_put(sjs->kv, actual_key, jstr, jlen);
                    }
                }

                JS_FreeValue(ctx, global);
            }

            if (req->session_new) {
                char *cookie = sjs_session_cookie_header(req->session_id);
                if (cookie) {
                    resp_add_header(req, "set-cookie", cookie);
                    free(cookie);
                }
            }
        }

        /* Commit — retry on conflict */
        if (kv_commit(sjs->kv) == KV_CONFLICT) {
            free(body);
            arena_reset(sjs);
            continue;
        }

        arena_reset(sjs);
        free(module_path); free(url_func); free(query_str);
        free(req->session_id);
        req->session_id = NULL;

        if (!body) {
            body = strdup("");
            *out_len = 0;
        }
        return body;

    txn_fail:
        kv_rollback(sjs->kv);
        arena_reset(sjs);
        free(module_path); free(url_func); free(query_str);
        free(req->session_id); req->session_id = NULL;
        req->resp_status = 500;
        {
            char *body = err_msg ? err_msg : strdup("Internal Server Error");
            *out_len = (uint32_t)strlen(body);
            return body;
        }
    }

    /* All retries exhausted */
    free(bytecode);
    free(module_path); free(url_func); free(query_str);
    free(req->session_id); req->session_id = NULL;
    req->resp_status = 409;
    char *body = strdup("Transaction Conflict");
    *out_len = (uint32_t)strlen(body);
    return body;
}
