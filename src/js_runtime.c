#include "js_runtime.h"
#include "kvstore.h"
#include "router.h"

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

static sjs_arena_t *arena_acquire(sjs_runtime_t *sjs) {
    sjs_arena_t *a = sjs->arena_free;
    if (a) {
        sjs->arena_free = a->next;
        a->next = NULL;
        a->used = 0;
        return a;
    }
    return NULL;
}

static void arena_release(sjs_runtime_t *sjs, sjs_arena_t *a) {
    a->next = sjs->arena_free;
    sjs->arena_free = a;
}

/* ======================================================================
 * kv global
 * ====================================================================== */

static kvstore_t *js_get_kv(JSContext *ctx) {
    sjs_runtime_t *sjs = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    return sjs ? sjs->kv : NULL;
}

static JSValue js_kv_get(JSContext *ctx, JSValue this_val,
                         int argc, JSValue *argv) {
    kvstore_t *kv = js_get_kv(ctx);
    if (!kv) return JS_EXCEPTION;

    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    void  *value = NULL;
    size_t vlen  = 0;
    int rc = kv_get(kv, key, &value, &vlen);
    JS_FreeCString(ctx, key);

    if (rc == -1) return JS_NULL;
    if (rc < 0)   return JS_EXCEPTION;

    JSValue result = JS_NewStringLen(ctx, value, vlen);
    free(value);
    return result;
}

static JSValue js_kv_put(JSContext *ctx, JSValue this_val,
                         int argc, JSValue *argv) {
    kvstore_t *kv = js_get_kv(ctx);
    if (!kv) return JS_EXCEPTION;

    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    size_t vlen;
    const char *val = JS_ToCStringLen(ctx, &vlen, argv[1]);
    if (!val) { JS_FreeCString(ctx, key); return JS_EXCEPTION; }

    int rc = kv_put(kv, key, val, vlen);
    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, val);

    return rc == 0 ? JS_TRUE : JS_EXCEPTION;
}

static JSValue js_kv_delete(JSContext *ctx, JSValue this_val,
                            int argc, JSValue *argv) {
    kvstore_t *kv = js_get_kv(ctx);
    if (!kv) return JS_EXCEPTION;

    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    int rc = kv_delete(kv, key);
    JS_FreeCString(ctx, key);

    return rc == 0 ? JS_TRUE : JS_EXCEPTION;
}

static JSValue js_kv_range(JSContext *ctx, JSValue this_val,
                           int argc, JSValue *argv) {
    kvstore_t *kv = js_get_kv(ctx);
    if (!kv) return JS_EXCEPTION;

    const char *start = JS_ToCString(ctx, argv[0]);
    if (!start) return JS_EXCEPTION;

    const char *end = JS_ToCString(ctx, argv[1]);
    if (!end) { JS_FreeCString(ctx, start); return JS_EXCEPTION; }

    int64_t count = 100;
    if (argc > 2) JS_ToInt64(ctx, &count, argv[2]);

    kv_range_result_t result;
    int rc = kv_range(kv, start, end, (size_t)count, &result);
    JS_FreeCString(ctx, start);
    JS_FreeCString(ctx, end);

    if (rc < 0) return JS_EXCEPTION;

    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < result.count; i++) {
        JSValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "key",
                          JS_NewString(ctx, result.entries[i].key));
        JS_SetPropertyStr(ctx, entry, "value",
                          JS_NewStringLen(ctx, result.entries[i].value,
                                          result.entries[i].value_len));
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
    if (!req) return JS_EXCEPTION;

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
    if (!req) return JS_EXCEPTION;

    int32_t code;
    if (JS_ToInt32(ctx, &code, argv[0])) return JS_EXCEPTION;
    req->resp_status = (uint16_t)code;
    return JS_UNDEFINED;
}

static JSValue js_response_header(JSContext *ctx, JSValue this_val,
                                  int argc, JSValue *argv) {
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (!req) return JS_EXCEPTION;

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
            return JS_EXCEPTION;
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
 * Module loader (compile context only)
 * ====================================================================== */

static JSModuleDef *sjs_module_loader(JSContext *ctx, const char *module_name,
                                      void *opaque) {
    sjs_runtime_t *sjs = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    kvstore_t *kv = sjs ? sjs->kv : NULL;
    if (!kv) return NULL;

    size_t name_len = strlen(module_name);
    size_t key_len = 7 + name_len;
    char *key = malloc(key_len + 1);
    if (!key) return NULL;
    snprintf(key, key_len + 1, "__code/%s", module_name);

    void  *source = NULL;
    size_t source_len = 0;
    int rc = kv_get(kv, key, &source, &source_len);
    free(key);

    if (rc != 0) return NULL;

    JSValue func = JS_Eval(ctx, source, source_len, module_name,
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(source);

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
    /* Use one arena from the pool to build the template */
    sjs_arena_t *arena = arena_acquire(sjs);
    if (!arena) return -1;

    JSRuntime *rt = JS_NewRuntime2(&bump_mf, arena);
    if (!rt) { arena_release(sjs, arena); return -1; }

    JS_SetRuntimeOpaque(rt, sjs);

    JSContext *ctx = JS_NewContextRaw(rt);
    if (!ctx) { arena_release(sjs, arena); return -1; }

    JS_AddIntrinsicBaseObjects(ctx);
    JS_AddIntrinsicJSON(ctx);
    JS_AddIntrinsicPromise(ctx);

    /* Install globals with NULL request context — just the structure */
    js_install_kv(ctx);
    js_install_request(ctx);
    js_install_response(ctx);

    /* Save the arena content */
    snap->used = arena->used;
    snap->data = malloc(snap->used);
    if (!snap->data) { arena_release(sjs, arena); return -1; }
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
    if (!snap->bitmap) { free(snap->data); arena_release(sjs, arena); return -1; }

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

    /* Return the arena to the pool — the snapshot is in snap->data */
    arena_release(sjs, arena);
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

int sjs_runtime_init(sjs_runtime_t *sjs, kvstore_t *kv) {
    memset(sjs, 0, sizeof(*sjs));
    sjs->kv = kv;

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

    /* Pre-allocate arena pool.
     * Each arena is sizeof(sjs_arena_t) + SJS_ARENA_SIZE bytes.
     * All arenas are in one contiguous allocation for cache friendliness. */
    size_t per_arena = sizeof(sjs_arena_t) + SJS_ARENA_SIZE;
    sjs->arena_pool = malloc(per_arena * SJS_ARENA_POOL);
    if (!sjs->arena_pool) {
        JS_FreeContext(sjs->compile_ctx);
        JS_FreeRuntime(sjs->compile_rt);
        return -1;
    }

    sjs->arena_free = NULL;
    for (int i = 0; i < SJS_ARENA_POOL; i++) {
        sjs_arena_t *a = (sjs_arena_t *)((char *)sjs->arena_pool + i * per_arena);
        a->used = 0;
        a->next = sjs->arena_free;
        sjs->arena_free = a;
    }

    /* Create the frozen snapshot of a fully-initialized runtime+context.
     * This is the template that gets memcpy'd + relocated per request. */
    if (snapshot_create(sjs, &sjs->snapshot) != 0) {
        fprintf(stderr, "shift-js: snapshot_create failed\n");
        free(sjs->arena_pool);
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
    free(sjs->arena_pool);
    sjs->arena_pool = NULL;
    sjs->arena_free = NULL;
}

/* ======================================================================
 * Request dispatch
 * ====================================================================== */

static const char *method_to_func(const char *method) {
    if (!strcmp(method, "GET"))     return "get";
    if (!strcmp(method, "POST"))    return "post";
    if (!strcmp(method, "PUT"))     return "put";
    if (!strcmp(method, "PATCH"))   return "patch";
    if (!strcmp(method, "DELETE"))  return "destroy";
    if (!strcmp(method, "HEAD"))    return "head";
    if (!strcmp(method, "OPTIONS")) return "options";
    return NULL;
}

char *sjs_dispatch(sjs_runtime_t *sjs, sjs_request_ctx_t *req,
                   uint32_t *out_len) {
    req->resp_status = 200;
    req->resp_header_names = NULL;
    req->resp_header_values = NULL;
    req->resp_header_count = 0;
    req->resp_header_cap = 0;

    const char *func_name = method_to_func(req->method);
    if (!func_name) {
        req->resp_status = 405;
        char *body = strdup("Method Not Allowed");
        *out_len = (uint32_t)strlen(body);
        return body;
    }

    char *module_path = sjs_resolve_module(req->path);
    if (!module_path) {
        req->resp_status = 500;
        char *body = strdup("Internal Server Error");
        *out_len = (uint32_t)strlen(body);
        return body;
    }

    /* ---- Phase 1: Ensure bytecode exists in KV cache ---- */
    char cache_key[512];
    snprintf(cache_key, sizeof(cache_key), "__compiled/%s", module_path);

    void  *bytecode = NULL;
    size_t bc_len = 0;

    if (kv_get(sjs->kv, cache_key, &bytecode, &bc_len) != 0) {
        char kv_key[512];
        snprintf(kv_key, sizeof(kv_key), "__code/%s", module_path);

        void  *source = NULL;
        size_t source_len = 0;
        if (kv_get(sjs->kv, kv_key, &source, &source_len) != 0) {
            free(module_path);
            req->resp_status = 404;
            char *body = strdup("Not Found");
            *out_len = (uint32_t)strlen(body);
            return body;
        }

        JSContext *cc = sjs->compile_ctx;
        JSValue module_val = JS_Eval(cc, source, source_len, module_path,
                                     JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        free(source);

        if (JS_IsException(module_val)) {
            JSValue exc = JS_GetException(cc);
            const char *err = JS_ToCString(cc, exc);
            fprintf(stderr, "shift-js: compile error in %s: %s\n",
                    module_path, err ? err : "(unknown)");
            JS_FreeCString(cc, err);
            JS_FreeValue(cc, exc);
            free(module_path);
            req->resp_status = 500;
            char *body = strdup("Module Compilation Error");
            *out_len = (uint32_t)strlen(body);
            return body;
        }

        size_t out_bc_len;
        uint8_t *bc = JS_WriteObject(cc, &out_bc_len, module_val,
                                     JS_WRITE_OBJ_BYTECODE);
        JS_FreeValue(cc, module_val);

        if (!bc) {
            free(module_path);
            req->resp_status = 500;
            char *body = strdup("Bytecode Serialization Error");
            *out_len = (uint32_t)strlen(body);
            return body;
        }

        kv_put(sjs->kv, cache_key, bc, out_bc_len);
        bytecode = bc;
        bc_len = out_bc_len;
    }

    /* ---- Phase 2: Restore snapshot into a fresh arena ---- */
    sjs_arena_t *arena = arena_acquire(sjs);
    if (!arena) {
        free(bytecode);
        free(module_path);
        req->resp_status = 503;
        char *body = strdup("Arena Pool Exhausted");
        *out_len = (uint32_t)strlen(body);
        return body;
    }

    JSRuntime *rt = NULL;
    JSContext *ctx = NULL;
    if (snapshot_restore(&sjs->snapshot, arena, sjs, &rt, &ctx) != 0) {
        arena_release(sjs, arena);
        free(bytecode);
        free(module_path);
        req->resp_status = 500;
        char *body = strdup("Internal Server Error");
        *out_len = (uint32_t)strlen(body);
        return body;
    }

    JS_SetContextOpaque(ctx, req);

    /* ---- Phase 3: Load bytecode, evaluate module, call handler ---- */
    JSValue module_val = JS_ReadObject(ctx, bytecode, bc_len,
                                       JS_READ_OBJ_BYTECODE);
    free(bytecode);
    bytecode = NULL;

    if (JS_IsException(module_val)) {
        const char *err = JS_ToCString(ctx, JS_GetException(ctx));
        fprintf(stderr, "shift-js: bytecode load error in %s: %s\n",
                module_path, err ? err : "(unknown)");
        goto arena_fail;
    }

    JSModuleDef *mod_def = JS_VALUE_GET_PTR(module_val);

    JSValue result = JS_EvalFunction(ctx, module_val);
    if (JS_IsException(result)) {
        const char *err = JS_ToCString(ctx, JS_GetException(ctx));
        fprintf(stderr, "shift-js: module eval error in %s: %s\n",
                module_path, err ? err : "(unknown)");
        goto arena_fail;
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
        fprintf(stderr, "shift-js: module rejected in %s: %s\n",
                module_path, err ? err : "(unknown)");
        goto arena_fail;
    }

    JSValue ns = JS_GetModuleNamespace(ctx, mod_def);
    JSValue handler = JS_GetPropertyStr(ctx, ns, func_name);

    if (!JS_IsFunction(ctx, handler)) {
        arena_release(sjs, arena);
        free(module_path);
        req->resp_status = 405;
        char *body = strdup("Method Not Allowed");
        *out_len = (uint32_t)strlen(body);
        return body;
    }

    JSValue ret = JS_Call(ctx, handler, JS_UNDEFINED, 0, NULL);

    /* Pump microtasks after handler call */
    {
        JSContext *pctx;
        while (JS_IsJobPending(rt))
            JS_ExecutePendingJob(rt, &pctx);
    }

    if (JS_IsException(ret)) {
        const char *err = JS_ToCString(ctx, JS_GetException(ctx));
        fprintf(stderr, "shift-js: handler error in %s.%s: %s\n",
                module_path, func_name, err ? err : "(unknown)");
        goto arena_fail;
    }

    /* ---- Phase 4: Extract response body to libc heap ---- */
    char *body = NULL;

    if (JS_IsString(ret)) {
        size_t len;
        const char *str = JS_ToCStringLen(ctx, &len, ret);
        body = malloc(len);
        if (body) {
            memcpy(body, str, len);
            *out_len = (uint32_t)len;
        }
    } else if (!JS_IsUndefined(ret) && !JS_IsNull(ret)) {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue json = JS_GetPropertyStr(ctx, global, "JSON");
        JSValue stringify = JS_GetPropertyStr(ctx, json, "stringify");

        JSValue json_str = JS_Call(ctx, stringify, json, 1, &ret);
        if (JS_IsString(json_str)) {
            size_t len;
            const char *str = JS_ToCStringLen(ctx, &len, json_str);
            body = malloc(len);
            if (body) {
                memcpy(body, str, len);
                *out_len = (uint32_t)len;
            }

            bool has_ct = false;
            for (uint32_t i = 0; i < req->resp_header_count; i++) {
                if (!strcasecmp(req->resp_header_names[i], "content-type")) {
                    has_ct = true;
                    break;
                }
            }
            if (!has_ct) {
                if (req->resp_header_count == req->resp_header_cap) {
                    uint32_t nc = req->resp_header_cap ? req->resp_header_cap * 2 : 8;
                    req->resp_header_names = realloc(req->resp_header_names, nc * sizeof(char *));
                    req->resp_header_values = realloc(req->resp_header_values, nc * sizeof(char *));
                    req->resp_header_cap = nc;
                }
                req->resp_header_names[req->resp_header_count] = strdup("content-type");
                req->resp_header_values[req->resp_header_count] = strdup("application/json");
                req->resp_header_count++;
            }
        }
    }

    /* No JS teardown — just release the arena back to the free list. */
    arena_release(sjs, arena);
    free(module_path);

    if (!body) {
        body = strdup("");
        *out_len = 0;
    }
    return body;

arena_fail:
    arena_release(sjs, arena);
    free(bytecode);
    free(module_path);
    req->resp_status = 500;
    {
        char *body = strdup("Internal Server Error");
        *out_len = (uint32_t)strlen(body);
        return body;
    }
}
