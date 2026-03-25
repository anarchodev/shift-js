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
 * Generated module exports all HTTP method handlers (get, post, put, patch,
 * destroy, head, options) that each call a shared __render() function. */
char *sjs_ejs_transform(const char *source, size_t len, size_t *out_len);
