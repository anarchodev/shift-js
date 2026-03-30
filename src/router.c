#include "router.h"

#include <stdlib.h>
#include <string.h>

/* Build "index" or "<path>/index" from a cleaned path segment. */
static char *make_module_path(const char *p, size_t len) {
    if (len == 0) {
        char *r = strdup("index");
        if (!r) return NULL;
        return r;
    }

    const char *suffix = "/index";
    size_t suffix_len = 6;
    char *result = malloc(len + suffix_len + 1);
    if (!result) return NULL;
    memcpy(result, p, len);
    memcpy(result + len, suffix, suffix_len);
    result[len + suffix_len] = '\0';
    return result;
}

/* Strip leading slashes, trailing slashes, and query string.
 * Returns pointer into url_path; sets *len to the cleaned length.
 * Sets *query to the query string (after '?') or NULL. */
static const char *clean_path(const char *url_path,
                               size_t *len, char **query) {
    const char *p = url_path;
    while (*p == '/') p++;

    /* Find query string */
    const char *qmark = strchr(p, '?');
    size_t path_len;
    if (qmark) {
        path_len = (size_t)(qmark - p);
        *query = strdup(qmark + 1);  /* NULL on OOM — caller sees no query */
    } else {
        path_len = strlen(p);
        *query = NULL;
    }

    /* Strip trailing slashes */
    while (path_len > 0 && p[path_len - 1] == '/') path_len--;

    *len = path_len;
    return p;
}

void sjs_resolve_route(const char *url_path, sjs_route_t *route) {
    memset(route, 0, sizeof(*route));
    if (!url_path) return;

    size_t len;
    const char *p = clean_path(url_path, &len, &route->query_string);

    route->module_path = make_module_path(p, len);
    route->func_name = NULL;
}

void sjs_route_free(sjs_route_t *route) {
    free(route->module_path);
    free(route->func_name);
    free(route->query_string);
    memset(route, 0, sizeof(*route));
}
