#define _GNU_SOURCE
#include "js_globals.h"
#include "js_runtime.h"
#include "crypto.h"
#include "kvstore.h"
#include "log_db.h"
#include "replay_capture.h"
#include "raft_thread.h"

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
 * JS error extraction — falls back to arena stats when OOM prevents
 * QuickJS from allocating an exception object.
 * ====================================================================== */

const char *js_err_string(JSContext *ctx, sjs_arena_t *arena) {
    static _Thread_local char err_buf[2048];

    JSValue exc = JS_GetException(ctx);
    if (JS_IsNull(exc) || JS_IsUndefined(exc)) {
        snprintf(err_buf, sizeof(err_buf),
                 "arena exhausted (%zu / %d bytes used)",
                 arena->used, SJS_ARENA_SIZE);
        return err_buf;
    }

    const char *msg = JS_ToCString(ctx, exc);
    snprintf(err_buf, sizeof(err_buf), "%s", msg ? msg : "(unknown error)");
    return err_buf;
}

const char *js_err_string_with_stack(JSContext *ctx) {
    static _Thread_local char err_buf[4096];

    JSValue exc = JS_GetException(ctx);
    if (JS_IsNull(exc) || JS_IsUndefined(exc)) {
        snprintf(err_buf, sizeof(err_buf), "(unknown error)");
        return err_buf;
    }

    const char *msg = JS_ToCString(ctx, exc);
    if (!msg) msg = "(unknown error)";

    JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
    if (JS_IsString(stack)) {
        const char *stack_str = JS_ToCString(ctx, stack);
        if (stack_str && stack_str[0]) {
            snprintf(err_buf, sizeof(err_buf), "%s\n%s", msg, stack_str);
            JS_FreeCString(ctx, stack_str);
            JS_FreeValue(ctx, stack);
            JS_FreeCString(ctx, msg);
            JS_FreeValue(ctx, exc);
            return err_buf;
        }
        if (stack_str) JS_FreeCString(ctx, stack_str);
    }
    if (!JS_IsException(stack))
        JS_FreeValue(ctx, stack);

    snprintf(err_buf, sizeof(err_buf), "%s", msg);
    JS_FreeCString(ctx, msg);
    JS_FreeValue(ctx, exc);
    return err_buf;
}

/* ======================================================================
 * kv global
 * ====================================================================== */

static kvstore_t *js_get_kv(JSContext *ctx) {
    sjs_runtime_t *sjs = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    return sjs ? sjs->kv : NULL;
}

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
    if (rc < 0)   return JS_ThrowInternalError(ctx, "kv get failed");

    /* Capture KV read for replay */
    sjs_request_ctx_t *req = JS_GetContextOpaque(ctx);
    if (req && req->replay_capture)
        replay_capture_kv_get(req->replay_capture, actual_key, value, vlen);

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

    size_t vlen;
    const char *value = JS_ToCStringLen(ctx, &vlen, argv[1]);
    if (!value) { JS_FreeCString(ctx, key); return JS_EXCEPTION; }

    char pfx_buf[512];
    const char *actual_key = sjs_prefixed_key(js_get_prefix(ctx), key,
                                               pfx_buf, sizeof(pfx_buf));
    if (!actual_key) {
        JS_FreeCString(ctx, key);
        JS_FreeCString(ctx, value);
        return JS_ThrowRangeError(ctx, "kv key too long");
    }

    sjs_request_ctx_t *req = JS_GetContextOpaque(ctx);
    int rc;
    if (req && req->write_set) {
        if (req->raft_seq == 0)
            req->raft_seq = kv_next_seq(kv);
        rc = kv_put_seq(kv, actual_key, value, vlen, req->raft_seq);
        if (rc == 0)
            raft_write_set_add_put(req->write_set, actual_key, value, vlen);
    } else {
        rc = kv_put(kv, actual_key, value, vlen);
    }

    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, value);
    return rc == 0 ? JS_UNDEFINED : JS_ThrowInternalError(ctx, "kv put failed");
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

    sjs_request_ctx_t *req = JS_GetContextOpaque(ctx);
    if (req && req->write_set)
        raft_write_set_add_delete(req->write_set, actual_key);

    int rc = kv_delete(kv, actual_key);
    JS_FreeCString(ctx, key);
    return rc == 0 ? JS_UNDEFINED : JS_ThrowInternalError(ctx, "kv delete failed");
}

static JSValue js_kv_range(JSContext *ctx, JSValue this_val,
                           int argc, JSValue *argv) {
    kvstore_t *kv = js_get_kv(ctx);
    if (!kv) return JS_ThrowInternalError(ctx, "kv store not available");

    const char *start = JS_ToCString(ctx, argv[0]);
    if (!start) return JS_EXCEPTION;
    const char *end = JS_ToCString(ctx, argv[1]);
    if (!end) { JS_FreeCString(ctx, start); return JS_EXCEPTION; }

    int count = -1;
    if (argc >= 3 && JS_IsNumber(argv[2]))
        JS_ToInt32(ctx, &count, argv[2]);

    char pfx_start[512], pfx_end[512];
    const char *actual_start = sjs_prefixed_key(js_get_prefix(ctx), start,
                                                 pfx_start, sizeof(pfx_start));
    const char *actual_end   = sjs_prefixed_key(js_get_prefix(ctx), end,
                                                 pfx_end, sizeof(pfx_end));
    JS_FreeCString(ctx, start);
    JS_FreeCString(ctx, end);
    if (!actual_start || !actual_end)
        return JS_ThrowRangeError(ctx, "kv range key too long");

    kv_range_result_t results = {0};
    int rc2 = kv_range(kv, actual_start, actual_end, count, &results);
    if (rc2 < 0) return JS_ThrowInternalError(ctx, "kv range failed");

    /* Strip prefix from keys in the result */
    const char *prefix = js_get_prefix(ctx);
    size_t prefix_len = prefix ? strlen(prefix) : 0;

    /* Capture KV range for replay */
    sjs_request_ctx_t *req = JS_GetContextOpaque(ctx);
    if (req && req->replay_capture)
        replay_capture_kv_range(req->replay_capture,
                                 actual_start, actual_end,
                                 &results, prefix_len);

    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < results.count; i++) {
        JSValue entry = JS_NewObject(ctx);
        const char *display_key = results.entries[i].key;
        if (prefix_len > 0 && strncmp(display_key, prefix, prefix_len) == 0)
            display_key += prefix_len;
        JS_SetPropertyStr(ctx, entry, "key",
                          JS_NewString(ctx, display_key));
        JS_SetPropertyStr(ctx, entry, "value",
                          JS_NewStringLen(ctx, results.entries[i].value,
                                          results.entries[i].value_len));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, entry);
    }
    kv_range_free(&results);
    return arr;
}

void js_install_kv(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue kv = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, kv, "get",
                      JS_NewCFunction(ctx, js_kv_get, "get", 1));
    JS_SetPropertyStr(ctx, kv, "put",
                      JS_NewCFunction(ctx, js_kv_put, "put", 2));
    JS_SetPropertyStr(ctx, kv, "delete",
                      JS_NewCFunction(ctx, js_kv_delete, "delete", 1));
    JS_SetPropertyStr(ctx, kv, "range",
                      JS_NewCFunction(ctx, js_kv_range, "range", 3));
    JS_SetPropertyStr(ctx, global, "kv", kv);
    JS_FreeValue(ctx, global);
}

/* ======================================================================
 * code global (module source management)
 * ====================================================================== */

const char *sha256_hex(const void *data, size_t len) {
    static _Thread_local char hex[65];
    unsigned char hash[32];
    EVP_Digest(data, len, hash, NULL, EVP_sha256(), NULL);
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
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
    if (kv_get(kv, actual_key, &value, &vlen) != 0)
        return JS_NULL;

    JSValue result = JS_NewStringLen(ctx, value, vlen);
    free(value);
    return result;
}

static JSValue js_code_put(JSContext *ctx, JSValue this_val,
                           int argc, JSValue *argv) {
    kvstore_t *kv = js_get_kv(ctx);
    if (!kv) return JS_ThrowInternalError(ctx, "kv store not available");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    size_t vlen;
    const char *value = JS_ToCStringLen(ctx, &vlen, argv[1]);
    if (!value) { JS_FreeCString(ctx, path); return JS_EXCEPTION; }

    const char *prefix = js_get_prefix(ctx);
    sjs_request_ctx_t *req = JS_GetContextOpaque(ctx);

    /* Store source code */
    char raw_key[256];
    snprintf(raw_key, sizeof(raw_key), "__code/%s", path);
    char pfx_buf[512];
    const char *actual_key = sjs_prefixed_key(prefix, raw_key,
                                               pfx_buf, sizeof(pfx_buf));
    if (!actual_key) {
        JS_FreeCString(ctx, path);
        JS_FreeCString(ctx, value);
        return JS_ThrowRangeError(ctx, "code path too long");
    }

    int rc;
    if (req && req->write_set) {
        if (req->raft_seq == 0)
            req->raft_seq = kv_next_seq(kv);
        rc = kv_put_seq(kv, actual_key, value, vlen, req->raft_seq);
        if (rc == 0)
            raft_write_set_add_put(req->write_set, actual_key, value, vlen);
    } else {
        rc = kv_put(kv, actual_key, value, vlen);
    }

    if (rc != 0) {
        JS_FreeCString(ctx, path);
        JS_FreeCString(ctx, value);
        return JS_ThrowInternalError(ctx, "code put failed");
    }

    /* Content-addressed blob */
    const char *hash = sha256_hex(value, vlen);
    {
        char meta_raw[256];
        snprintf(meta_raw, sizeof(meta_raw), "__code_meta/%s", path);
        char mk[512];
        const char *meta_key = kv_prefixed_key(prefix, meta_raw, mk, sizeof(mk));
        if (meta_key) {
            if (req && req->write_set) {
                kv_put_seq(kv, meta_key, hash, 64, req->raft_seq);
                raft_write_set_add_put(req->write_set, meta_key, hash, 64);
            } else {
                kv_put(kv, meta_key, hash, 64);
            }
        }
    }
    {
        char blob_raw[256];
        snprintf(blob_raw, sizeof(blob_raw), "__code_blob/%s", hash);
        char bk[512];
        const char *blob_key = kv_prefixed_key(prefix, blob_raw, bk, sizeof(bk));
        if (blob_key) {
            if (req && req->write_set) {
                kv_put_seq(kv, blob_key, value, vlen, req->raft_seq);
                raft_write_set_add_put(req->write_set, blob_key, value, vlen);
            } else {
                kv_put(kv, blob_key, value, vlen);
            }
        }
    }

    /* Invalidate bytecode cache */
    {
        char compiled_raw[256];
        snprintf(compiled_raw, sizeof(compiled_raw), "__compiled/%s", path);
        char ck[512];
        const char *compiled_key = kv_prefixed_key(prefix, compiled_raw,
                                                     ck, sizeof(ck));
        if (compiled_key) {
            if (req && req->write_set) {
                kv_delete(kv, compiled_key);
                raft_write_set_add_delete(req->write_set, compiled_key);
            } else {
                kv_delete(kv, compiled_key);
            }
        }
    }

    JS_FreeCString(ctx, path);
    JS_FreeCString(ctx, value);
    return JS_UNDEFINED;
}

static JSValue js_code_delete(JSContext *ctx, JSValue this_val,
                              int argc, JSValue *argv) {
    kvstore_t *kv = js_get_kv(ctx);
    if (!kv) return JS_ThrowInternalError(ctx, "kv store not available");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    const char *prefix = js_get_prefix(ctx);
    sjs_request_ctx_t *req = JS_GetContextOpaque(ctx);

    /* Delete source */
    char raw_key[256];
    snprintf(raw_key, sizeof(raw_key), "__code/%s", path);
    char pfx_buf[512];
    const char *actual_key = sjs_prefixed_key(prefix, raw_key,
                                               pfx_buf, sizeof(pfx_buf));
    if (!actual_key) {
        JS_FreeCString(ctx, path);
        return JS_ThrowRangeError(ctx, "code path too long");
    }

    if (req && req->write_set)
        raft_write_set_add_delete(req->write_set, actual_key);
    kv_delete(kv, actual_key);

    /* Delete metadata */
    char meta_raw[256];
    snprintf(meta_raw, sizeof(meta_raw), "__code_meta/%s", path);
    char mk[512];
    const char *meta_key = kv_prefixed_key(prefix, meta_raw, mk, sizeof(mk));
    if (meta_key) {
        if (req && req->write_set)
            raft_write_set_add_delete(req->write_set, meta_key);
        kv_delete(kv, meta_key);
    }

    /* Delete cached bytecode */
    char compiled_raw[256];
    snprintf(compiled_raw, sizeof(compiled_raw), "__compiled/%s", path);
    char ck[512];
    const char *compiled_key = kv_prefixed_key(prefix, compiled_raw, ck, sizeof(ck));
    if (compiled_key) {
        if (req && req->write_set)
            raft_write_set_add_delete(req->write_set, compiled_key);
        kv_delete(kv, compiled_key);
    }

    /* Delete transpiled JS and source map */
    char js_raw[256];
    snprintf(js_raw, sizeof(js_raw), "__code_js/%s", path);
    char jk[512];
    const char *js_key = kv_prefixed_key(prefix, js_raw, jk, sizeof(jk));
    if (js_key) {
        if (req && req->write_set)
            raft_write_set_add_delete(req->write_set, js_key);
        kv_delete(kv, js_key);
    }

    char sm_raw[256];
    snprintf(sm_raw, sizeof(sm_raw), "__code_sourcemap/%s", path);
    char sk[512];
    const char *sm_key = kv_prefixed_key(prefix, sm_raw, sk, sizeof(sk));
    if (sm_key) {
        if (req && req->write_set)
            raft_write_set_add_delete(req->write_set, sm_key);
        kv_delete(kv, sm_key);
    }

    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

static JSValue js_code_list(JSContext *ctx, JSValue this_val,
                            int argc, JSValue *argv) {
    kvstore_t *kv = js_get_kv(ctx);
    if (!kv) return JS_ThrowInternalError(ctx, "kv store not available");

    const char *prefix = js_get_prefix(ctx);

    char start_raw[] = "__code/";
    char end_raw[]   = "__code0";  /* '0' = '/' + 1 */
    char start_buf[512], end_buf[512];
    const char *actual_start = sjs_prefixed_key(prefix, start_raw,
                                                 start_buf, sizeof(start_buf));
    const char *actual_end   = sjs_prefixed_key(prefix, end_raw,
                                                 end_buf, sizeof(end_buf));
    if (!actual_start || !actual_end)
        return JS_ThrowRangeError(ctx, "prefix too long");

    kv_range_result_t results = {0};
    int rc2 = kv_range(kv, actual_start, actual_end, SIZE_MAX, &results);
    if (rc2 < 0) return JS_ThrowInternalError(ctx, "code list failed");

    JSValue arr = JS_NewArray(ctx);
    size_t code_prefix_len = strlen(actual_start);
    uint32_t out_idx = 0;
    for (size_t i = 0; i < results.count; i++) {
        const char *k = results.entries[i].key;
        if (strncmp(k, actual_start, code_prefix_len) != 0) continue;
        if (strstr(k, "__compiled/") || strstr(k, "__code_meta/") ||
            strstr(k, "__code_blob/") || strstr(k, "__code_js/") ||
            strstr(k, "__code_sourcemap/")) continue;

        const char *display = k + code_prefix_len;

        char meta_raw[256];
        snprintf(meta_raw, sizeof(meta_raw), "__code_meta/%s", display);
        char mk[512];
        const char *meta_key = kv_prefixed_key(prefix, meta_raw, mk, sizeof(mk));
        void *hash_val = NULL;
        size_t hash_len = 0;
        if (meta_key)
            kv_get(kv, meta_key, &hash_val, &hash_len);

        JSValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "path",
                          JS_NewString(ctx, display));
        if (hash_val && hash_len >= 64) {
            JS_SetPropertyStr(ctx, entry, "hash",
                              JS_NewStringLen(ctx, hash_val, 64));
        }
        free(hash_val);
        JS_SetPropertyUint32(ctx, arr, out_idx++, entry);
    }
    kv_range_free(&results);
    return arr;
}

void js_install_code(JSContext *ctx) {
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
 * request global (read-only per-request data)
 * ====================================================================== */

static sjs_request_ctx_t *js_get_req_ctx(JSContext *ctx) {
    return JS_GetContextOpaque(ctx);
}

static JSValue js_request_get(JSContext *ctx, JSValue this_val, int magic) {
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (!req) return JS_UNDEFINED;

    switch (magic) {
    case 0: return JS_NewString(ctx, req->method ? req->method : "");
    case 1: return JS_NewString(ctx, req->path ? req->path : "");
    case 2:
        if (req->body && req->body_len > 0)
            return JS_NewStringLen(ctx, req->body, req->body_len);
        return JS_NULL;
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
    case 4: return JS_NewInt64(ctx, (int64_t)req->request_id);
    default: return JS_UNDEFINED;
    }
}

static const JSCFunctionListEntry js_request_props[] = {
    JS_CGETSET_MAGIC_DEF("method",  js_request_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("path",    js_request_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("body",    js_request_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("headers", js_request_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("id",      js_request_get, NULL, 4),
};

void js_install_request(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue req = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, req, js_request_props,
                               sizeof(js_request_props) / sizeof(js_request_props[0]));
    JS_SetPropertyStr(ctx, global, "request", req);
    JS_FreeValue(ctx, global);
}

/* ======================================================================
 * response global (status and header)
 * ====================================================================== */

static JSValue js_response_status(JSContext *ctx, JSValue this_val,
                                   int argc, JSValue *argv) {
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (!req || !req->resp_st) return JS_UNDEFINED;
    int32_t code;
    if (JS_ToInt32(ctx, &code, argv[0])) return JS_EXCEPTION;
    if (code >= 100 && code <= 999)
        req->resp_st->code = (uint16_t)code;
    return JS_UNDEFINED;
}

static JSValue js_response_header(JSContext *ctx, JSValue this_val,
                                   int argc, JSValue *argv) {
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (!req || !req->resp_hdrs) return JS_UNDEFINED;

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    const char *value = JS_ToCString(ctx, argv[1]);
    if (!value) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }

    sjs_resp_headers_t *h = req->resp_hdrs;
    if (h->count == h->cap) {
        uint32_t new_cap = h->cap ? h->cap * 2 : 8;
        char **nn = realloc(h->names, new_cap * sizeof(char *));
        if (!nn) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, value);
            return JS_ThrowInternalError(ctx, "response header allocation failed");
        }
        h->names = nn;
        char **nv = realloc(h->values, new_cap * sizeof(char *));
        if (!nv) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, value);
            return JS_ThrowInternalError(ctx, "response header allocation failed");
        }
        h->values = nv;
        h->cap = new_cap;
    }

    h->names[h->count] = strdup(name);
    h->values[h->count] = strdup(value);
    if (!h->names[h->count] || !h->values[h->count]) {
        free(h->names[h->count]);
        free(h->values[h->count]);
        JS_FreeCString(ctx, name);
        JS_FreeCString(ctx, value);
        return JS_ThrowInternalError(ctx, "response header strdup failed");
    }
    h->count++;

    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, value);
    return JS_UNDEFINED;
}

void js_install_response(JSContext *ctx) {
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

static JSValue js_session_id_get(JSContext *ctx, JSValue this_val, int magic) {
    (void)magic;
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (!req || !req->session || !req->session->id)
        return JS_NULL;
    return JS_NewString(ctx, req->session->id);
}

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

static JSValue js_session_set(JSContext *ctx, JSValue this_val,
                               int argc, JSValue *argv) {
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (req && req->session) req->session->is_dirty = true;

    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    JSValue data = JS_GetPropertyStr(ctx, this_val, "__data");
    JS_SetPropertyStr(ctx, data, key, JS_DupValue(ctx, argv[1]));
    JS_FreeValue(ctx, data);
    JS_FreeCString(ctx, key);
    return JS_UNDEFINED;
}

static JSValue js_session_delete(JSContext *ctx, JSValue this_val,
                                  int argc, JSValue *argv) {
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (req && req->session) req->session->is_dirty = true;

    JSAtom atom = JS_ValueToAtom(ctx, argv[0]);
    if (atom == JS_ATOM_NULL) return JS_EXCEPTION;

    JSValue data = JS_GetPropertyStr(ctx, this_val, "__data");
    JS_DeleteProperty(ctx, data, atom, 0);
    JS_FreeValue(ctx, data);
    JS_FreeAtom(ctx, atom);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_session_props[] = {
    JS_CGETSET_MAGIC_DEF("id", js_session_id_get, NULL, 0),
};

void js_install_session(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue s = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, s, js_session_props,
                               sizeof(js_session_props) / sizeof(js_session_props[0]));
    JS_SetPropertyStr(ctx, s, "get",
                      JS_NewCFunction(ctx, js_session_get, "get", 1));
    JS_SetPropertyStr(ctx, s, "set",
                      JS_NewCFunction(ctx, js_session_set, "set", 2));
    JS_SetPropertyStr(ctx, s, "delete",
                      JS_NewCFunction(ctx, js_session_delete, "delete", 1));
    /* Initialize __data as an empty object */
    JS_SetPropertyStr(ctx, s, "__data", JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, global, "session", s);
    JS_FreeValue(ctx, global);
}

/* ======================================================================
 * console global (logging)
 * ====================================================================== */

static JSValue js_console_log_impl(JSContext *ctx, int argc, JSValue *argv,
                                    int level) {
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (!req || !req->log_batch) return JS_UNDEFINED;

    /* Build message from arguments */
    char buf[4096];
    size_t pos = 0;
    for (int i = 0; i < argc; i++) {
        if (i > 0 && pos < sizeof(buf) - 1) buf[pos++] = ' ';
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            size_t slen = strlen(str);
            size_t room = sizeof(buf) - pos - 1;
            if (slen > room) slen = room;
            memcpy(buf + pos, str, slen);
            pos += slen;
            JS_FreeCString(ctx, str);
        }
    }
    buf[pos] = '\0';

    /* Timestamp */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    /* Append to per-request log batch */
    if (req->log_batch->count < LOG_BATCH_MAX) {
        log_pending_t *e = &req->log_batch->entries[req->log_batch->count++];
        e->level = level;
        e->msg = strndup(buf, pos);
        e->msg_len = (uint32_t)pos;
        e->timestamp_ns = timestamp_ns;
    }

    /* Also write to stderr */
    fprintf(stderr, "[%s] %s\n",
            level == LOG_LEVEL_ERROR ? "ERROR" :
            level == LOG_LEVEL_WARN  ? "WARN"  : "LOG",
            buf);

    return JS_UNDEFINED;
}

static JSValue js_console_log(JSContext *ctx, JSValue this_val,
                               int argc, JSValue *argv) {
    return js_console_log_impl(ctx, argc, argv, LOG_LEVEL_LOG);
}

static JSValue js_console_warn(JSContext *ctx, JSValue this_val,
                                int argc, JSValue *argv) {
    return js_console_log_impl(ctx, argc, argv, LOG_LEVEL_WARN);
}

static JSValue js_console_error(JSContext *ctx, JSValue this_val,
                                 int argc, JSValue *argv) {
    return js_console_log_impl(ctx, argc, argv, LOG_LEVEL_ERROR);
}

void js_install_console(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",
                      JS_NewCFunction(ctx, js_console_log, "log", 0));
    JS_SetPropertyStr(ctx, console, "warn",
                      JS_NewCFunction(ctx, js_console_warn, "warn", 0));
    JS_SetPropertyStr(ctx, console, "error",
                      JS_NewCFunction(ctx, js_console_error, "error", 0));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}

/* ======================================================================
 * logs global (query, replay, requests)
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
    if (strcmp(s, "log") == 0)   return LOG_LEVEL_LOG;
    if (strcmp(s, "warn") == 0)  return LOG_LEVEL_WARN;
    if (strcmp(s, "error") == 0) return LOG_LEVEL_ERROR;
    return -1;
}

/* Temporary struct for collecting log entries across DBs before sorting */
typedef struct {
    int      worker_id;
    int64_t  request_id;
    char    *session_id;   /* strdup'd, may be NULL */
    int      level;
    char    *message;      /* strdup'd */
    double   timestamp;
} log_query_entry_t;

static int log_entry_cmp_desc(const void *a, const void *b) {
    const log_query_entry_t *ea = a, *eb = b;
    if (ea->timestamp > eb->timestamp) return -1;
    if (ea->timestamp < eb->timestamp) return 1;
    return 0;
}

static JSValue js_logs_query(JSContext *ctx, JSValue this_val,
                              int argc, JSValue *argv) {
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (!req || !req->log_reader || !req->log_reader->dbs)
        return JS_NewArray(ctx);

    /* Parse options object */
    int level_filter = -1;
    int limit = 100;
    uint64_t request_id = 0;
    const char *session_id = NULL;
    double before = 0, after = 0;

    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue v;

        v = JS_GetPropertyStr(ctx, argv[0], "level");
        if (JS_IsString(v)) {
            const char *ls = JS_ToCString(ctx, v);
            if (ls) { level_filter = level_from_string(ls); JS_FreeCString(ctx, ls); }
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, argv[0], "limit");
        if (JS_IsNumber(v)) JS_ToInt32(ctx, &limit, v);
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, argv[0], "request_id");
        if (JS_IsNumber(v)) {
            int64_t tmp;
            JS_ToInt64(ctx, &tmp, v);
            request_id = (uint64_t)tmp;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, argv[0], "session_id");
        if (JS_IsString(v)) session_id = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, argv[0], "before");
        if (JS_IsNumber(v)) JS_ToFloat64(ctx, &before, v);
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, argv[0], "after");
        if (JS_IsNumber(v)) JS_ToFloat64(ctx, &after, v);
        JS_FreeValue(ctx, v);
    }

    /* Build query */
    char sql[512];
    size_t pos = 0;
    pos += snprintf(sql + pos, sizeof(sql) - pos,
        "SELECT worker_id, request_id, session_id, level, message, timestamp "
        "FROM logs WHERE 1=1");
    if (level_filter >= 0)
        pos += snprintf(sql + pos, sizeof(sql) - pos, " AND level = %d", level_filter);
    if (request_id > 0)
        pos += snprintf(sql + pos, sizeof(sql) - pos, " AND request_id = %" PRIu64, request_id);
    if (session_id)
        pos += snprintf(sql + pos, sizeof(sql) - pos, " AND session_id = ?");
    if (before > 0)
        pos += snprintf(sql + pos, sizeof(sql) - pos, " AND timestamp < %f", before);
    if (after > 0)
        pos += snprintf(sql + pos, sizeof(sql) - pos, " AND timestamp > %f", after);
    pos += snprintf(sql + pos, sizeof(sql) - pos, " ORDER BY timestamp DESC LIMIT %d", limit);

    /* Query all worker DBs and collect results */
    log_db_reader_t *reader = req->log_reader;
    size_t total = 0, cap = (size_t)limit * (size_t)reader->count;
    log_query_entry_t *entries = malloc(cap * sizeof(*entries));
    if (!entries) {
        if (session_id) JS_FreeCString(ctx, session_id);
        return JS_NewArray(ctx);
    }

    for (int w = 0; w < reader->count; w++) {
        if (!reader->dbs[w]) continue;

        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(reader->dbs[w], sql, -1, &stmt, NULL) != SQLITE_OK)
            continue;

        if (session_id)
            sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);

        while (sqlite3_step(stmt) == SQLITE_ROW && total < cap) {
            log_query_entry_t *e = &entries[total++];
            e->worker_id  = sqlite3_column_int(stmt, 0);
            e->request_id = sqlite3_column_int64(stmt, 1);
            const char *sid = (const char *)sqlite3_column_text(stmt, 2);
            e->session_id = sid ? strdup(sid) : NULL;
            e->level      = sqlite3_column_int(stmt, 3);
            e->message    = strdup((const char *)sqlite3_column_text(stmt, 4));
            e->timestamp  = sqlite3_column_double(stmt, 5);
        }
        sqlite3_finalize(stmt);
    }

    /* Sort merged results by timestamp DESC and apply limit */
    qsort(entries, total, sizeof(*entries), log_entry_cmp_desc);
    if (total > (size_t)limit) total = (size_t)limit;

    /* Build JS array */
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < total; i++) {
        log_query_entry_t *e = &entries[i];
        JSValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "worker_id", JS_NewInt32(ctx, e->worker_id));
        JS_SetPropertyStr(ctx, entry, "request_id", JS_NewInt64(ctx, e->request_id));
        JS_SetPropertyStr(ctx, entry, "session_id",
            e->session_id ? JS_NewString(ctx, e->session_id) : JS_NULL);
        JS_SetPropertyStr(ctx, entry, "level", JS_NewString(ctx, level_name(e->level)));
        JS_SetPropertyStr(ctx, entry, "message", JS_NewString(ctx, e->message));
        JS_SetPropertyStr(ctx, entry, "timestamp", JS_NewFloat64(ctx, e->timestamp));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, entry);
    }

    /* Cleanup */
    for (size_t i = 0; i < total; i++) {
        free(entries[i].session_id);
        free(entries[i].message);
    }
    free(entries);
    if (session_id) JS_FreeCString(ctx, session_id);
    return arr;
}

static JSValue js_logs_replay(JSContext *ctx, JSValue this_val,
                               int argc, JSValue *argv) {
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (!req || !req->log_reader) return JS_NULL;

    int64_t rid;
    if (JS_ToInt64(ctx, &rid, argv[0])) return JS_EXCEPTION;

    char *request_data = NULL, *response_data = NULL,
         *kv_tape = NULL, *date_tape = NULL,
         *math_random_tape = NULL, *module_tree = NULL, *source_maps = NULL;
    uint8_t *random_tape = NULL;
    size_t random_tape_len = 0;

    /* Try each worker DB — request_id is in exactly one */
    log_db_reader_t *reader = req->log_reader;
    int rc = -1;
    for (int w = 0; w < reader->count && rc != 0; w++) {
        if (!reader->dbs[w]) continue;
        log_db_t tmp = { .db = reader->dbs[w] };
        rc = log_db_get_replay(&tmp, (uint64_t)rid,
                               &request_data, &response_data,
                               &kv_tape, &random_tape, &random_tape_len,
                               &date_tape, &math_random_tape,
                               &module_tree, &source_maps);
    }
    if (rc != 0) return JS_NULL;

    JSValue obj = JS_NewObject(ctx);

    /* Parse JSON fields using the inline helper */
    #define PARSE_JSON_FIELD(field, cstr) do { \
        if (cstr) { \
            JSValue s = JS_NewString(ctx, cstr); \
            JSValue parsed = js_json_parse(ctx, s); \
            if (JS_IsException(parsed)) \
                JS_SetPropertyStr(ctx, obj, field, JS_NULL); \
            else \
                JS_SetPropertyStr(ctx, obj, field, parsed); \
            free(cstr); \
        } else { \
            JS_SetPropertyStr(ctx, obj, field, JS_NULL); \
        } \
    } while (0)

    PARSE_JSON_FIELD("request", request_data);
    PARSE_JSON_FIELD("response", response_data);
    PARSE_JSON_FIELD("kv_tape", kv_tape);
    PARSE_JSON_FIELD("date_tape", date_tape);
    PARSE_JSON_FIELD("math_random_tape", math_random_tape);
    PARSE_JSON_FIELD("module_tree", module_tree);
    PARSE_JSON_FIELD("source_maps", source_maps);

    #undef PARSE_JSON_FIELD

    /* Random tape: binary → hex string */
    if (random_tape && random_tape_len > 0) {
        char *hex = malloc(random_tape_len * 2 + 1);
        if (hex) {
            for (size_t i = 0; i < random_tape_len; i++)
                snprintf(hex + i * 2, 3, "%02x", random_tape[i]);
            JS_SetPropertyStr(ctx, obj, "random_tape",
                              JS_NewString(ctx, hex));
            free(hex);
        }
    } else {
        JS_SetPropertyStr(ctx, obj, "random_tape", JS_NULL);
    }
    free(random_tape);

    return obj;
}

static int request_entry_cmp_desc(const void *a, const void *b) {
    const log_db_request_entry_t *ea = a, *eb = b;
    if (ea->request_id > eb->request_id) return -1;
    if (ea->request_id < eb->request_id) return 1;
    return 0;
}

static JSValue js_logs_requests(JSContext *ctx, JSValue this_val,
                                 int argc, JSValue *argv) {
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (!req || !req->log_reader) return JS_NewArray(ctx);

    int limit = 50;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], "limit");
        if (JS_IsNumber(v)) JS_ToInt32(ctx, &limit, v);
        JS_FreeValue(ctx, v);
    }

    /* Query all worker DBs and merge */
    log_db_reader_t *reader = req->log_reader;
    size_t total = 0, cap = (size_t)limit * (size_t)reader->count;
    log_db_request_entry_t *all = malloc(cap * sizeof(*all));
    if (!all) return JS_NewArray(ctx);

    for (int w = 0; w < reader->count; w++) {
        if (!reader->dbs[w]) continue;
        log_db_t tmp = { .db = reader->dbs[w] };
        log_db_request_entry_t *entries = NULL;
        size_t count = 0;
        if (log_db_list_requests(&tmp, limit, &entries, &count) != 0)
            continue;
        for (size_t i = 0; i < count && total < cap; i++)
            all[total++] = entries[i];  /* steal ownership */
        free(entries);  /* free array only, not strings */
    }

    /* Sort by request_id DESC and apply limit */
    qsort(all, total, sizeof(*all), request_entry_cmp_desc);
    if (total > (size_t)limit) total = (size_t)limit;

    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < total; i++) {
        JSValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "request_id",
            JS_NewInt64(ctx, (int64_t)all[i].request_id));

        if (all[i].request_data) {
            JSValue s = JS_NewString(ctx, all[i].request_data);
            JSValue parsed = js_json_parse(ctx, s);
            JS_SetPropertyStr(ctx, entry, "request",
                JS_IsException(parsed) ? JS_NULL : parsed);
        } else {
            JS_SetPropertyStr(ctx, entry, "request", JS_NULL);
        }

        if (all[i].response_data) {
            JSValue s = JS_NewString(ctx, all[i].response_data);
            JSValue parsed = js_json_parse(ctx, s);
            JS_SetPropertyStr(ctx, entry, "response",
                JS_IsException(parsed) ? JS_NULL : parsed);
        } else {
            JS_SetPropertyStr(ctx, entry, "response", JS_NULL);
        }

        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, entry);
    }
    log_db_free_request_entries(all, total);
    return arr;
}

void js_install_logs(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue logs = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, logs, "query",
                      JS_NewCFunction(ctx, js_logs_query, "query", 1));
    JS_SetPropertyStr(ctx, logs, "replay",
                      JS_NewCFunction(ctx, js_logs_replay, "replay", 1));
    JS_SetPropertyStr(ctx, logs, "requests",
                      JS_NewCFunction(ctx, js_logs_requests, "requests", 1));
    JS_SetPropertyStr(ctx, global, "logs", logs);
    JS_FreeValue(ctx, global);
}
