#pragma once

#include "kvstore.h"
#include <stddef.h>
#include <stdint.h>

typedef struct code_db  code_db_t;
typedef struct code_store code_store_t;

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

/* Services available to admin endpoints. */
typedef struct {
    kvstore_t    *kv;
    code_db_t    *code_db;    /* may be NULL */
    code_store_t *code_store; /* may be NULL */
} admin_services_t;

/* Check if a path should be handled by the admin API. */
int admin_match(const char *path, uint32_t path_len);

/* Dispatch an admin API request.
 * Returns 0 on success, 1 if the request needs to be proxied to the
 * code server (code_db is NULL and path is /upload or /deploy).
 * Caller must free resp->body on return 0. */
#define ADMIN_NEEDS_PROXY 1
int admin_dispatch(const admin_services_t *svc, const admin_request_t *req,
                   admin_response_t *resp);
