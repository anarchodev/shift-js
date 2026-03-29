#pragma once

#include <stddef.h>

/* Transform EJS template source into a JS module string.
 * Returns malloc'd string. Caller frees. Returns NULL on error.
 *
 * Supported tags:
 *   <%= expr %>   — HTML-escaped output
 *   <%- expr %>   — raw output
 *   <% code  %>   — execute code
 *   <%# comment %> — ignored
 *
 * Generated module has a default export function that renders the template. */
char *sjs_ejs_transform(const char *source, size_t len, size_t *out_len,
                         void *user_data);
