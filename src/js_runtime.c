#include "js_runtime.h"
#include "kvstore.h"
#include "router.h"

#include <quickjs.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ======================================================================
 * Opaque key for per-context request state
 * ====================================================================== */

/* ======================================================================
 * kv global
 * ====================================================================== */

static kvstore_t *js_get_kv_from_rt(JSContext *ctx) {
    sjs_runtime_t *sjs = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    return sjs ? sjs->kv : NULL;
}

static JSValue js_kv_get(JSContext *ctx, JSValue this_val,
                         int argc, JSValue *argv) {
    kvstore_t *kv = js_get_kv_from_rt(ctx);
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
    kvstore_t *kv = js_get_kv_from_rt(ctx);
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
    kvstore_t *kv = js_get_kv_from_rt(ctx);
    if (!kv) return JS_EXCEPTION;

    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    int rc = kv_delete(kv, key);
    JS_FreeCString(ctx, key);

    return rc == 0 ? JS_TRUE : JS_EXCEPTION;
}

static JSValue js_kv_range(JSContext *ctx, JSValue this_val,
                           int argc, JSValue *argv) {
    kvstore_t *kv = js_get_kv_from_rt(ctx);
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
 * Module loader — loads source from __code/ in the KV store
 * ====================================================================== */

static JSModuleDef *sjs_module_loader(JSContext *ctx, const char *module_name,
                                      void *opaque) {
    kvstore_t *kv = js_get_kv_from_rt(ctx);
    if (!kv) return NULL;

    /* Build the KV key: __code/<module_name> */
    size_t name_len = strlen(module_name);
    size_t key_len = 7 + name_len; /* "__code/" + name */
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
 * Runtime lifecycle
 * ====================================================================== */

int sjs_runtime_init(sjs_runtime_t *sjs, kvstore_t *kv) {
    sjs->kv = kv;
    sjs->rt = JS_NewRuntime();
    if (!sjs->rt) return -1;

    JS_SetRuntimeOpaque(sjs->rt, sjs);
    JS_SetModuleLoaderFunc(sjs->rt, NULL, sjs_module_loader, NULL);

    /* Set reasonable memory/stack limits */
    JS_SetMemoryLimit(sjs->rt, 256 * 1024 * 1024);  /* 256 MB */
    JS_SetMaxStackSize(sjs->rt, 1024 * 1024);        /* 1 MB stack */

    return 0;
}

void sjs_runtime_free(sjs_runtime_t *sjs) {
    if (sjs->rt) {
        JS_FreeRuntime(sjs->rt);
        sjs->rt = NULL;
    }
}

/* ======================================================================
 * Request dispatch
 * ====================================================================== */

/* Map HTTP method string to JS export name */
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

static void sjs_request_ctx_free(sjs_request_ctx_t *req) {
    for (uint32_t i = 0; i < req->resp_header_count; i++) {
        free(req->resp_header_names[i]);
        free(req->resp_header_values[i]);
    }
    free(req->resp_header_names);
    free(req->resp_header_values);
}

char *sjs_dispatch(sjs_runtime_t *sjs, sjs_request_ctx_t *req,
                   uint32_t *out_len) {
    /* Default response state */
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

    /* Resolve the module path: /foo/bar → __code/foo/bar/index.mjs */
    char *module_path = sjs_resolve_module(req->path);
    if (!module_path) {
        req->resp_status = 500;
        char *body = strdup("Internal Server Error");
        *out_len = (uint32_t)strlen(body);
        return body;
    }

    /* Check if module source exists */
    char kv_key[512];
    snprintf(kv_key, sizeof(kv_key), "__code/%s", module_path);

    void  *source = NULL;
    size_t source_len = 0;
    int rc = kv_get(sjs->kv, kv_key, &source, &source_len);
    if (rc != 0) {
        free(module_path);
        req->resp_status = 404;
        char *body = strdup("Not Found");
        *out_len = (uint32_t)strlen(body);
        return body;
    }

    /* Create a per-request JS context */
    JSContext *ctx = JS_NewContext(sjs->rt);
    if (!ctx) {
        free(source);
        free(module_path);
        req->resp_status = 500;
        char *body = strdup("Internal Server Error");
        *out_len = (uint32_t)strlen(body);
        return body;
    }

    JS_SetContextOpaque(ctx, req);

    /* Install globals */
    js_install_kv(ctx);
    js_install_request(ctx);
    js_install_response(ctx);

    /* Try bytecode cache first */
    char cache_key[512];
    snprintf(cache_key, sizeof(cache_key), "__compiled/%s", module_path);

    void  *bytecode = NULL;
    size_t bc_len = 0;
    JSValue module_val;

    if (kv_get(sjs->kv, cache_key, &bytecode, &bc_len) == 0) {
        /* Load from bytecode cache */
        module_val = JS_ReadObject(ctx, bytecode, bc_len,
                                   JS_READ_OBJ_BYTECODE);
        free(bytecode);
        if (JS_IsException(module_val)) {
            /* Cache invalid, fall through to source compilation */
            JS_FreeValue(ctx, JS_GetException(ctx));
            goto compile_source;
        }
    } else {
compile_source:
        /* Compile from source */
        module_val = JS_Eval(ctx, source, source_len, module_path,
                             JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(module_val)) {
            JSValue exc = JS_GetException(ctx);
            const char *err = JS_ToCString(ctx, exc);
            fprintf(stderr, "shift-js: compile error in %s: %s\n",
                    module_path, err ? err : "(unknown)");
            JS_FreeCString(ctx, err);
            JS_FreeValue(ctx, exc);
            JS_FreeContext(ctx);
            free(source);
            free(module_path);
            req->resp_status = 500;
            char *body = strdup("Module Compilation Error");
            *out_len = (uint32_t)strlen(body);
            return body;
        }

        /* Cache the bytecode */
        size_t out_bc_len;
        uint8_t *bc = JS_WriteObject(ctx, &out_bc_len, module_val,
                                     JS_WRITE_OBJ_BYTECODE);
        if (bc) {
            kv_put(sjs->kv, cache_key, bc, out_bc_len);
            js_free(ctx, bc);
        }
    }

    free(source);

    /* Save module def pointer before EvalFunction consumes the value */
    JSModuleDef *mod_def = JS_VALUE_GET_PTR(module_val);

    /* Evaluate the module (executes top-level code, resolves exports) */
    JSValue result = JS_EvalFunction(ctx, module_val);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char *err = JS_ToCString(ctx, exc);
        fprintf(stderr, "shift-js: module eval error in %s: %s\n",
                module_path, err ? err : "(unknown)");
        JS_FreeCString(ctx, err);
        JS_FreeValue(ctx, exc);
        JS_FreeContext(ctx);
        free(module_path);
        req->resp_status = 500;
        char *body = strdup("Module Evaluation Error");
        *out_len = (uint32_t)strlen(body);
        return body;
    }
    JS_FreeValue(ctx, result);

    /* Get the module's namespace object to access exports */
    JSValue ns = JS_GetModuleNamespace(ctx, mod_def);

    /* Look up the handler function */
    JSValue handler = JS_GetPropertyStr(ctx, ns, func_name);
    JS_FreeValue(ctx, ns);

    char *body = NULL;

    if (!JS_IsFunction(ctx, handler)) {
        JS_FreeValue(ctx, handler);
        JS_FreeContext(ctx);
        free(module_path);
        req->resp_status = 405;
        body = strdup("Method Not Allowed");
        *out_len = (uint32_t)strlen(body);
        return body;
    }

    /* Call the handler */
    JSValue ret = JS_Call(ctx, handler, JS_UNDEFINED, 0, NULL);
    JS_FreeValue(ctx, handler);

    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(ctx);
        const char *err = JS_ToCString(ctx, exc);
        fprintf(stderr, "shift-js: handler error in %s.%s: %s\n",
                module_path, func_name, err ? err : "(unknown)");
        JS_FreeCString(ctx, err);
        JS_FreeValue(ctx, exc);
        JS_FreeContext(ctx);
        free(module_path);
        req->resp_status = 500;
        body = strdup("Handler Error");
        *out_len = (uint32_t)strlen(body);
        return body;
    }

    /* Convert return value to body string */
    if (JS_IsString(ret)) {
        size_t len;
        const char *str = JS_ToCStringLen(ctx, &len, ret);
        body = malloc(len);
        if (body) {
            memcpy(body, str, len);
            *out_len = (uint32_t)len;
        }
        JS_FreeCString(ctx, str);
    } else if (!JS_IsUndefined(ret) && !JS_IsNull(ret)) {
        /* JSON.stringify for non-string return values */
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
            JS_FreeCString(ctx, str);

            /* Auto-set content-type to application/json if not already set */
            bool has_ct = false;
            for (uint32_t i = 0; i < req->resp_header_count; i++) {
                if (!strcasecmp(req->resp_header_names[i], "content-type")) {
                    has_ct = true;
                    break;
                }
            }
            if (!has_ct) {
                /* Temporarily create a fake JSValue to reuse header func,
                 * or just directly set it */
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

        JS_FreeValue(ctx, json_str);
        JS_FreeValue(ctx, stringify);
        JS_FreeValue(ctx, json);
        JS_FreeValue(ctx, global);
    }

    JS_FreeValue(ctx, ret);
    JS_FreeContext(ctx);
    free(module_path);

    if (!body) {
        body = strdup("");
        *out_len = 0;
    }

    return body;
}
