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

/* Try to fetch __code/<base_path><ext> from KV with tenant prefix. */
static int try_extension(kvstore_t *kv, const char *base_path,
                          const char *ext, const char *kv_prefix,
                          char **out_resolved, void **source,
                          size_t *source_len) {
    /* Build full module name: base_path + ext */
    size_t blen = strlen(base_path);
    size_t elen = strlen(ext);
    char *module_name = malloc(blen + elen + 1);
    if (!module_name) return -1;
    memcpy(module_name, base_path, blen);
    memcpy(module_name + blen, ext, elen);
    module_name[blen + elen] = '\0';

    /* Build __code/<module_name> key */
    char raw_key[256];
    int n = snprintf(raw_key, sizeof(raw_key), "__code/%s", module_name);
    if (n < 0 || (size_t)n >= sizeof(raw_key)) {
        free(module_name);
        return -1;
    }

    char key_buf[512];
    const char *key = kv_prefixed_key(kv_prefix, raw_key,
                                       key_buf, sizeof(key_buf));
    if (!key) {
        free(module_name);
        return -1;
    }

    int rc = kv_get(kv, key, source, source_len);
    if (rc != 0) {
        free(module_name);
        return -1;
    }

    *out_resolved = module_name;
    return 0;
}

char *sjs_resolve_with_extensions(
    const sjs_preprocessor_registry_t *reg,
    kvstore_t *kv,
    const char *base_path,
    const char *kv_prefix,
    void **source, size_t *source_len) {

    char *resolved = NULL;

    /* Try .mjs first (native JS, no preprocessing needed) */
    if (try_extension(kv, base_path, ".mjs", kv_prefix,
                      &resolved, source, source_len) == 0)
        return resolved;

    /* Try each registered preprocessor extension */
    if (reg) {
        for (size_t i = 0; i < reg->count; i++) {
            if (try_extension(kv, base_path, reg->entries[i].extension,
                              kv_prefix, &resolved, source, source_len) == 0)
                return resolved;
        }
    }

    return NULL;
}
