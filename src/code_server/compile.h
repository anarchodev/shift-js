#pragma once

#include "code_db.h"
#include "../preprocessor.h"
#include "../typescript.h"
#include <quickjs.h>

/* Compilation context — one per code server process.
 * Owns a long-lived QuickJS runtime for compiling modules,
 * the preprocessor registry, and the Sucrase/TypeScript context. */
typedef struct {
    JSRuntime *rt;
    JSContext *ctx;
    sjs_preprocessor_registry_t preprocessors;
    sjs_ts_ctx_t ts_ctx;

    /* Set during compilation for the module loader callback. */
    code_db_t *current_db;
    int64_t    current_tenant_id;
} compile_ctx_t;

/* Compiled module result from a single module compilation. */
typedef struct {
    char   *module_path;       /* base path without extension */
    void   *bytecode;          /* QuickJS serialized bytecode */
    size_t  bytecode_len;
    char   *source_map;        /* JSON source map or NULL */
    char   *content_hash;      /* SHA-256 of original source */
    char   *error;             /* error message or NULL on success */
} compile_result_t;

/* Full tree compilation result. */
typedef struct {
    char             *tree_hash;    /* SHA-256 of the full tree */
    compile_result_t *modules;      /* array of compiled modules */
    size_t            module_count;
    char             *error;        /* first error encountered, or NULL */
} compile_tree_result_t;

/* Initialize the compilation context (QuickJS runtime, preprocessors, Sucrase).
 * Returns 0 on success. */
int compile_ctx_init(compile_ctx_t *cc);

/* Free the compilation context. */
void compile_ctx_free(compile_ctx_t *cc);

/* Compile all modules for a tenant into a tree.
 * Stores results in the database (trees + tree_modules tables).
 * Returns the tree result (caller must call compile_tree_result_free). */
compile_tree_result_t compile_tree(compile_ctx_t *cc, code_db_t *db,
                                    int64_t tenant_id);

/* Free a single compile result's strings/blobs. */
void compile_result_free(compile_result_t *r);

/* Free a tree result and all its module results. */
void compile_tree_result_free(compile_tree_result_t *tr);
