#define _GNU_SOURCE
#include "js_runtime.h"
#include "crypto.h"
#include "kvstore.h"
#include "preprocessor.h"
#include "router.h"
#include "session.h"

#include <quickjs.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>
#include <inttypes.h>

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

    /* Capture kv read for replay */
    {
        sjs_request_ctx_t *req = JS_GetContextOpaque(ctx);
        if (req && req->replay_capture)
            replay_capture_kv_get(req->replay_capture, key,
                                  rc == 0 ? value : NULL,
                                  rc == 0 ? vlen : 0);
    }

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

    /* When raft is active, stamp each KV row with the transaction's seq.
     * Allocate the seq lazily on first write in the transaction. */
    sjs_request_ctx_t *req = JS_GetContextOpaque(ctx);
    int rc;
    if (req && req->write_set) {
        if (req->raft_seq == 0)
            req->raft_seq = kv_next_seq(kv);
        rc = kv_put_seq(kv, actual_key, val, vlen, req->raft_seq);
    } else {
        rc = kv_put(kv, actual_key, val, vlen);
    }

    /* Track the write for Raft replication */
    if (rc == 0 && req && req->write_set)
        raft_write_set_add_put(req->write_set, actual_key, val, (uint32_t)vlen);

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

    /* Track the delete for Raft replication */
    sjs_request_ctx_t *req = JS_GetContextOpaque(ctx);
    if (rc == 0 && req && req->write_set)
        raft_write_set_add_delete(req->write_set, actual_key);

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

    if (rc < 0) {
        JS_FreeCString(ctx, start);
        JS_FreeCString(ctx, end);
        return JS_ThrowInternalError(ctx, "kv.range failed (rc=%d)", rc);
    }

    size_t prefix_len = (prefix && prefix[0]) ? strlen(prefix) : 0;

    /* Capture kv range read for replay */
    {
        sjs_request_ctx_t *req = JS_GetContextOpaque(ctx);
        if (req && req->replay_capture)
            replay_capture_kv_range(req->replay_capture, start, end,
                                    &result, prefix_len);
    }

    JS_FreeCString(ctx, start);
    JS_FreeCString(ctx, end);

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
 * code global (module source management with content hashing)
 * ====================================================================== */

/* Compute SHA-256 hex string of content. Returns static thread-local buffer. */
static const char *sha256_hex(const void *data, size_t len) {
    static _Thread_local char hex[65];
    uint8_t digest[32];
    unsigned int dlen = 0;
    EVP_Digest(data, len, digest, &dlen, EVP_sha256(), NULL);
    for (unsigned int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return hex;
}

static JSValue js_code_get(JSContext *ctx, JSValue this_val,
                            int argc, JSValue *argv) {
    kvstore_t *kv = js_get_kv(ctx);
    if (!kv) return JS_ThrowInternalError(ctx, "kv store not available");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    char raw_key[256];
    snprintf(raw_key, sizeof(raw_key), "__code/%s", path);
    JS_FreeCString(ctx, path);

    char pfx_buf[512];
    const char *actual_key = sjs_prefixed_key(js_get_prefix(ctx), raw_key,
                                               pfx_buf, sizeof(pfx_buf));
    if (!actual_key) return JS_ThrowRangeError(ctx, "code path too long");

    void  *value = NULL;
    size_t vlen  = 0;
    int rc = kv_get(kv, actual_key, &value, &vlen);

    if (rc == -1) return JS_NULL;
    if (rc < 0) return JS_ThrowInternalError(ctx, "code.get failed");

    JSValue result = JS_NewStringLen(ctx, value, vlen);
    free(value);
    return result;
}

static JSValue js_code_put(JSContext *ctx, JSValue this_val,
                            int argc, JSValue *argv) {
    kvstore_t *kv = js_get_kv(ctx);
    if (!kv) return JS_ThrowInternalError(ctx, "kv store not available");
    sjs_request_ctx_t *req = JS_GetContextOpaque(ctx);

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    size_t content_len;
    const char *content = JS_ToCStringLen(ctx, &content_len, argv[1]);
    if (!content) { JS_FreeCString(ctx, path); return JS_EXCEPTION; }

    const char *prefix = js_get_prefix(ctx);

    /* 1. Write source to __code/<path> */
    char raw_key[256];
    snprintf(raw_key, sizeof(raw_key), "__code/%s", path);
    char pfx_buf[512];
    const char *code_key = sjs_prefixed_key(prefix, raw_key,
                                             pfx_buf, sizeof(pfx_buf));
    if (!code_key) {
        JS_FreeCString(ctx, path);
        JS_FreeCString(ctx, content);
        return JS_ThrowRangeError(ctx, "code path too long");
    }

    int rc;
    if (req && req->write_set) {
        if (req->raft_seq == 0)
            req->raft_seq = kv_next_seq(kv);
        rc = kv_put_seq(kv, code_key, content, content_len, req->raft_seq);
    } else {
        rc = kv_put(kv, code_key, content, content_len);
    }
    if (rc == 0 && req && req->write_set)
        raft_write_set_add_put(req->write_set, code_key, content,
                               (uint32_t)content_len);

    /* 2. Compute content hash, store meta and content-addressed blob */
    if (rc == 0) {
        const char *hash = sha256_hex(content, content_len);

        /* __code_meta/<path> = hash (latest version pointer) */
        char meta_raw[256];
        snprintf(meta_raw, sizeof(meta_raw), "__code_meta/%s", path);
        char meta_buf[512];
        const char *meta_key = sjs_prefixed_key(prefix, meta_raw,
                                                 meta_buf, sizeof(meta_buf));
        if (meta_key) {
            if (req && req->write_set)
                kv_put_seq(kv, meta_key, hash, 64, req->raft_seq);
            else
                kv_put(kv, meta_key, hash, 64);
            if (req && req->write_set)
                raft_write_set_add_put(req->write_set, meta_key, hash, 64);
        }

        /* __code_blob/<hash> = content (immutable, deduplicated) */
        char blob_raw[128];
        snprintf(blob_raw, sizeof(blob_raw), "__code_blob/%s", hash);
        char blob_buf[512];
        const char *blob_key = sjs_prefixed_key(prefix, blob_raw,
                                                  blob_buf, sizeof(blob_buf));
        if (blob_key) {
            /* Only write if not already present (content-addressed = immutable) */
            void *existing = NULL;
            size_t elen = 0;
            if (kv_get(kv, blob_key, &existing, &elen) != 0) {
                if (req && req->write_set)
                    kv_put_seq(kv, blob_key, content, content_len, req->raft_seq);
                else
                    kv_put(kv, blob_key, content, content_len);
                if (req && req->write_set)
                    raft_write_set_add_put(req->write_set, blob_key, content,
                                           (uint32_t)content_len);
            }
            free(existing);
        }
    }

    /* 3. Invalidate bytecode cache: delete __compiled/<base> */
    if (rc == 0) {
        char base[256];
        snprintf(base, sizeof(base), "%s", path);
        char *dot = strrchr(base, '.');
        char *slash = strrchr(base, '/');
        if (dot && (!slash || dot > slash)) *dot = '\0';

        char cache_raw[256];
        snprintf(cache_raw, sizeof(cache_raw), "__compiled/%s", base);
        char cache_buf[512];
        const char *cache_key = sjs_prefixed_key(prefix, cache_raw,
                                                   cache_buf, sizeof(cache_buf));
        if (cache_key) {
            kv_delete(kv, cache_key);
            if (req && req->write_set)
                raft_write_set_add_delete(req->write_set, cache_key);
        }
    }

    JS_FreeCString(ctx, path);
    JS_FreeCString(ctx, content);

    if (rc != 0) return JS_ThrowInternalError(ctx, "code.put failed");
    return JS_TRUE;
}

static JSValue js_code_delete(JSContext *ctx, JSValue this_val,
                               int argc, JSValue *argv) {
    kvstore_t *kv = js_get_kv(ctx);
    if (!kv) return JS_ThrowInternalError(ctx, "kv store not available");
    sjs_request_ctx_t *req = JS_GetContextOpaque(ctx);

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    const char *prefix = js_get_prefix(ctx);

    /* Delete __code/<path> */
    char raw_key[256];
    snprintf(raw_key, sizeof(raw_key), "__code/%s", path);
    char pfx_buf[512];
    const char *code_key = sjs_prefixed_key(prefix, raw_key,
                                             pfx_buf, sizeof(pfx_buf));
    if (!code_key) {
        JS_FreeCString(ctx, path);
        return JS_ThrowRangeError(ctx, "code path too long");
    }

    int rc = kv_delete(kv, code_key);
    if (rc == 0 && req && req->write_set)
        raft_write_set_add_delete(req->write_set, code_key);

    /* Delete __code_meta/<path> */
    char meta_raw[256];
    snprintf(meta_raw, sizeof(meta_raw), "__code_meta/%s", path);
    char meta_buf[512];
    const char *meta_key = sjs_prefixed_key(prefix, meta_raw,
                                             meta_buf, sizeof(meta_buf));
    if (meta_key) {
        kv_delete(kv, meta_key);
        if (req && req->write_set)
            raft_write_set_add_delete(req->write_set, meta_key);
    }

    /* Delete __compiled/<base> */
    char base[256];
    snprintf(base, sizeof(base), "%s", path);
    char *dot = strrchr(base, '.');
    char *slash = strrchr(base, '/');
    if (dot && (!slash || dot > slash)) *dot = '\0';

    char cache_raw[256];
    snprintf(cache_raw, sizeof(cache_raw), "__compiled/%s", base);
    char cache_buf[512];
    const char *cache_key = sjs_prefixed_key(prefix, cache_raw,
                                               cache_buf, sizeof(cache_buf));
    if (cache_key) {
        kv_delete(kv, cache_key);
        if (req && req->write_set)
            raft_write_set_add_delete(req->write_set, cache_key);
    }

    JS_FreeCString(ctx, path);

    if (rc != 0) return JS_ThrowInternalError(ctx, "code.delete failed");
    return JS_TRUE;
}

static JSValue js_code_list(JSContext *ctx, JSValue this_val,
                             int argc, JSValue *argv) {
    kvstore_t *kv = js_get_kv(ctx);
    if (!kv) return JS_ThrowInternalError(ctx, "kv store not available");

    const char *prefix = js_get_prefix(ctx);

    char start_raw[] = "__code/";
    char end_raw[]   = "__code/\x7f";
    char start_buf[512], end_buf[512];
    const char *start = sjs_prefixed_key(prefix, start_raw,
                                          start_buf, sizeof(start_buf));
    const char *end = sjs_prefixed_key(prefix, end_raw,
                                        end_buf, sizeof(end_buf));
    if (!start || !end) return JS_NewArray(ctx);

    kv_range_result_t result;
    int rc = kv_range(kv, start, end, 10000, &result);
    if (rc < 0) return JS_NewArray(ctx);

    size_t prefix_len = (prefix && prefix[0]) ? strlen(prefix) : 0;

    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < result.count; i++) {
        JSValue entry = JS_NewObject(ctx);
        if (JS_IsException(entry)) {
            kv_range_free(&result);
            return JS_ThrowInternalError(ctx, "code.list: arena exhausted");
        }
        /* Strip prefix + "__code/" (7 chars) to get the module path */
        const char *full_key = result.entries[i].key + prefix_len + 7;
        JS_SetPropertyStr(ctx, entry, "path",
                          JS_NewString(ctx, full_key));
        JS_SetPropertyStr(ctx, entry, "size",
                          JS_NewInt64(ctx, (int64_t)result.entries[i].value_len));

        /* Look up content hash from __code_meta/ */
        char meta_raw[256];
        snprintf(meta_raw, sizeof(meta_raw), "__code_meta/%s", full_key);
        char meta_buf[512];
        const char *meta_key = sjs_prefixed_key(prefix, meta_raw,
                                                 meta_buf, sizeof(meta_buf));
        if (meta_key) {
            void *hash_val = NULL;
            size_t hash_len = 0;
            if (kv_get(kv, meta_key, &hash_val, &hash_len) == 0 && hash_val) {
                JS_SetPropertyStr(ctx, entry, "content_hash",
                                  JS_NewStringLen(ctx, hash_val, hash_len));
                free(hash_val);
            } else {
                JS_SetPropertyStr(ctx, entry, "content_hash", JS_NULL);
            }
        }

        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, entry);
    }

    kv_range_free(&result);
    return arr;
}

static void js_install_code(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue code = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, code, "get",
                      JS_NewCFunction(ctx, js_code_get, "get", 1));
    JS_SetPropertyStr(ctx, code, "put",
                      JS_NewCFunction(ctx, js_code_put, "put", 2));
    JS_SetPropertyStr(ctx, code, "delete",
                      JS_NewCFunction(ctx, js_code_delete, "delete", 1));
    JS_SetPropertyStr(ctx, code, "list",
                      JS_NewCFunction(ctx, js_code_list, "list", 0));

    JS_SetPropertyStr(ctx, global, "code", code);
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
    case 4: {
        char buf[24];
        snprintf(buf, sizeof(buf), "%" PRIu64, req->request_id);
        return JS_NewString(ctx, buf);
    }
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_request_props[] = {
    JS_CGETSET_MAGIC_DEF("method",  js_request_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("path",    js_request_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("body",    js_request_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("headers", js_request_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("id",      js_request_get, NULL, 4),
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
    req->resp_st->code = (uint16_t)code;
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

    sjs_resp_headers_t *h = req->resp_hdrs;
    if (h->count == h->cap) {
        uint32_t new_cap = h->cap ? h->cap * 2 : 8;
        char **nn = realloc(h->names, new_cap * sizeof(char *));
        char **nv = realloc(h->values, new_cap * sizeof(char *));
        if (!nn || !nv) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, value);
            return JS_ThrowInternalError(ctx, "response header allocation failed");
        }
        h->names = nn;
        h->values = nv;
        h->cap = new_cap;
    }

    h->names[h->count] = strdup(name);
    h->values[h->count] = strdup(value);
    h->count++;

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
    if (!req || !req->session || !req->session->id) return JS_NULL;
    return JS_NewString(ctx, req->session->id);
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

    req->session->is_dirty = true;
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

    req->session->is_dirty = true;
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
 * console global (write-only logging to per-worker log DB)
 * ====================================================================== */

static JSValue js_console_log_impl(JSContext *ctx, JSValue this_val,
                                    int argc, JSValue *argv, log_level_t level) {
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (!req || !req->log_batch)
        return JS_UNDEFINED;

    log_batch_t *batch = req->log_batch;
    if (batch->count >= LOG_BATCH_MAX)
        return JS_UNDEFINED;  /* silently drop */

    /* Stringify all arguments, space-separated, into stack buffer */
    char tmp[4096];
    size_t pos = 0;

    for (int i = 0; i < argc && pos < sizeof(tmp) - 1; i++) {
        if (i > 0 && pos < sizeof(tmp) - 1)
            tmp[pos++] = ' ';

        const char *str = JS_ToCString(ctx, argv[i]);
        if (!str) str = "(null)";
        size_t slen = strlen(str);
        size_t avail = sizeof(tmp) - 1 - pos;
        if (slen > avail) slen = avail;
        memcpy(tmp + pos, str, slen);
        pos += slen;
        JS_FreeCString(ctx, str);
    }
    tmp[pos] = '\0';

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    log_pending_t *e = &batch->entries[batch->count++];
    e->level = level;
    e->timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    /* strdup so lifetime is independent of the stack buffer and arena */
    e->msg = strndup(tmp, pos);
    e->msg_len = (uint32_t)pos;

    return JS_UNDEFINED;
}

static JSValue js_console_log(JSContext *ctx, JSValue this_val,
                               int argc, JSValue *argv) {
    return js_console_log_impl(ctx, this_val, argc, argv, LOG_LEVEL_LOG);
}

static JSValue js_console_warn(JSContext *ctx, JSValue this_val,
                                int argc, JSValue *argv) {
    return js_console_log_impl(ctx, this_val, argc, argv, LOG_LEVEL_WARN);
}

static JSValue js_console_error(JSContext *ctx, JSValue this_val,
                                 int argc, JSValue *argv) {
    return js_console_log_impl(ctx, this_val, argc, argv, LOG_LEVEL_ERROR);
}

static void js_install_console(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, console, "log",
                      JS_NewCFunction(ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, console, "warn",
                      JS_NewCFunction(ctx, js_console_warn, "warn", 1));
    JS_SetPropertyStr(ctx, console, "error",
                      JS_NewCFunction(ctx, js_console_error, "error", 1));

    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}

/* ======================================================================
 * logs global (read-only query against per-worker log DB)
 * ====================================================================== */

static const char *level_name(int level) {
    switch (level) {
    case LOG_LEVEL_LOG:   return "log";
    case LOG_LEVEL_WARN:  return "warn";
    case LOG_LEVEL_ERROR: return "error";
    default:              return "unknown";
    }
}

static int level_from_string(const char *s) {
    if (!s) return -1;
    if (strcmp(s, "log") == 0)   return LOG_LEVEL_LOG;
    if (strcmp(s, "warn") == 0)  return LOG_LEVEL_WARN;
    if (strcmp(s, "error") == 0) return LOG_LEVEL_ERROR;
    return -1;
}

static JSValue js_logs_query(JSContext *ctx, JSValue this_val,
                              int argc, JSValue *argv) {
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (!req || !req->log_db || !req->log_db->db)
        return JS_NewArray(ctx);

    /* Parse options: {level, limit, request_id, session_id, before, after} */
    int filter_level = -1;
    int64_t limit = 100;
    int64_t filter_request_id = -1;
    const char *filter_session_id = NULL;
    int64_t filter_before = 0;
    int64_t filter_after = 0;

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue opts = argv[0];
        JSValue v;

        v = JS_GetPropertyStr(ctx, opts, "level");
        if (JS_IsString(v)) {
            const char *s = JS_ToCString(ctx, v);
            filter_level = level_from_string(s);
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "limit");
        if (JS_IsNumber(v))
            JS_ToInt64(ctx, &limit, v);
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "request_id");
        if (JS_IsString(v)) {
            const char *s = JS_ToCString(ctx, v);
            if (s) filter_request_id = (int64_t)strtoull(s, NULL, 10);
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "session_id");
        if (JS_IsString(v))
            filter_session_id = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "before");
        if (JS_IsNumber(v))
            JS_ToInt64(ctx, &filter_before, v);
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "after");
        if (JS_IsNumber(v))
            JS_ToInt64(ctx, &filter_after, v);
        JS_FreeValue(ctx, v);
    }

    if (limit <= 0) limit = 100;
    if (limit > 1000) limit = 1000;

    /* Build query dynamically */
    char sql[512];
    size_t pos = 0;
    pos += snprintf(sql + pos, sizeof(sql) - pos,
                    "SELECT timestamp, worker_id, request_id, session_id, level, message "
                    "FROM logs WHERE 1=1");

    if (filter_level >= 0)
        pos += snprintf(sql + pos, sizeof(sql) - pos,
                        " AND level = %d", filter_level);
    if (filter_request_id >= 0)
        pos += snprintf(sql + pos, sizeof(sql) - pos,
                        " AND request_id = %" PRId64, filter_request_id);
    if (filter_session_id)
        pos += snprintf(sql + pos, sizeof(sql) - pos,
                        " AND session_id = ?");
    if (filter_before > 0)
        pos += snprintf(sql + pos, sizeof(sql) - pos,
                        " AND timestamp < %" PRId64, filter_before);
    if (filter_after > 0)
        pos += snprintf(sql + pos, sizeof(sql) - pos,
                        " AND timestamp > %" PRId64, filter_after);

    pos += snprintf(sql + pos, sizeof(sql) - pos,
                    " ORDER BY timestamp DESC LIMIT %" PRId64, limit);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(req->log_db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (filter_session_id) JS_FreeCString(ctx, filter_session_id);
        return JS_NewArray(ctx);
    }

    /* Bind session_id if present */
    if (filter_session_id) {
        sqlite3_bind_text(stmt, 1, filter_session_id, -1, SQLITE_STATIC);
    }

    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        JSValue entry = JS_NewObject(ctx);

        char ts_buf[24];
        snprintf(ts_buf, sizeof(ts_buf), "%" PRId64,
                 (int64_t)sqlite3_column_int64(stmt, 0));
        JS_SetPropertyStr(ctx, entry, "timestamp", JS_NewString(ctx, ts_buf));

        JS_SetPropertyStr(ctx, entry, "worker_id",
                          JS_NewInt32(ctx, sqlite3_column_int(stmt, 1)));

        char rid_buf[24];
        snprintf(rid_buf, sizeof(rid_buf), "%" PRId64,
                 (int64_t)sqlite3_column_int64(stmt, 2));
        JS_SetPropertyStr(ctx, entry, "request_id", JS_NewString(ctx, rid_buf));

        const char *sid = (const char *)sqlite3_column_text(stmt, 3);
        JS_SetPropertyStr(ctx, entry, "session_id",
                          sid ? JS_NewString(ctx, sid) : JS_NULL);

        JS_SetPropertyStr(ctx, entry, "level",
                          JS_NewString(ctx, level_name(sqlite3_column_int(stmt, 4))));

        const char *msg = (const char *)sqlite3_column_text(stmt, 5);
        JS_SetPropertyStr(ctx, entry, "message",
                          msg ? JS_NewString(ctx, msg) : JS_NewString(ctx, ""));

        JS_SetPropertyUint32(ctx, arr, idx++, entry);
    }

    sqlite3_finalize(stmt);
    if (filter_session_id) JS_FreeCString(ctx, filter_session_id);

    return arr;
}

static JSValue js_logs_replay(JSContext *ctx, JSValue this_val,
                              int argc, JSValue *argv) {
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (!req || !req->log_db || !req->log_db->db)
        return JS_NULL;

    const char *rid_str = JS_ToCString(ctx, argv[0]);
    if (!rid_str) return JS_EXCEPTION;
    uint64_t request_id = (uint64_t)strtoull(rid_str, NULL, 10);
    JS_FreeCString(ctx, rid_str);

    char *request_data = NULL, *response_data = NULL, *kv_tape = NULL;
    uint8_t *random_tape = NULL;
    size_t random_tape_len = 0;
    char *date_tape = NULL, *math_random_tape = NULL, *module_tree = NULL;
    char *source_maps = NULL;

    if (log_db_get_replay(req->log_db, request_id,
                          &request_data, &response_data, &kv_tape,
                          &random_tape, &random_tape_len,
                          &date_tape, &math_random_tape,
                          &module_tree, &source_maps) != 0)
        return JS_NULL;

    JSValue obj = JS_NewObject(ctx);

    /* Parse JSON strings into JS objects where appropriate */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue json_obj = JS_GetPropertyStr(ctx, global, "JSON");
    JSValue parse_fn = JS_GetPropertyStr(ctx, json_obj, "parse");

    #define PARSE_JSON_FIELD(field, cstr) do { \
        if (cstr) { \
            JSValue s = JS_NewString(ctx, cstr); \
            JSValue parsed = JS_Call(ctx, parse_fn, json_obj, 1, &s); \
            JS_SetPropertyStr(ctx, obj, field, parsed); \
            JS_FreeValue(ctx, s); \
            free(cstr); \
        } else { \
            JS_SetPropertyStr(ctx, obj, field, JS_NULL); \
        } \
    } while (0)

    PARSE_JSON_FIELD("request_data", request_data);
    PARSE_JSON_FIELD("response_data", response_data);
    PARSE_JSON_FIELD("kv_tape", kv_tape);
    PARSE_JSON_FIELD("date_tape", date_tape);
    PARSE_JSON_FIELD("math_random_tape", math_random_tape);
    PARSE_JSON_FIELD("module_tree", module_tree);
    PARSE_JSON_FIELD("source_maps", source_maps);
    #undef PARSE_JSON_FIELD

    /* random_tape as base64 — for now just hex-encode */
    if (random_tape && random_tape_len > 0) {
        char *hex = malloc(random_tape_len * 2 + 1);
        for (size_t i = 0; i < random_tape_len; i++)
            snprintf(hex + i * 2, 3, "%02x", random_tape[i]);
        JS_SetPropertyStr(ctx, obj, "random_tape",
                          JS_NewString(ctx, hex));
        free(hex);
        free(random_tape);
    } else {
        JS_SetPropertyStr(ctx, obj, "random_tape", JS_NULL);
    }

    JS_FreeValue(ctx, parse_fn);
    JS_FreeValue(ctx, json_obj);
    JS_FreeValue(ctx, global);

    return obj;
}

static void js_install_logs(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue logs = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, logs, "query",
                      JS_NewCFunction(ctx, js_logs_query, "query", 1));
    JS_SetPropertyStr(ctx, logs, "replay",
                      JS_NewCFunction(ctx, js_logs_replay, "replay", 1));

    JS_SetPropertyStr(ctx, global, "logs", logs);
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
    }

    JSValue func = JS_Eval(ctx, compile_source, compile_len, resolved_name,
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(compile_source);

    if (JS_IsException(func)) { free(resolved_alloc); return NULL; }

    /* Cache bytecode for imported modules so the per-request runtime can
     * load them without a full compile context. */
    size_t bc_len;
    uint8_t *bc = JS_WriteObject(ctx, &bc_len, func,
                                 JS_WRITE_OBJ_BYTECODE);
    if (bc) {
        /* Strip extension for cache key: "components/greeting.tsx" →
         * "__compiled/components/greeting" */
        char base[256];
        snprintf(base, sizeof(base), "%s", resolved_name);
        char *dot = strrchr(base, '.');
        char *slash = strrchr(base, '/');
        if (dot && (!slash || dot > slash)) *dot = '\0';

        char raw_cache[256];
        snprintf(raw_cache, sizeof(raw_cache), "__compiled/%s", base);
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
 *
 * Loads pre-compiled bytecode from KV for import resolution on the
 * per-request (snapshot-restored) runtime.
 * ====================================================================== */

static JSModuleDef *sjs_request_module_loader(JSContext *ctx,
                                              const char *module_name,
                                              void *opaque) {
    sjs_runtime_t *sjs = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    if (!sjs || !sjs->kv) return NULL;

    sjs_request_ctx_t *req = JS_GetContextOpaque(ctx);
    const char *prefix = req ? req->kv_prefix : NULL;

    /* Build cache key: strip extension, look up __compiled/<base> */
    char base[256];
    snprintf(base, sizeof(base), "%s", module_name);
    char *dot = strrchr(base, '.');
    char *slash = strrchr(base, '/');
    if (dot && (!slash || dot > slash)) *dot = '\0';

    char raw_cache[256];
    snprintf(raw_cache, sizeof(raw_cache), "__compiled/%s", base);
    char cache_buf[512];
    const char *ck = sjs_prefixed_key(prefix, raw_cache,
                                      cache_buf, sizeof(cache_buf));
    if (!ck) return NULL;

    void  *bc = NULL;
    size_t bc_len = 0;
    if (kv_get(sjs->kv, ck, &bc, &bc_len) != 0 || !bc) return NULL;

    JSValue module_val = JS_ReadObject(ctx, bc, bc_len, JS_READ_OBJ_BYTECODE);
    free(bc);

    if (JS_IsException(module_val)) return NULL;

    JSModuleDef *mod = JS_VALUE_GET_PTR(module_val);
    JS_FreeValue(ctx, module_val);
    return mod;
}

/* ======================================================================
 * Date.now / Math.random capture overrides
 *
 * Replace QuickJS built-in implementations with versions that record
 * return values to the replay capture tape.
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
    /* Generate a random double in [0, 1) using CSPRNG */
    uint64_t v;
    RAND_bytes((uint8_t *)&v, sizeof(v));

    /* Same conversion as QuickJS: map to [1.0, 2.0) then subtract 1 */
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

/* Initialize a JS runtime+context in the given arena with all intrinsics
 * and globals installed. Arena must be zeroed and empty. Returns offsets
 * of JSRuntime and JSContext within the arena data, or -1 on error. */
static int snapshot_init_runtime(sjs_arena_t *arena,
                                 size_t *out_rt_offset, size_t *out_ctx_offset) {
    arena->used = 0;

    JSRuntime *rt = JS_NewRuntime2(&bump_mf, arena);
    if (!rt) return -1;

    /* Do NOT call JS_SetRuntimeOpaque here — the runtime opaque is a
     * per-worker external pointer (sjs_runtime_t*) that must not be
     * baked into the snapshot.  It is set after each restore instead. */

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
    js_install_crypto(ctx);
    js_install_code(ctx);
    js_install_console(ctx);
    js_install_logs(ctx);

    /* Override Date.now, Date constructor, and Math.random with
     * capture versions that record to the replay tape. */
    {
        JSValue global = JS_GetGlobalObject(ctx);

        /* Override Date.now */
        JSValue date_obj = JS_GetPropertyStr(ctx, global, "Date");
        JS_SetPropertyStr(ctx, date_obj, "now",
                          JS_NewCFunction(ctx, js_date_now_capture, "now", 0));
        JS_FreeValue(ctx, date_obj);

        /* Wrap Date constructor so new Date() routes through Date.now().
         * This ensures no-arg Date construction is captured to the tape. */
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

        /* Override Math.random */
        JSValue math_obj = JS_GetPropertyStr(ctx, global, "Math");
        JS_SetPropertyStr(ctx, math_obj, "random",
                          JS_NewCFunction(ctx, js_math_random_capture, "random", 0));
        JS_FreeValue(ctx, math_obj);
        JS_FreeValue(ctx, global);
    }

    /* Disable stack checking — zeroes stack_limit (stack_top becomes
     * irrelevant).  Re-enabled after each snapshot restore. */
    JS_SetMaxStackSize(rt, 0);

    *out_rt_offset  = (char *)rt  - arena->data;
    *out_ctx_offset = (char *)ctx - arena->data;
    return 0;
}

static int snapshot_create(sjs_runtime_t *sjs, sjs_snapshot_t *snap) {
    sjs_arena_t *arena_a = sjs->arena;

    /* Zero before init so padding/alignment holes are deterministic. */
    memset(arena_a->data, 0, SJS_ARENA_SIZE);

    size_t rt_off, ctx_off;
    if (snapshot_init_runtime(arena_a, &rt_off, &ctx_off) != 0)
        return -1;

    size_t used_a = arena_a->used;

    /* Save arena A's content. */
    char *data_a = malloc(used_a);
    if (!data_a) return -1;
    memcpy(data_a, arena_a->data, used_a);

    /* Allocate a second arena at a different address and repeat. The
     * two copies let us diff: any 8-byte slot whose value shifted by
     * exactly (base_b - base_a) is a real pointer; everything else is
     * data.  This eliminates the old heuristic that could mistake
     * integers for pointers (or vice-versa) depending on heap layout. */
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

    /* Sanity: deterministic init must produce identical layout. */
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
            continue;  /* identical — data or external pointer, no relocation */

        if ((int64_t)(val_b - val_a) == base_delta) {
            /* Value shifted by exactly the arena base delta — real pointer. */
            snap->bitmap[i / 64] |= (uint64_t)1 << (i % 64);
            reloc_count++;
        } else {
            /* Non-deterministic data (e.g. random_state, time_origin).
             * Record its byte offset so restore can re-seed it, and
             * zero it in the snapshot. */
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

    /* Safety check: all known volatile fields (random_state, time_origin)
     * live inside the JSContext struct.  The JSRuntime volatile fields
     * (stack_top, stack_limit) are pre-zeroed via JS_SetMaxStackSize(rt,0).
     *
     * If a volatile slot appears outside the JSContext region, it means
     * an upstream QuickJS change introduced new non-deterministic state
     * that we haven't accounted for.  Abort rather than silently
     * corrupting data at runtime.
     *
     * We allow up to 2 volatile slots (random_state is always present;
     * time_origin appears when the two arena inits straddle a millisecond
     * boundary).  More than that also indicates an upstream change. */
    {
        /* JSContext is bump-allocated: bump_hdr_t (8 bytes) then the struct.
         * The struct ends before the next allocation after it. */
        size_t ctx_start = ctx_off;
        size_t ctx_end   = ctx_off + 1024; /* generous upper bound for JSContext */
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

    /* Patch runtime opaque to point to our per-worker sjs_runtime_t.
     *
     * The JSMallocState.opaque (arena pointer) is correctly handled by
     * the bitmap: the two-address diff detected it as a pointer because
     * the sjs_arena_t header sits at a fixed offset before arena->data,
     * so its delta matches the base delta. After relocation it points
     * to the restore arena's header — which is exactly right. */
    JS_SetRuntimeOpaque(rt, sjs);

    /* Re-initialize volatile fields that were zeroed in the snapshot.
     * These are non-deterministic data (timestamps, stack addresses)
     * that must be set to fresh per-thread/per-request values. */
    JS_UpdateStackTop(rt);
    JS_SetMaxStackSize(rt, JS_DEFAULT_STACK_SIZE);

    /* Re-seed remaining volatile slots (random_state, time_origin) with
     * a fresh microsecond timestamp. This gives each worker a unique
     * PRNG sequence and correct performance.now() baseline. */
    for (size_t i = 0; i < snap->volatile_count; i++) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        uint64_t seed = (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;
        if (seed == 0) seed = 1;  /* xorshift64* requires non-zero state */
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

    /* Copy the shared registry so we can patch per-worker user_data. */
    sjs->preprocessors_local = *preprocessors;
    sjs->preprocessors = &sjs->preprocessors_local;

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

    /* Initialize Sucrase TypeScript transpiler in the compile context and
     * patch the per-worker registry entries with the live context pointer. */
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

/* ======================================================================
 * Request dispatch
 * ====================================================================== */

/* ---- helpers: response header manipulation ---- */

static void resp_add_header(sjs_resp_headers_t *h,
                            const char *name, const char *value) {
    if (!h) return;
    if (h->count >= h->cap || !h->names || !h->values) {
        uint32_t nc = h->cap ? h->cap * 2 : 8;
        char **nn = realloc(h->names,  nc * sizeof(char *));
        char **nv = realloc(h->values, nc * sizeof(char *));
        if (!nn || !nv) return;
        h->names  = nn;
        h->values = nv;
        h->cap = nc;
    }
    h->names[h->count]  = strdup(name);
    h->values[h->count] = strdup(value);
    h->count++;
}

static bool resp_has_header(const sjs_resp_headers_t *h, const char *name) {
    for (uint32_t i = 0; i < h->count; i++)
        if (!strcasecmp(h->names[i], name)) return true;
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
 * positive on error (err_body filled, caller sets status). */
static int compile_module(sjs_runtime_t *sjs, const char *kv_prefix,
                          const char *base_path, const char *cache_key,
                          void **bytecode, size_t *bc_len,
                          char **out_module_path,
                          char **err_body, uint32_t *err_len) {
    void  *source = NULL;
    size_t source_len = 0;
    char *module_path = sjs_resolve_with_extensions(
        sjs->preprocessors, sjs->kv, base_path,
        kv_prefix, &source, &source_len);

    if (!module_path) return -1;   /* not found */

    /* Record module load for replay capture */
    if (sjs->current_replay_capture) {
        const char *hash = sha256_hex(source, source_len);
        replay_capture_module(sjs->current_replay_capture,
                              module_path, hash);
    }

    const char *ext = sjs_path_extension(module_path);
    const sjs_preprocessor_entry_t *pp = ext
        ? sjs_preprocessor_find(sjs->preprocessors, ext) : NULL;

    char  *compile_source = source;
    size_t compile_len    = source_len;

    if (pp) {
        /* Set module path for source map generation */
        if (pp->transform == sjs_typescript_transform) {
            sjs_ts_binding_t *binding = pp->user_data;
            if (binding && binding->ts_ctx)
                binding->ts_ctx->current_module_path = module_path;
        }

        size_t js_len;
        char *js = pp->transform(source, source_len, &js_len, pp->user_data);
        free(source);
        if (!js) {
            free(module_path);
            *err_body = strdup("Preprocessor Error");
            *err_len = (uint32_t)strlen(*err_body);
            return 1;
        }
        compile_source = js;
        compile_len    = js_len;

        /* Store source map in KV if the TypeScript preprocessor produced one */
        if (pp->transform == sjs_typescript_transform) {
            sjs_ts_binding_t *binding = pp->user_data;
            if (binding && binding->last_source_map) {
                char sm_key[512];
                snprintf(sm_key, sizeof(sm_key),
                         "__code_sourcemap/%s", module_path);
                char pk[512];
                const char *key = kv_prefixed_key(kv_prefix, sm_key,
                                                   pk, sizeof(pk));
                if (key)
                    kv_put(sjs->kv, key, binding->last_source_map,
                           binding->last_source_map_len);

                if (sjs->current_replay_capture)
                    replay_capture_sourcemap(sjs->current_replay_capture,
                                             module_path,
                                             binding->last_source_map);

                free(binding->last_source_map);
                binding->last_source_map = NULL;
                binding->last_source_map_len = 0;
            }
        }
    }

    sjs->current_prefix = kv_prefix;
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
        *err_len = (uint32_t)strlen(*err_body);
        return 1;
    }

    size_t out_bc_len;
    uint8_t *bc = JS_WriteObject(cc, &out_bc_len, module_val,
                                 JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(cc, module_val);

    if (!bc) {
        free(module_path);
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
                                 sjs_resp_headers_t *resp_hdrs,
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
        if (!resp_has_header(resp_hdrs, "content-type"))
            resp_add_header(resp_hdrs, "content-type", "application/json");
    }
    return body;
}

/* ---- route resolution ---- */

/* Resolve route and load/compile bytecode. Populates route and bc components.
 * Returns 0 on success, or an HTTP status code on error (with err_body set). */
static int sjs_resolve_request_route(sjs_runtime_t *sjs, const char *path,
                              const char *kv_prefix,
                              sjs_route_info_t *route, sjs_bytecode_t *bc,
                              char **err_body, uint32_t *err_len) {
    sjs_route_t parsed;
    sjs_resolve_route(path, &parsed);

    char *base_path = parsed.module_path;
    route->query_string = parsed.query_string;
    parsed.module_path = NULL;
    parsed.query_string = NULL;
    sjs_route_free(&parsed);

    if (!base_path) {
        *err_body = strdup("Internal Server Error");
        *err_len = (uint32_t)strlen(*err_body);
        return 500;
    }

    /* Build cache key */
    char raw_cache[256];
    snprintf(raw_cache, sizeof(raw_cache), "__compiled/%s", base_path);
    char cache_key_buf[512];
    const char *ck = sjs_prefixed_key(kv_prefix, raw_cache,
                                       cache_key_buf, sizeof(cache_key_buf));
    if (!ck) {
        free(base_path);
        *err_body = strdup("Key Too Long");
        *err_len = (uint32_t)strlen(*err_body);
        return 500;
    }

    /* Try cache hit */
    if (kv_get(sjs->kv, ck, &bc->data, &bc->len) == 0) {
        /* Record module in replay capture even on cache hit.
         * Try each extension to find the __code_meta/ entry. */
        if (sjs->current_replay_capture) {
            static const char *exts[] = { ".mjs", ".ejs", ".ts", ".tsx" };
            for (int ei = 0; ei < 4; ei++) {
                char mod_name[256];
                snprintf(mod_name, sizeof(mod_name), "%s%s", base_path, exts[ei]);
                char meta_raw[256];
                snprintf(meta_raw, sizeof(meta_raw), "__code_meta/%s", mod_name);
                char meta_buf[512];
                const char *meta_key = sjs_prefixed_key(kv_prefix, meta_raw,
                                                         meta_buf, sizeof(meta_buf));
                if (!meta_key) continue;
                void *hash_val = NULL;
                size_t hash_len = 0;
                if (kv_get(sjs->kv, meta_key, &hash_val, &hash_len) == 0 && hash_val) {
                    char hash_str[65] = {0};
                    if (hash_len >= 64) memcpy(hash_str, hash_val, 64);
                    replay_capture_module(sjs->current_replay_capture,
                                          mod_name, hash_str);
                    free(hash_val);
                    break;
                }
            }
        }
        goto found;
    }

    /* Cache miss — compile primary route */
    {
        char *comp_err = NULL;
        uint32_t comp_err_len = 0;
        int rc = compile_module(sjs, kv_prefix, base_path, ck,
                                &bc->data, &bc->len, &route->module_path,
                                &comp_err, &comp_err_len);
        if (rc == 0) goto found;
        if (rc > 0) {
            free(base_path);
            *err_body = comp_err;
            *err_len = comp_err_len;
            return 500;
        }
    }

    /* Module not found */
    free(base_path);
    *err_body = strdup("Not Found");
    *err_len = (uint32_t)strlen(*err_body);
    return 404;

found:
    if (!route->module_path) route->module_path = strdup(base_path);
    free(base_path);
    return 0;
}

/* ---- session loading ---- */

static void sjs_session_load(sjs_runtime_t *sjs, sjs_request_ctx_t *req,
                              JSContext *ctx) {
    sjs_session_t *sess = req->session;
    sess->is_dirty = false;

    /* Extract session ID from Cookie header */
    if (!sess->id) {
        for (uint32_t i = 0; i < req->header_count; i++) {
            if (req->headers[i].name_len == 6 &&
                memcmp(req->headers[i].name, "cookie", 6) == 0) {
                sess->id = sjs_session_parse_cookie(
                    req->headers[i].value, req->headers[i].value_len);
                break;
            }
        }
        if (!sess->id) {
            char id_buf[SJS_SESSION_ID_LEN + 1];
            if (sjs_session_generate_id(id_buf, req))
                sess->id = strdup(id_buf);
            sess->is_new = true;
        }
    }

    /* Load session data from KV into session.__data */
    if (sess->id && !sess->is_new) {
        char sess_key[80];
        snprintf(sess_key, sizeof(sess_key), "sessions/%s", sess->id);

        char pfx_buf[512];
        const char *actual_key = sjs_prefixed_key(req->kv_prefix, sess_key,
                                                   pfx_buf, sizeof(pfx_buf));
        if (actual_key) {
            void  *sdata = NULL;
            size_t sdata_len = 0;
            if (kv_get(sjs->kv, actual_key, &sdata, &sdata_len) == 0 && sdata) {
                /* Capture session data for replay */
                if (req->replay_capture)
                    replay_capture_session(req->replay_capture, sdata, sdata_len);

                JSValue global = JS_GetGlobalObject(ctx);
                JSValue json_obj = JS_GetPropertyStr(ctx, global, "JSON");
                JSValue parse_fn = JS_GetPropertyStr(ctx, json_obj, "parse");
                JSValue json_str = JS_NewStringLen(ctx, sdata, sdata_len);
                JSValue parsed = JS_Call(ctx, parse_fn, json_obj, 1, &json_str);

                if (!JS_IsException(parsed)) {
                    JSValue s = JS_GetPropertyStr(ctx, global, "session");
                    JS_SetPropertyStr(ctx, s, "__data", parsed);
                    JS_FreeValue(ctx, s);
                } else {
                    JS_GetException(ctx); /* clear */
                }

                JS_FreeValue(ctx, global);
                free(sdata);
            }
        }
    }
}

/* ---- session persistence ---- */

static void sjs_session_persist(sjs_runtime_t *sjs, sjs_request_ctx_t *req,
                                 JSContext *ctx) {
    sjs_session_t *sess = req->session;
    if (!sess->id || (!sess->is_dirty && !sess->is_new))
        return;

    if (sess->is_dirty) {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue s = JS_GetPropertyStr(ctx, global, "session");
        JSValue data = JS_GetPropertyStr(ctx, s, "__data");
        JSValue json_obj = JS_GetPropertyStr(ctx, global, "JSON");
        JSValue stringify = JS_GetPropertyStr(ctx, json_obj, "stringify");
        JSValue json_val = JS_Call(ctx, stringify, json_obj, 1, &data);

        if (JS_IsString(json_val)) {
            size_t jlen;
            const char *jstr = JS_ToCStringLen(ctx, &jlen, json_val);
            if (jstr) {
                char sess_key[80];
                snprintf(sess_key, sizeof(sess_key), "sessions/%s", sess->id);
                char pfx_buf[512];
                const char *actual_key = sjs_prefixed_key(
                    req->kv_prefix, sess_key, pfx_buf, sizeof(pfx_buf));
                if (actual_key)
                    kv_put(sjs->kv, actual_key, jstr, jlen);
            }
        }

        JS_FreeValue(ctx, global);
    }

    if (sess->is_new) {
        char *cookie = sjs_session_cookie_header(sess->id);
        if (cookie) {
            resp_add_header(req->resp_hdrs, "set-cookie", cookie);
            free(cookie);
        }
    }
}

/* ---- JSONP wrapping ---- */

static void sjs_jsonp_wrap(char **body, uint32_t *body_len,
                            const char *query_str, const char *method,
                            sjs_resp_headers_t *resp_hdrs) {
    if (!*body || (strcmp(method, "GET") && strcmp(method, "HEAD")))
        return;
    if (!query_str)
        return;

    /* Quick scan for callback= param */
    const char *cb = NULL;
    char *cb_buf = NULL;
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

    if (!cb || !cb[0]) { free(cb_buf); return; }

    /* Validate callback name (alphanumeric + _ + . only) */
    bool valid = true;
    for (const char *c = cb; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || *c == '_' || *c == '.')) {
            valid = false;
            break;
        }
    }

    if (valid) {
        size_t cb_len = strlen(cb);
        size_t blen = *body_len;
        size_t wrapped_len = cb_len + 1 + blen + 2;
        char *wrapped = malloc(wrapped_len);
        if (wrapped) {
            memcpy(wrapped, cb, cb_len);
            wrapped[cb_len] = '(';
            memcpy(wrapped + cb_len + 1, *body, blen);
            wrapped[cb_len + 1 + blen] = ')';
            wrapped[cb_len + 1 + blen + 1] = ';';
            free(*body);
            *body = wrapped;
            *body_len = (uint32_t)wrapped_len;

            /* Override content-type for JSONP */
            for (uint32_t i = 0; i < resp_hdrs->count; i++) {
                if (!strcasecmp(resp_hdrs->names[i], "content-type")) {
                    free(resp_hdrs->values[i]);
                    resp_hdrs->values[i] = strdup("application/javascript");
                    break;
                }
            }
        }
    }
    free(cb_buf);
}

/* ---- main dispatch ---- */

char *sjs_dispatch(sjs_runtime_t *sjs, sjs_request_ctx_t *req,
                   sjs_route_info_t *route, sjs_bytecode_t *bc,
                   uint32_t *out_len) {
    /* Set replay capture on runtime for module loader access */
    sjs->current_replay_capture = req->replay_capture;

    /* ---- Phase 1: Route resolution and bytecode loading ---- */
    {
        char *err_body = NULL;
        uint32_t err_len = 0;
        int status = sjs_resolve_request_route(sjs, req->path, req->kv_prefix,
                                        route, bc, &err_body, &err_len);
        if (status != 0) {
            req->resp_st->code = (uint16_t)status;
            *out_len = err_len;
            return err_body;
        }
    }

    /* Cache key for bytecode reload on retry */
    char raw_cache[256];
    snprintf(raw_cache, sizeof(raw_cache), "__compiled/%s",
             route->module_path);
    char cache_key_buf[512];
    const char *ck = sjs_prefixed_key(req->kv_prefix, raw_cache,
                                       cache_key_buf, sizeof(cache_key_buf));

    /* ---- Phases 2-4 with transaction retry on conflict ---- */
    #define MAX_TXN_RETRIES 3

    char *err_msg = NULL;

    for (int attempt = 0; attempt <= MAX_TXN_RETRIES; attempt++) {

        /* Reset mutable state on retry */
        if (attempt > 0) {
            sjs_resp_headers_reset(req->resp_hdrs);
            req->resp_st->code = 200;

            /* Clear accumulated log batch from previous attempt */
            if (req->log_batch) {
                for (uint32_t li = 0; li < req->log_batch->count; li++)
                    free((void *)req->log_batch->entries[li].msg);
                req->log_batch->count = 0;
            }

            /* Clear accumulated Raft write-set from previous attempt */
            if (req->write_set) {
                raft_write_set_free(req->write_set);
                raft_write_set_init(req->write_set);
                req->raft_seq = 0;
            }

            free(bc->data);
            bc->data = NULL;
            if (!ck || kv_get(sjs->kv, ck, &bc->data, &bc->len) != 0) {
                req->resp_st->code = 500;
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
            req->resp_st->code = 500;
            char *body = strdup("Internal Server Error");
            *out_len = (uint32_t)strlen(body);
            return body;
        }

        JS_SetContextOpaque(ctx, req);
        JS_SetModuleLoaderFunc(rt, NULL, sjs_request_module_loader, NULL);

        /* Phase 3: Begin transaction, load bytecode, evaluate module */
        if (kv_begin(sjs->kv) == KV_CONFLICT) {
            arena_reset(sjs);
            continue;
        }

        /* Load session */
        sjs_session_load(sjs, req, ctx);

        /* Reset random tape — session ID generation consumed bytes that
         * are not part of the JS-visible random sequence. The tape should
         * only contain bytes from JS crypto.getRandomValues calls. */
        if (req->tape) {
            req->tape->len = 0;
            req->tape->pos = 0;
        }

        JSValue module_val = JS_ReadObject(ctx, bc->data, bc->len,
                                           JS_READ_OBJ_BYTECODE);
        /* Bytecode was consumed; clear so destructor doesn't double-free
         * (we'll reload from KV on retry) */
        free(bc->data);
        bc->data = NULL;

        if (JS_IsException(module_val)) {
            const char *err = js_err_string(ctx, arena);
            fprintf(stderr, "shift-js: bytecode load error in %s: %s\n",
                    route->module_path, err);
            char *body;
            asprintf(&body, "bytecode load error in %s: %s",
                     route->module_path, err);
            kv_rollback(sjs->kv);
            arena_reset(sjs);
            req->resp_st->code = 500;
            *out_len = (uint32_t)strlen(body);
            return body;
        }

        JSModuleDef *mod_def = JS_VALUE_GET_PTR(module_val);

        JSValue result = JS_EvalFunction(ctx, module_val);
        if (JS_IsException(result)) {
            const char *err = js_err_string(ctx, arena);
            fprintf(stderr, "shift-js: module eval error in %s: %s\n",
                    route->module_path, err);
            asprintf(&err_msg, "module eval error in %s: %s",
                     route->module_path, err);
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
                    route->module_path, err);
            asprintf(&err_msg, "module rejected in %s: %s",
                     route->module_path, err);
            goto txn_fail;
        }

        /* ---- Dispatch: check for default export function ---- */
        JSValue ns = JS_GetModuleNamespace(ctx, mod_def);
        JSValue default_fn = JS_GetPropertyStr(ctx, ns, "default");
        bool is_default_fn = JS_IsFunction(ctx, default_fn);

        JSValue ret;
        const char *called_func = NULL;

        if (is_default_fn) {
            called_func = "default";
            if (!resp_has_header(req->resp_hdrs, "content-type"))
                resp_add_header(req->resp_hdrs, "content-type", "text/html");

            ret = JS_Call(ctx, default_fn, JS_UNDEFINED, 0, NULL);
        } else {
            /* MJS dispatch: fn required from query (GET) or body (POST) */
            const char *fn_name = NULL;
            JSValue args = JS_UNDEFINED;
            bool is_get = !strcmp(req->method, "GET") || !strcmp(req->method, "HEAD");
            bool is_post = !strcmp(req->method, "POST");

            if (!is_get && !is_post) {
                kv_rollback(sjs->kv);
                arena_reset(sjs);
                req->resp_st->code = 405;
                char *body = strdup("Method Not Allowed");
                *out_len = (uint32_t)strlen(body);
                return body;
            }

            if (is_get) {
                /* Extract fn from query string, remaining params become args */
                JSValue qs_obj = parse_query_string(ctx, route->query_string);
                JSValue fn_val = JS_GetPropertyStr(ctx, qs_obj, "fn");
                if (JS_IsString(fn_val)) {
                    fn_name = JS_ToCString(ctx, fn_val);
                }
                JS_FreeValue(ctx, fn_val);

                if (!fn_name || !fn_name[0]) {
                    if (fn_name) JS_FreeCString(ctx, fn_name);
                    kv_rollback(sjs->kv);
                    arena_reset(sjs);
                    req->resp_st->code = 400;
                    char *body = strdup("\"fn\" query parameter is required");
                    *out_len = (uint32_t)strlen(body);
                    return body;
                }

                /* Remove fn from the args object */
                JS_DeleteProperty(ctx, qs_obj,
                    JS_NewAtom(ctx, "fn"), 0);
                args = qs_obj;
            } else {
                /* POST: parse JSON body, extract fn and args */
                JSValue body_obj = JS_UNDEFINED;
                if (req->body && req->body_len > 0) {
                    JSValue global = JS_GetGlobalObject(ctx);
                    JSValue json_obj = JS_GetPropertyStr(ctx, global, "JSON");
                    JSValue parse_fn_val = JS_GetPropertyStr(ctx, json_obj, "parse");
                    JSValue body_str = JS_NewStringLen(ctx, req->body, req->body_len);
                    body_obj = JS_Call(ctx, parse_fn_val, json_obj, 1, &body_str);
                    if (JS_IsException(body_obj)) {
                        JS_GetException(ctx);
                        body_obj = JS_UNDEFINED;
                    }
                }

                if (JS_IsObject(body_obj)) {
                    JSValue fn_val = JS_GetPropertyStr(ctx, body_obj, "fn");
                    if (JS_IsString(fn_val))
                        fn_name = JS_ToCString(ctx, fn_val);
                    JS_FreeValue(ctx, fn_val);

                    JSValue args_val = JS_GetPropertyStr(ctx, body_obj, "args");
                    args = JS_IsObject(args_val) ? args_val : JS_NewObject(ctx);
                } else {
                    JS_FreeValue(ctx, body_obj);
                }

                if (!fn_name || !fn_name[0]) {
                    if (fn_name) JS_FreeCString(ctx, fn_name);
                    kv_rollback(sjs->kv);
                    arena_reset(sjs);
                    req->resp_st->code = 400;
                    char *body = strdup("\"fn\" field is required in request body");
                    *out_len = (uint32_t)strlen(body);
                    return body;
                }
            }

            called_func = fn_name;

            JSValue handler = JS_GetPropertyStr(ctx, ns, fn_name);
            if (!JS_IsFunction(ctx, handler)) {
                kv_rollback(sjs->kv);
                arena_reset(sjs);
                req->resp_st->code = 404;
                char *body;
                asprintf(&body, "function \"%s\" not found", fn_name);
                JS_FreeCString(ctx, fn_name);
                *out_len = (uint32_t)strlen(body);
                return body;
            }

            ret = JS_Call(ctx, handler, JS_UNDEFINED, 1, &args);
            JS_FreeCString(ctx, fn_name);
        }

        /* Capture exception immediately */
        if (JS_IsException(ret)) {
            const char *err = js_err_string(ctx, arena);
            fprintf(stderr, "shift-js: handler error in %s.%s: %s\n",
                    route->module_path, called_func, err);
            asprintf(&err_msg, "handler error in %s.%s: %s",
                     route->module_path, called_func, err);
            goto txn_fail;
        }

        /* Pump microtasks after handler call */
        {
            JSContext *pctx;
            while (JS_IsJobPending(rt))
                JS_ExecutePendingJob(rt, &pctx);
        }

        /* Unwrap if handler returned a promise (async function) */
        if (JS_PromiseState(ctx, ret) == JS_PROMISE_FULFILLED) {
            JSValue resolved = JS_PromiseResult(ctx, ret);
            ret = JS_DupValue(ctx, resolved);
        } else if (JS_PromiseState(ctx, ret) == JS_PROMISE_REJECTED) {
            JSValue reason = JS_PromiseResult(ctx, ret);
            const char *err = JS_ToCString(ctx, reason);
            fprintf(stderr, "shift-js: async handler rejected in %s.%s: %s\n",
                    route->module_path, called_func, err ? err : "(unknown)");
            asprintf(&err_msg, "async handler rejected in %s.%s: %s",
                     route->module_path, called_func, err ? err : "(unknown)");
            goto txn_fail;
        }

        /* Phase 4: Extract response body to libc heap */
        char *body = extract_body_string(ctx, arena, req->resp_hdrs, ret,
                                         route->module_path, called_func,
                                         out_len, &err_msg);
        if (err_msg) goto txn_fail;

        /* Auto-prepend <!DOCTYPE html> when default export returns <html>... */
        if (is_default_fn && body && *out_len >= 5 &&
            strncasecmp(body, "<html", 5) == 0) {
            static const char DOCTYPE[] = "<!DOCTYPE html>\n";
            size_t dt_len = sizeof(DOCTYPE) - 1;
            char *new_body = malloc(dt_len + *out_len);
            if (new_body) {
                memcpy(new_body, DOCTYPE, dt_len);
                memcpy(new_body + dt_len, body, *out_len);
                free(body);
                body = new_body;
                *out_len += (uint32_t)dt_len;
            }
        }

        /* JSONP wrapping for API mode GET requests */
        if (!is_default_fn)
            sjs_jsonp_wrap(&body, out_len, route->query_string,
                           req->method, req->resp_hdrs);

        /* Session persistence */
        sjs_session_persist(sjs, req, ctx);

        /* Transfer the seq assigned during kv_put_seq to the write-set.
         * The seq was allocated inside this transaction via kv_next_seq
         * on the first kv.put call, so it's WAL-ordered. */
        if (req->write_set && req->write_set->op_count > 0)
            req->write_set->seq = req->raft_seq;

        /* Auto-inject x-request-id header */
        {
            char rid_buf[24];
            snprintf(rid_buf, sizeof(rid_buf), "%" PRIu64, req->request_id);
            resp_add_header(req->resp_hdrs, "x-request-id", rid_buf);
        }

        /* Commit — retry on conflict */
        if (kv_commit(sjs->kv) == KV_CONFLICT) {
            kv_rollback(sjs->kv);
            free(body);
            arena_reset(sjs);
            continue;
        }

        /* Flush pending log entries to log DB (before arena reset,
         * though msg strings are strdup'd so arena-independent). */
        if (req->log_batch && req->log_batch->count > 0 && req->log_db) {
            log_db_flush(req->log_db, (int)(req->request_id >> 48),
                         req->request_id,
                         req->session ? req->session->id : NULL,
                         req->log_batch);
            /* Free strdup'd messages */
            for (uint32_t i = 0; i < req->log_batch->count; i++)
                free((void *)req->log_batch->entries[i].msg);
            req->log_batch->count = 0;
        }

        /* Flush replay capture to log DB */
        if (req->replay_capture && req->log_db) {
            sjs_replay_capture_t *cap = req->replay_capture;

            /* Finalize JSON array buffers */
            replay_capture_finalize(&cap->kv_tape);
            replay_capture_finalize(&cap->date_tape);
            replay_capture_finalize(&cap->math_random_tape);
            replay_capture_finalize(&cap->module_tree);
            replay_capture_finalize(&cap->source_maps);

            /* Build request data JSON via QuickJS JSON.stringify */
            char *req_json = NULL;
            {
                JSValue obj = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, obj, "method",
                    JS_NewString(ctx, req->method));
                JS_SetPropertyStr(ctx, obj, "path",
                    JS_NewString(ctx, req->path));
                if (req->body && req->body_len > 0)
                    JS_SetPropertyStr(ctx, obj, "body",
                        JS_NewStringLen(ctx, req->body, req->body_len));
                else
                    JS_SetPropertyStr(ctx, obj, "body", JS_NULL);

                /* Headers */
                JSValue hdrs = JS_NewObject(ctx);
                for (uint32_t h = 0; h < req->header_count; h++) {
                    char name[256];
                    size_t nlen = req->headers[h].name_len;
                    if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
                    memcpy(name, req->headers[h].name, nlen);
                    name[nlen] = '\0';
                    JS_SetPropertyStr(ctx, hdrs, name,
                        JS_NewStringLen(ctx, req->headers[h].value,
                                        req->headers[h].value_len));
                }
                JS_SetPropertyStr(ctx, obj, "headers", hdrs);

                /* Session */
                if (cap->session_json) {
                    JSValue global2 = JS_GetGlobalObject(ctx);
                    JSValue json2 = JS_GetPropertyStr(ctx, global2, "JSON");
                    JSValue parse2 = JS_GetPropertyStr(ctx, json2, "parse");
                    JSValue sjs2 = JS_NewString(ctx, cap->session_json);
                    JSValue parsed = JS_Call(ctx, parse2, json2, 1, &sjs2);
                    JS_SetPropertyStr(ctx, obj, "session",
                        JS_IsException(parsed) ? JS_NULL : parsed);
                } else {
                    JS_SetPropertyStr(ctx, obj, "session", JS_NULL);
                }

                JSValue global2 = JS_GetGlobalObject(ctx);
                JSValue json2 = JS_GetPropertyStr(ctx, global2, "JSON");
                JSValue stringify2 = JS_GetPropertyStr(ctx, json2, "stringify");
                JSValue result = JS_Call(ctx, stringify2, json2, 1, &obj);
                if (JS_IsString(result)) {
                    const char *s = JS_ToCString(ctx, result);
                    req_json = strdup(s);
                    JS_FreeCString(ctx, s);
                }
            }

            /* Build response data JSON */
            char *resp_json = NULL;
            {
                size_t rsize = 128 + (body ? *out_len * 2 : 0);
                resp_json = malloc(rsize);
                snprintf(resp_json, rsize,
                    "{\"status\":%d}", req->resp_st->code);
            }

            log_db_flush_replay(req->log_db, req->request_id,
                req_json, resp_json,
                cap->kv_tape.data,
                req->tape ? req->tape->data : NULL,
                req->tape ? req->tape->len : 0,
                cap->date_tape.data,
                cap->math_random_tape.data,
                cap->module_tree.data,
                cap->source_maps.data);

            free(req_json);
            free(resp_json);
        }

        sjs->current_replay_capture = NULL;
        arena_reset(sjs);

        if (!body) {
            body = strdup("");
            *out_len = 0;
        }
        return body;

    txn_fail:
        kv_rollback(sjs->kv);
        /* Free strdup'd log messages on failure */
        if (req->log_batch) {
            for (uint32_t li = 0; li < req->log_batch->count; li++)
                free((void *)req->log_batch->entries[li].msg);
            req->log_batch->count = 0;
        }
        arena_reset(sjs);
        req->resp_st->code = 500;
        {
            char *body = err_msg ? err_msg : strdup("Internal Server Error");
            *out_len = (uint32_t)strlen(body);
            return body;
        }
    }

    /* All retries exhausted */
    req->resp_st->code = 409;
    char *body = strdup("Transaction Conflict");
    *out_len = (uint32_t)strlen(body);
    return body;
}
