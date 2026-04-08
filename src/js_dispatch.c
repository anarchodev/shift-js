#define _GNU_SOURCE
#include "js_globals.h"
#include "js_runtime.h"
#include "code_store.h"
#include "kvstore.h"
#include "router.h"
#include "session.h"
#include "replay_capture.h"
#include "log_db.h"
#include "raft_thread.h"

#include <quickjs.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

/* Refresh the cached current_tree_hash from the in-memory code store. */
static void refresh_tree_hash(sjs_runtime_t *sjs) {
    const char *h = code_store_tree_hash(sjs->code_store);
    if (h) {
        snprintf(sjs->current_tree_hash,
                 sizeof(sjs->current_tree_hash), "%s", h);
    } else {
        sjs->current_tree_hash[0] = '\0';
    }
}

/* ======================================================================
 * Response header helpers
 * ====================================================================== */

static void resp_add_header(sjs_resp_headers_t *h,
                            const char *name, const char *value) {
    if (!h) return;
    if (h->count >= h->cap || !h->names || !h->values) {
        uint32_t nc = h->cap ? h->cap * 2 : 8;
        char **nn = realloc(h->names,  nc * sizeof(char *));
        if (!nn) return;
        h->names = nn;
        char **nv = realloc(h->values, nc * sizeof(char *));
        if (!nv) return;
        h->values = nv;
        h->cap = nc;
    }
    h->names[h->count]  = strdup(name);
    h->values[h->count] = strdup(value);
    if (!h->names[h->count] || !h->values[h->count]) {
        free(h->names[h->count]);
        free(h->values[h->count]);
        return;
    }
    h->count++;
}

static bool resp_has_header(const sjs_resp_headers_t *h, const char *name) {
    for (uint32_t i = 0; i < h->count; i++)
        if (!strcasecmp(h->names[i], name)) return true;
    return false;
}

/* ======================================================================
 * Query string parsing
 * ====================================================================== */

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

/* (Module compilation moved to code_server.c — worker only loads
 * pre-compiled bytecode from the in-memory code store.) */

/* ======================================================================
 * Body extraction (return value → libc heap string)
 * ====================================================================== */

static char *extract_body_string(JSContext *ctx, qjs_snap_arena_t *arena,
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
    JSValue json_str = js_json_stringify(ctx, ret);
    if (JS_IsException(json_str)) {
        const char *err = js_err_string(ctx, arena);
        fprintf(stderr, "shift-js: JSON.stringify error in %s.%s: %s\n",
                module_path, func_name, err);
        if (asprintf(err_msg, "JSON.stringify error in %s.%s: %s",
                     module_path, func_name, err) < 0)
            *err_msg = NULL;
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
            resp_add_header(resp_hdrs, "content-type", "application/json; charset=utf-8");
    }
    return body;
}

/* ======================================================================
 * Route resolution
 * ====================================================================== */

static int sjs_resolve_request_route(sjs_runtime_t *sjs, const char *path,
                              const char *kv_prefix,
                              sjs_route_info_t *route, sjs_bytecode_t *bc,
                              char **err_body, uint32_t *err_len) {
    refresh_tree_hash(sjs);

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

    /* Look up bytecode from in-memory code store */
    {
        static const char *exts[] = { ".mjs", ".ejs", ".ts", ".tsx" };
        for (int ei = 0; ei < 4; ei++) {
            char mod_name[256];
            snprintf(mod_name, sizeof(mod_name), "%s%s", base_path, exts[ei]);

            code_entry_t entry;
            if (code_store_get(sjs->code_store, mod_name, &entry) == 0) {
                bc->data = malloc(entry.bytecode_len);
                memcpy(bc->data, entry.bytecode, entry.bytecode_len);
                bc->len = entry.bytecode_len;
                route->module_path = strdup(mod_name);
                if (sjs->current_replay_capture && entry.source_hash[0])
                    replay_capture_module(sjs->current_replay_capture,
                                          mod_name, entry.source_hash);
                goto found;
            }
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

/* ======================================================================
 * Session loading and persistence
 * ====================================================================== */

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
                if (req->replay_capture)
                    replay_capture_session(req->replay_capture, sdata, sdata_len);

                JSValue json_str = JS_NewStringLen(ctx, sdata, sdata_len);
                JSValue parsed = js_json_parse(ctx, json_str);

                if (!JS_IsException(parsed)) {
                    JSValue s = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "session");
                    JS_SetPropertyStr(ctx, s, "__data", parsed);
                } else {
                    JS_GetException(ctx); /* clear */
                }

                free(sdata);
            }
        }
    }
}

static void sjs_session_persist(sjs_runtime_t *sjs, sjs_request_ctx_t *req,
                                 JSContext *ctx) {
    sjs_session_t *sess = req->session;
    if (!sess->id || (!sess->is_dirty && !sess->is_new))
        return;

    if (sess->is_dirty) {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue s = JS_GetPropertyStr(ctx, global, "session");
        JSValue data = JS_GetPropertyStr(ctx, s, "__data");

        JSValue json_val = js_json_stringify(ctx, data);

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
    }

    if (sess->is_new) {
        char *cookie = sjs_session_cookie_header(sess->id);
        if (cookie) {
            resp_add_header(req->resp_hdrs, "set-cookie", cookie);
            free(cookie);
        }
    }
}

/* ======================================================================
 * JSONP wrapping
 * ====================================================================== */

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

/* ======================================================================
 * Replay capture flush helper
 * ====================================================================== */

/* Populate the log record with replay capture data.  Ownership of
 * the replay_capture tape buffers transfers to the log_record (the
 * pointers are stolen, not copied). */
static void capture_replay(sjs_request_ctx_t *req, JSContext *ctx) {
    sjs_replay_capture_t *cap = req->replay_capture;
    sjs_log_record_t *rec = req->log_record;
    if (!cap || !rec) return;

    rec->request_id = req->request_id;
    rec->worker_id = (int)(req->request_id >> 48);
    rec->session_id = (req->session && req->session->id)
                      ? strdup(req->session->id) : NULL;

    replay_capture_finalize(&cap->kv_tape);
    replay_capture_finalize(&cap->date_tape);
    replay_capture_finalize(&cap->math_random_tape);
    replay_capture_finalize(&cap->module_tree);

    /* Build request data JSON */
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

        if (cap->session_json) {
            JSValue sjs_str = JS_NewString(ctx, cap->session_json);
            JSValue parsed = js_json_parse(ctx, sjs_str);
            JS_SetPropertyStr(ctx, obj, "session",
                JS_IsException(parsed) ? JS_NULL : parsed);
        } else {
            JS_SetPropertyStr(ctx, obj, "session", JS_NULL);
        }

        JSValue str_result = js_json_stringify(ctx, obj);
        if (JS_IsString(str_result)) {
            const char *s = JS_ToCString(ctx, str_result);
            rec->req_json = strdup(s);
            JS_FreeCString(ctx, s);
        }
    }

    /* Steal tape buffers from replay capture (avoid copy) */
    rec->kv_tape = cap->kv_tape.data;
    cap->kv_tape.data = NULL;
    rec->date_tape = cap->date_tape.data;
    cap->date_tape.data = NULL;
    rec->math_random_tape = cap->math_random_tape.data;
    cap->math_random_tape.data = NULL;
    rec->module_tree = cap->module_tree.data;
    cap->module_tree.data = NULL;

    /* Copy random tape (owned by ECS component, freed on arena reset) */
    if (req->tape && req->tape->data && req->tape->len > 0) {
        rec->random_tape = malloc(req->tape->len);
        if (rec->random_tape) {
            memcpy(rec->random_tape, req->tape->data, req->tape->len);
            rec->random_tape_len = req->tape->len;
        }
    }
}

/* Copy console.log entries from the per-request batch into the log record.
 * Steals the msg pointers (caller must not free them). */
static void capture_logs(sjs_request_ctx_t *req) {
    sjs_log_record_t *rec = req->log_record;
    log_batch_t *batch = req->log_batch;
    if (!rec || !batch || batch->count == 0) return;

    rec->log_entries = malloc(batch->count * sizeof(log_pending_t));
    if (!rec->log_entries) return;
    memcpy(rec->log_entries, batch->entries,
           batch->count * sizeof(log_pending_t));
    rec->log_count = batch->count;
    /* Ownership of msg strings transferred — prevent double-free */
    batch->count = 0;
}

/* ======================================================================
 * Main dispatch
 * ====================================================================== */

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

            if (req->log_batch) {
                for (uint32_t li = 0; li < req->log_batch->count; li++)
                    free((void *)req->log_batch->entries[li].msg);
                req->log_batch->count = 0;
            }

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
        qjs_snap_arena_t *arena = sjs->arena;

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

        if (req->tape) {
            req->tape->len = 0;
            req->tape->pos = 0;
        }

        JSValue module_val = JS_ReadObject(ctx, bc->data, bc->len,
                                           JS_READ_OBJ_BYTECODE);
        free(bc->data);
        bc->data = NULL;

        if (JS_IsException(module_val)) {
            const char *err = js_err_string(ctx, arena);
            fprintf(stderr, "shift-js: bytecode load error in %s: %s\n",
                    route->module_path, err);
            char *body;
            if (asprintf(&body, "bytecode load error in %s: %s",
                         route->module_path, err) < 0)
                body = strdup("bytecode load error");
            kv_rollback(sjs->kv);
            arena_reset(sjs);
            req->resp_st->code = 500;
            *out_len = body ? (uint32_t)strlen(body) : 0;
            return body;
        }

        JSModuleDef *mod_def = JS_VALUE_GET_PTR(module_val);

        JSValue result = JS_EvalFunction(ctx, module_val);
        if (JS_IsException(result)) {
            const char *err = js_err_string_with_stack(ctx);
            fprintf(stderr, "shift-js: module eval error in %s: %s\n",
                    route->module_path, err);
            if (asprintf(&err_msg, "module eval error in %s: %s",
                         route->module_path, err) < 0)
                err_msg = NULL;
            goto txn_fail;
        }

        /* Pump microtask queue */
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
            if (asprintf(&err_msg, "module rejected in %s: %s",
                         route->module_path, err) < 0)
                err_msg = NULL;
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
                resp_add_header(req->resp_hdrs, "content-type", "text/html; charset=utf-8");

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

                JS_DeleteProperty(ctx, qs_obj,
                    JS_NewAtom(ctx, "fn"), 0);
                args = qs_obj;
            } else {
                JSValue body_obj = JS_UNDEFINED;
                if (req->body && req->body_len > 0) {
                    JSValue body_str = JS_NewStringLen(ctx, req->body, req->body_len);
                    body_obj = js_json_parse(ctx, body_str);
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
                if (asprintf(&body, "function \"%s\" not found", fn_name) < 0)
                    body = strdup("function not found");
                JS_FreeCString(ctx, fn_name);
                *out_len = body ? (uint32_t)strlen(body) : 0;
                return body;
            }

            ret = JS_Call(ctx, handler, JS_UNDEFINED, 1, &args);
            JS_FreeCString(ctx, fn_name);
        }

        /* Capture exception immediately */
        if (JS_IsException(ret)) {
            const char *err = js_err_string_with_stack(ctx);
            fprintf(stderr, "shift-js: handler error in %s.%s: %s\n",
                    route->module_path, called_func, err);
            if (asprintf(&err_msg, "handler error in %s.%s: %s",
                         route->module_path, called_func, err) < 0)
                err_msg = NULL;
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
            if (asprintf(&err_msg, "async handler rejected in %s.%s: %s",
                         route->module_path, called_func, err ? err : "(unknown)") < 0)
                err_msg = NULL;
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

        /* Capture logs + replay into log record for batched flush */
        capture_logs(req);
        capture_replay(req, ctx);

        sjs->current_replay_capture = NULL;
        arena_reset(sjs);

        if (!body) {
            body = strdup("");
            *out_len = 0;
        }
        return body;

    txn_fail:
        kv_rollback(sjs->kv);
        req->resp_st->code = 500;
        capture_logs(req);
        capture_replay(req, ctx);
        arena_reset(sjs);
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
