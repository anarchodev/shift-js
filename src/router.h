#pragma once

/* Resolve a URL path to a module path within the code store.
 * "/foo/bar"  → "foo/bar/index.mjs"
 * "/"         → "index.mjs"
 * Returns a malloc'd string; caller frees. NULL on error. */
char *sjs_resolve_module(const char *url_path);
