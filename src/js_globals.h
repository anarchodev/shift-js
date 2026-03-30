#pragma once

#include "js_runtime.h"
#include <quickjs.h>
#include <stddef.h>

/* Install JS global objects into a QuickJS context.
 * Called during snapshot initialization. */
void js_install_kv(JSContext *ctx);
void js_install_code(JSContext *ctx);
void js_install_request(JSContext *ctx);
void js_install_response(JSContext *ctx);
void js_install_session(JSContext *ctx);
void js_install_console(JSContext *ctx);
void js_install_logs(JSContext *ctx);

/* SHA-256 hex string (returns thread-local static buffer). */
const char *sha256_hex(const void *data, size_t len);

/* JS error extraction helpers. */
const char *js_err_string(JSContext *ctx, sjs_arena_t *arena);
const char *js_err_string_with_stack(JSContext *ctx);

/* JSON stringify/parse via the global JSON object.
 * These run in per-request arena context so intermediate JSValues
 * don't need manual freeing. */
static inline JSValue js_json_stringify(JSContext *ctx, JSValue val) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue json = JS_GetPropertyStr(ctx, global, "JSON");
    JSValue stringify = JS_GetPropertyStr(ctx, json, "stringify");
    return JS_Call(ctx, stringify, json, 1, &val);
}

static inline JSValue js_json_parse(JSContext *ctx, JSValue str) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue json = JS_GetPropertyStr(ctx, global, "JSON");
    JSValue parse = JS_GetPropertyStr(ctx, json, "parse");
    return JS_Call(ctx, parse, json, 1, &str);
}
