#pragma once

#include <stddef.h>

/* Result of URL routing: module base path + optional function name. */
typedef struct {
    char *module_path;   /* extensionless base path, e.g. "api/users/index" */
    char *func_name;     /* function segment from URL, or NULL */
    char *query_string;  /* query string (after '?'), or NULL */
} sjs_route_t;

/* Resolve a URL path to a module path.
 *
 * Strips query string, leading/trailing slashes, appends "/index":
 *   "/"         → module="index"
 *   "/foo"      → module="foo/index"
 *   "/foo/bar"  → module="foo/bar/index"
 *
 * For .mjs modules, the function name comes from the "fn" query parameter
 * (GET) or "fn" body field (POST), not from the URL path.
 *
 * All returned strings are malloc'd; caller frees via sjs_route_free(). */
void sjs_resolve_route(const char *url_path, sjs_route_t *route);
void sjs_route_free(sjs_route_t *route);
