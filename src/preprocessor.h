#pragma once

#include "kvstore.h"
#include <stddef.h>

/* Transform function: takes source bytes, returns malloc'd JS string.
 * Returns NULL on error. Caller frees the result. */
typedef char *(*sjs_preprocess_fn)(const char *source, size_t len,
                                    size_t *out_len);

#define SJS_MAX_PREPROCESSORS 16

typedef struct {
    const char       *extension;   /* e.g. ".ejs" — static string, not owned */
    sjs_preprocess_fn transform;
} sjs_preprocessor_entry_t;

typedef struct {
    sjs_preprocessor_entry_t entries[SJS_MAX_PREPROCESSORS];
    size_t                   count;
} sjs_preprocessor_registry_t;

void sjs_preprocessor_init(sjs_preprocessor_registry_t *reg);

int sjs_preprocessor_register(sjs_preprocessor_registry_t *reg,
                               const char *extension,
                               sjs_preprocess_fn fn);

sjs_preprocess_fn sjs_preprocessor_find(const sjs_preprocessor_registry_t *reg,
                                         const char *extension);

/* Return pointer to the extension within path (e.g. ".ejs"), or NULL. */
const char *sjs_path_extension(const char *path);

/* Resolve a module by probing KV for base_path with each known extension.
 * Tries ".mjs" first, then each registered preprocessor extension.
 * Returns malloc'd resolved filename (e.g. "index.ejs") on success,
 * sets *source and *source_len to the fetched content (caller frees *source).
 * Returns NULL if nothing found. */
char *sjs_resolve_with_extensions(
    const sjs_preprocessor_registry_t *reg,
    kvstore_t *kv,
    const char *base_path,
    const char *kv_prefix,
    void **source, size_t *source_len);
