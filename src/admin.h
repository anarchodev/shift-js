#pragma once

#include "kvstore.h"
#include <stddef.h>
#include <stdint.h>

/* Admin API request (parsed from HTTP/2 headers + body). */
typedef struct {
    const char *method;      /* "GET", "POST", "DELETE" */
    const char *path;        /* full path, e.g. "/_admin/kv?key=foo" */
    const char *body;        /* request body (may be NULL) */
    uint32_t    body_len;
    const char *kv_prefix;   /* tenant prefix (may be NULL) */
} admin_request_t;

/* Admin API response (caller frees body). */
typedef struct {
    uint16_t    status;
    char       *body;        /* malloc'd JSON response */
    uint32_t    body_len;
    const char *content_type; /* static string, not owned */
} admin_response_t;

/* Check if a path should be handled by the admin API. */
int admin_match(const char *path, uint32_t path_len);

/* Dispatch an admin API request.  Returns 0 on success.
 * Caller must free resp->body. */
int admin_dispatch(kvstore_t *kv, const admin_request_t *req,
                   admin_response_t *resp);
