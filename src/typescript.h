#pragma once

#include <quickjs.h>
#include <stddef.h>

/* Forward declaration for circular reference. */
typedef struct sjs_ts_ctx sjs_ts_ctx_t;

/* Per-extension transpiler binding: pairs the shared Sucrase function with
 * the correct options object ({transforms: ["typescript"]} vs
 * {transforms: ["typescript", "jsx"]}). */
typedef struct {
    sjs_ts_ctx_t *ts_ctx;
    JSValue      *options;         /* points into ts_ctx->ts_options or tsx_options */
    int           is_tsx;          /* 1 for .tsx (prepend JSX runtime), 0 for .ts */
    char         *last_source_map; /* malloc'd JSON from last transform, or NULL */
    size_t        last_source_map_len;
} sjs_ts_binding_t;

/* Sucrase TypeScript transpiler context — one per worker. */
struct sjs_ts_ctx {
    JSContext *ctx;
    JSValue    transform_fn;   /* sucrase.transform */
    JSValue    ts_options;     /* {transforms: ["typescript"]} */
    JSValue    tsx_options;    /* {transforms: ["typescript", "jsx"]} */
    JSValue    json_stringify; /* cached JSON.stringify reference */
    const char *current_module_path; /* set by caller before transform */
    int        initialized;
    sjs_ts_binding_t ts_binding;   /* .ts user_data target */
    sjs_ts_binding_t tsx_binding;  /* .tsx user_data target */
};

/* Evaluate the embedded Sucrase bundle in compile_ctx and cache references.
 * Returns 0 on success, -1 on error. */
int sjs_typescript_init(JSContext *compile_ctx, sjs_ts_ctx_t *ts);

/* Preprocessor callback: transpile TypeScript → JavaScript via Sucrase.
 * user_data must be a sjs_ts_binding_t*. Returns malloc'd JS string. */
char *sjs_typescript_transform(const char *source, size_t len,
                                size_t *out_len, void *user_data);

/* Free cached JSValues. Call before freeing the compile context. */
void sjs_typescript_free(sjs_ts_ctx_t *ts);
