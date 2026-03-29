#include "compile.h"
#include "../ejs.h"

#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================
 * SHA-256 helper
 * ====================================================================== */

static void sha256_hex(const void *data, size_t len, char out[65]) {
    unsigned char hash[32];
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(mdctx, data, len);
    EVP_DigestFinal_ex(mdctx, hash, NULL);
    EVP_MD_CTX_free(mdctx);
    for (int i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", hash[i]);
    out[64] = '\0';
}

/* ======================================================================
 * Module loader callback for QuickJS
 *
 * When QuickJS encounters an `import` during compilation, it calls this
 * to resolve and compile the dependency. We read from the code_db
 * modules table instead of KV.
 * ====================================================================== */

/* Find the extension portion of a path, e.g. ".ts" from "foo/bar.ts" */
static const char *path_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot || dot == path) return NULL;
    if (strchr(dot, '/')) return NULL;
    return dot;
}

/* Try to load a module from code_db with a given extension appended.
 * Returns 0 on success (populates source, source_len, resolved_path). */
static int try_load(code_db_t *db, int64_t tenant_id,
                    const char *base_path, const char *ext,
                    void **source, size_t *source_len,
                    char **resolved_path, char **extension_out) {
    size_t blen = strlen(base_path);
    size_t elen = strlen(ext);
    char *try_path = malloc(blen + elen + 1);
    memcpy(try_path, base_path, blen);
    memcpy(try_path + blen, ext, elen);
    try_path[blen + elen] = '\0';

    code_module_t m;
    if (code_db_get_module(db, tenant_id, try_path, &m) != 0) {
        free(try_path);
        return -1;
    }

    *source = m.source;
    *source_len = m.source_len;
    *resolved_path = try_path;
    *extension_out = m.extension;
    free(m.path);
    free(m.content_hash);
    return 0;
}

/* Resolve a module path by probing .mjs, then each preprocessor extension. */
static int resolve_module(compile_ctx_t *cc, const char *module_name,
                          void **source, size_t *source_len,
                          char **resolved_path, char **extension) {
    code_db_t *db = cc->current_db;
    int64_t tid = cc->current_tenant_id;

    /* If name has explicit extension, fetch directly */
    const char *ext = path_ext(module_name);
    if (ext) {
        code_module_t m;
        if (code_db_get_module(db, tid, module_name, &m) != 0)
            return -1;
        *source = m.source;
        *source_len = m.source_len;
        *resolved_path = m.path;
        *extension = m.extension;
        free(m.content_hash);
        return 0;
    }

    /* Try .mjs first */
    if (try_load(db, tid, module_name, ".mjs",
                 source, source_len, resolved_path, extension) == 0)
        return 0;

    /* Try each registered preprocessor extension */
    for (size_t i = 0; i < cc->preprocessors.count; i++) {
        const char *pp_ext = cc->preprocessors.entries[i].extension;
        if (try_load(db, tid, module_name, pp_ext,
                     source, source_len, resolved_path, extension) == 0)
            return 0;
    }

    return -1;
}

/* QuickJS module loader callback */
static JSModuleDef *code_module_loader(JSContext *ctx,
                                        const char *module_name,
                                        void *opaque) {
    compile_ctx_t *cc = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    if (!cc) return NULL;

    void *source = NULL;
    size_t source_len = 0;
    char *resolved_path = NULL;
    char *extension = NULL;

    if (resolve_module(cc, module_name, &source, &source_len,
                       &resolved_path, &extension) != 0)
        return NULL;

    const char *resolved_name = resolved_path ? resolved_path : module_name;

    /* Apply preprocessor if extension has one registered */
    char *compile_source = source;
    size_t compile_len = source_len;

    const char *ext = path_ext(resolved_name);
    const sjs_preprocessor_entry_t *pp = ext
        ? sjs_preprocessor_find(&cc->preprocessors, ext) : NULL;

    if (pp) {
        if (pp->transform == sjs_typescript_transform) {
            sjs_ts_binding_t *binding = pp->user_data;
            if (binding && binding->ts_ctx)
                binding->ts_ctx->current_module_path = resolved_name;
        }
        size_t js_len;
        char *js = pp->transform(source, source_len, &js_len, pp->user_data);
        free(source);
        if (!js) { free(resolved_path); free(extension); return NULL; }
        compile_source = js;
        compile_len = js_len;
    }

    JSValue func = JS_Eval(ctx, compile_source, compile_len, resolved_name,
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(compile_source);
    free(resolved_path);
    free(extension);

    if (JS_IsException(func)) return NULL;

    JSModuleDef *mod = JS_VALUE_GET_PTR(func);
    JS_FreeValue(ctx, func);
    return mod;
}

/* ======================================================================
 * Compile context init / free
 * ====================================================================== */

int compile_ctx_init(compile_ctx_t *cc) {
    memset(cc, 0, sizeof(*cc));

    cc->rt = JS_NewRuntime();
    if (!cc->rt) return -1;

    JS_SetRuntimeOpaque(cc->rt, cc);
    JS_SetModuleLoaderFunc(cc->rt, NULL, code_module_loader, NULL);

    cc->ctx = JS_NewContext(cc->rt);
    if (!cc->ctx) {
        JS_FreeRuntime(cc->rt);
        return -1;
    }

    /* Set up preprocessor registry */
    sjs_preprocessor_init(&cc->preprocessors);
    sjs_preprocessor_register(&cc->preprocessors, ".ejs",
                               sjs_ejs_transform, NULL);
    sjs_preprocessor_register(&cc->preprocessors, ".ts",
                               sjs_typescript_transform, NULL);
    sjs_preprocessor_register(&cc->preprocessors, ".tsx",
                               sjs_typescript_transform, NULL);

    /* Initialize Sucrase in the compile context */
    if (sjs_typescript_init(cc->ctx, &cc->ts_ctx) != 0) {
        fprintf(stderr, "compile: failed to initialize Sucrase\n");
        JS_FreeContext(cc->ctx);
        JS_FreeRuntime(cc->rt);
        return -1;
    }

    /* Patch preprocessor entries with per-context TypeScript bindings */
    for (size_t i = 0; i < cc->preprocessors.count; i++) {
        sjs_preprocessor_entry_t *e = &cc->preprocessors.entries[i];
        if (e->transform != sjs_typescript_transform) continue;
        if (strcmp(e->extension, ".tsx") == 0)
            e->user_data = &cc->ts_ctx.tsx_binding;
        else
            e->user_data = &cc->ts_ctx.ts_binding;
    }

    return 0;
}

void compile_ctx_free(compile_ctx_t *cc) {
    if (!cc) return;
    sjs_typescript_free(&cc->ts_ctx);
    if (cc->ctx) { JS_FreeContext(cc->ctx); cc->ctx = NULL; }
    if (cc->rt) { JS_FreeRuntime(cc->rt); cc->rt = NULL; }
}

/* ======================================================================
 * Single module compilation
 * ====================================================================== */

/* Strip extension from a path to get the base path.
 * Returns malloc'd string. "api/users.ts" → "api/users" */
static char *strip_extension(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot || dot == path || strchr(dot, '/'))
        return strdup(path);
    return strndup(path, (size_t)(dot - path));
}

static int compile_single(compile_ctx_t *cc, code_module_t *mod,
                           compile_result_t *out) {
    memset(out, 0, sizeof(*out));
    out->module_path = strip_extension(mod->path);
    out->content_hash = strdup(mod->content_hash);

    /* Make a null-terminated copy of the source for QuickJS */
    char *compile_source = malloc(mod->source_len + 1);
    if (!compile_source) {
        out->error = strdup("out of memory");
        return -1;
    }
    memcpy(compile_source, mod->source, mod->source_len);
    compile_source[mod->source_len] = '\0';
    size_t compile_len = mod->source_len;
    bool source_owned = true;

    const char *ext = path_ext(mod->path);
    const sjs_preprocessor_entry_t *pp = ext
        ? sjs_preprocessor_find(&cc->preprocessors, ext) : NULL;

    if (pp) {
        if (pp->transform == sjs_typescript_transform) {
            sjs_ts_binding_t *binding = pp->user_data;
            if (binding && binding->ts_ctx)
                binding->ts_ctx->current_module_path = mod->path;
        }
        size_t js_len;
        char *js = pp->transform(compile_source, compile_len,
                                  &js_len, pp->user_data);
        free(compile_source);
        if (!js) {
            asprintf(&out->error, "preprocessor error in %s", mod->path);
            return -1;
        }
        compile_source = js;
        compile_len = js_len;

        /* Extract source map if TypeScript */
        if (pp->transform == sjs_typescript_transform) {
            sjs_ts_binding_t *binding = pp->user_data;
            if (binding && binding->last_source_map) {
                out->source_map = binding->last_source_map;
                binding->last_source_map = NULL;
                binding->last_source_map_len = 0;
            }
        }
    }

    /* Compile to bytecode */
    JSValue module_val = JS_Eval(cc->ctx, compile_source, compile_len,
                                  mod->path,
                                  JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (source_owned) free(compile_source);

    if (JS_IsException(module_val)) {
        JSValue exc = JS_GetException(cc->ctx);
        const char *err = JS_ToCString(cc->ctx, exc);
        asprintf(&out->error, "compile error in %s: %s",
                 mod->path, err ? err : "(unknown)");
        JS_FreeCString(cc->ctx, err);
        JS_FreeValue(cc->ctx, exc);
        return -1;
    }

    /* Serialize to bytecode */
    size_t bc_len;
    uint8_t *bc = JS_WriteObject(cc->ctx, &bc_len, module_val,
                                  JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(cc->ctx, module_val);

    if (!bc) {
        asprintf(&out->error, "bytecode serialization failed for %s", mod->path);
        return -1;
    }

    out->bytecode = bc;
    out->bytecode_len = bc_len;
    return 0;
}

/* ======================================================================
 * Full tree compilation
 * ====================================================================== */

/* Compare compile_result_t by module_path for sorting */
static int cmp_results(const void *a, const void *b) {
    const compile_result_t *ra = a, *rb = b;
    return strcmp(ra->module_path, rb->module_path);
}

compile_tree_result_t compile_tree(compile_ctx_t *cc, code_db_t *db,
                                    int64_t tenant_id) {
    compile_tree_result_t tr = {0};

    /* Set compilation context for module loader */
    cc->current_db = db;
    cc->current_tenant_id = tenant_id;

    /* Fetch all modules for this tenant */
    size_t mod_count;
    code_module_t *modules = code_db_list_modules(db, tenant_id, &mod_count);
    if (!modules || mod_count == 0) {
        tr.error = strdup("no modules found");
        free(modules);
        cc->current_db = NULL;
        return tr;
    }

    /* We need full source for compilation — re-fetch each module */
    tr.modules = calloc(mod_count, sizeof(compile_result_t));
    tr.module_count = 0;

    for (size_t i = 0; i < mod_count; i++) {
        code_module_t full_mod;
        if (code_db_get_module(db, tenant_id, modules[i].path, &full_mod) != 0) {
            fprintf(stderr, "compile_tree: failed to fetch %s\n", modules[i].path);
            continue;
        }

        compile_result_t *r = &tr.modules[tr.module_count];
        if (compile_single(cc, &full_mod, r) != 0) {
            if (!tr.error) tr.error = strdup(r->error);
            fprintf(stderr, "compile_tree: %s\n", r->error);
            /* Keep going to report all errors, but mark the tree as failed */
        }
        tr.module_count++;
        code_module_free(&full_mod);
    }

    /* Free module list */
    for (size_t i = 0; i < mod_count; i++)
        code_module_free(&modules[i]);
    free(modules);

    cc->current_db = NULL;

    /* If any errors, don't build the tree */
    if (tr.error) return tr;

    /* Sort results by module_path for deterministic hashing */
    qsort(tr.modules, tr.module_count, sizeof(compile_result_t), cmp_results);

    /* Compute tree hash: SHA-256 of all (module_path + bytecode) */
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL);
    for (size_t i = 0; i < tr.module_count; i++) {
        compile_result_t *r = &tr.modules[i];
        EVP_DigestUpdate(mdctx, r->module_path, strlen(r->module_path));
        EVP_DigestUpdate(mdctx, r->bytecode, r->bytecode_len);
    }
    unsigned char hash[32];
    EVP_DigestFinal_ex(mdctx, hash, NULL);
    EVP_MD_CTX_free(mdctx);

    tr.tree_hash = malloc(65);
    for (int i = 0; i < 32; i++)
        snprintf(tr.tree_hash + i * 2, 3, "%02x", hash[i]);
    tr.tree_hash[64] = '\0';

    /* Store in database */
    if (code_db_put_tree(db, tr.tree_hash, tenant_id,
                          (int)tr.module_count) != 0) {
        free(tr.tree_hash);
        tr.tree_hash = NULL;
        tr.error = strdup("failed to store tree");
        return tr;
    }

    for (size_t i = 0; i < tr.module_count; i++) {
        compile_result_t *r = &tr.modules[i];
        code_db_put_tree_module(db, tr.tree_hash, r->module_path,
                                 r->bytecode, r->bytecode_len,
                                 r->source_map, r->content_hash);
    }

    return tr;
}

/* ======================================================================
 * Cleanup
 * ====================================================================== */

void compile_result_free(compile_result_t *r) {
    if (!r) return;
    free(r->module_path);
    free(r->bytecode);
    free(r->source_map);
    free(r->content_hash);
    free(r->error);
    memset(r, 0, sizeof(*r));
}

void compile_tree_result_free(compile_tree_result_t *tr) {
    if (!tr) return;
    for (size_t i = 0; i < tr->module_count; i++)
        compile_result_free(&tr->modules[i]);
    free(tr->modules);
    free(tr->tree_hash);
    free(tr->error);
    memset(tr, 0, sizeof(*tr));
}
