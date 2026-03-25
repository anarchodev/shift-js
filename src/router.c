#include "router.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

char *sjs_resolve_module(const char *url_path) {
    if (!url_path) return NULL;

    /* Skip leading slash */
    const char *p = url_path;
    while (*p == '/') p++;

    /* Strip trailing slash */
    size_t len = strlen(p);
    while (len > 0 && p[len - 1] == '/') len--;

    /* Build: <path>/index.mjs or just index.mjs for root */
    const char *suffix = "/index.mjs";
    size_t suffix_len = strlen(suffix);

    char *result;
    if (len == 0) {
        result = strdup("index.mjs");
    } else {
        result = malloc(len + suffix_len + 1);
        if (!result) return NULL;
        memcpy(result, p, len);
        memcpy(result + len, suffix, suffix_len);
        result[len + suffix_len] = '\0';
    }

    return result;
}
