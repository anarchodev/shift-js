#pragma once

#include <stddef.h>

/* Result of URL routing: module base path + optional function name. */
typedef struct {
    char *module_path;   /* extensionless base path, e.g. "api/users/index" */
    char *func_name;     /* function segment from URL, or NULL */
    char *query_string;  /* query string (after '?'), or NULL */
} sjs_route_t;

/* Resolve a URL path to routing components.
 *
 * First strips the query string. Then produces two resolution candidates
 * for the caller to try in order:
 *
 * 1. Full path as module:  "/foo/bar" → module="foo/bar/index", func=NULL
 * 2. Last segment as func: "/foo/bar" → module="foo/index",     func="bar"
 *
 * The caller (sjs_dispatch) tries candidate 1 first. If no module is found,
 * it uses func_name from candidate 2.
 *
 * sjs_resolve_route fills in module_path for candidate 1.
 * sjs_resolve_route_fallback fills in module_path and func_name for candidate 2.
 *
 * All returned strings are malloc'd; caller frees via sjs_route_free(). */
void sjs_resolve_route(const char *url_path, sjs_route_t *route);
void sjs_resolve_route_fallback(const char *url_path, sjs_route_t *route);
void sjs_route_free(sjs_route_t *route);
