#include "preprocessor.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void sjs_preprocessor_init(sjs_preprocessor_registry_t *reg) {
    reg->count = 0;
}

int sjs_preprocessor_register(sjs_preprocessor_registry_t *reg,
                               const char *extension,
                               sjs_preprocess_fn fn,
                               void *user_data) {
    if (reg->count >= SJS_MAX_PREPROCESSORS) return -1;
    reg->entries[reg->count].extension  = extension;
    reg->entries[reg->count].transform  = fn;
    reg->entries[reg->count].user_data  = user_data;
    reg->count++;
    return 0;
}

const sjs_preprocessor_entry_t *sjs_preprocessor_find(
    const sjs_preprocessor_registry_t *reg, const char *extension) {
    if (!reg || !extension) return NULL;
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].extension, extension) == 0)
            return &reg->entries[i];
    }
    return NULL;
}

const char *sjs_path_extension(const char *path) {
    if (!path) return NULL;
    const char *dot = strrchr(path, '.');
    if (!dot || dot == path) return NULL;
    /* Ensure no slash after the dot (i.e. it's a real extension) */
    if (strchr(dot, '/')) return NULL;
    return dot;
}

/* Try to fetch <base_path><ext> via the fetch callback. */
static int try_extension(sjs_fetch_fn fetch, void *fetch_ctx,
                          const char *base_path, const char *ext,
                          char **out_resolved, void **source,
                          size_t *source_len) {
    size_t blen = strlen(base_path);
    size_t elen = strlen(ext);
    char *module_name = malloc(blen + elen + 1);
    if (!module_name) return -1;
    memcpy(module_name, base_path, blen);
    memcpy(module_name + blen, ext, elen);
    module_name[blen + elen] = '\0';

    int rc = fetch(module_name, source, source_len, fetch_ctx);
    if (rc != 0) {
        free(module_name);
        return -1;
    }

    *out_resolved = module_name;
    return 0;
}

char *sjs_resolve_with_extensions(
    const sjs_preprocessor_registry_t *reg,
    sjs_fetch_fn fetch, void *fetch_ctx,
    const char *base_path,
    void **source, size_t *source_len) {

    char *resolved = NULL;

    /* Try .mjs first (native JS, no preprocessing needed) */
    if (try_extension(fetch, fetch_ctx, base_path, ".mjs",
                      &resolved, source, source_len) == 0)
        return resolved;

    /* Try each registered preprocessor extension */
    if (reg) {
        for (size_t i = 0; i < reg->count; i++) {
            if (try_extension(fetch, fetch_ctx, base_path,
                              reg->entries[i].extension,
                              &resolved, source, source_len) == 0)
                return resolved;
        }
    }

    return NULL;
}
