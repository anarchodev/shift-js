#include "admin.h"
#include "code_db.h"
#include "code_store.h"
#include "code_server.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ======================================================================
 * Helpers
 * ====================================================================== */

static char *json_error(const char *msg) {
    char *buf = malloc(256);
    if (!buf) return NULL;
    int n = snprintf(buf, 256, "{\"error\":\"%s\"}", msg);
    if (n < 0) { free(buf); return NULL; }
    return buf;
}

/* Extract query parameter value from path.  Returns malloc'd string or NULL. */
static char *query_param(const char *path, const char *name) {
    const char *qs = strchr(path, '?');
    if (!qs) return NULL;
    qs++;

    size_t nlen = strlen(name);
    const char *p = qs;
    while (*p) {
        if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            const char *val = p + nlen + 1;
            const char *end = strchr(val, '&');
            size_t vlen = end ? (size_t)(end - val) : strlen(val);
            char *out = malloc(vlen + 1);
            if (!out) return NULL;
            memcpy(out, val, vlen);
            out[vlen] = '\0';
            return out;
        }
        const char *amp = strchr(p, '&');
        if (!amp) break;
        p = amp + 1;
    }
    return NULL;
}

/* Match path prefix, ignoring query string. */
static int path_starts_with(const char *path, const char *prefix) {
    size_t plen = strlen(prefix);
    if (strncmp(path, prefix, plen) != 0) return 0;
    char c = path[plen];
    return c == '\0' || c == '?' || c == '/';
}

static void resp_json(admin_response_t *resp, uint16_t status,
                      char *body, uint32_t body_len) {
    resp->status       = status;
    resp->body         = body;
    resp->body_len     = body_len;
    resp->content_type = "application/json; charset=utf-8";
}

static void resp_error(admin_response_t *resp, uint16_t status,
                       const char *msg) {
    char *body = json_error(msg);
    resp_json(resp, status, body, body ? (uint32_t)strlen(body) : 0);
}

/* ======================================================================
 * /_admin/kv — KV get, put, delete, range
 * ====================================================================== */

static void handle_kv_get(kvstore_t *kv, const admin_request_t *req,
                          admin_response_t *resp) {
    char *key = query_param(req->path, "key");
    if (!key) {
        resp_error(resp, 400, "missing key parameter");
        return;
    }

    char key_buf[512];
    const char *actual_key = kv_prefixed_key(req->kv_prefix, key,
                                              key_buf, sizeof(key_buf));
    if (!actual_key) {
        resp_error(resp, 400, "key too long");
        free(key);
        return;
    }

    void *value = NULL;
    size_t vlen = 0;
    int rc = kv_get(kv, actual_key, &value, &vlen);
    free(key);

    if (rc != 0) {
        resp_error(resp, 404, "not found");
        return;
    }

    /* Return raw value with content-type based on content */
    resp->status       = 200;
    resp->body         = value;
    resp->body_len     = (uint32_t)vlen;
    resp->content_type = "application/octet-stream";
}

static void handle_kv_put(kvstore_t *kv, const admin_request_t *req,
                          admin_response_t *resp) {
    char *key = query_param(req->path, "key");
    if (!key) {
        resp_error(resp, 400, "missing key parameter");
        return;
    }

    if (!req->body || req->body_len == 0) {
        resp_error(resp, 400, "missing request body");
        free(key);
        return;
    }

    char key_buf[512];
    const char *actual_key = kv_prefixed_key(req->kv_prefix, key,
                                              key_buf, sizeof(key_buf));
    if (!actual_key) {
        resp_error(resp, 400, "key too long");
        free(key);
        return;
    }

    int rc = kv_put(kv, actual_key, req->body, req->body_len);
    free(key);

    if (rc != 0) {
        resp_error(resp, 500, "write failed");
        return;
    }

    char *body = strdup("{\"ok\":true}");
    resp_json(resp, 200, body, body ? 11 : 0);
}

static void handle_kv_delete(kvstore_t *kv, const admin_request_t *req,
                             admin_response_t *resp) {
    char *key = query_param(req->path, "key");
    if (!key) {
        resp_error(resp, 400, "missing key parameter");
        return;
    }

    char key_buf[512];
    const char *actual_key = kv_prefixed_key(req->kv_prefix, key,
                                              key_buf, sizeof(key_buf));
    if (!actual_key) {
        resp_error(resp, 400, "key too long");
        free(key);
        return;
    }

    int rc = kv_delete(kv, actual_key);
    free(key);

    if (rc != 0) {
        resp_error(resp, 500, "delete failed");
        return;
    }

    char *body = strdup("{\"ok\":true}");
    resp_json(resp, 200, body, body ? 11 : 0);
}

static void handle_kv_range(kvstore_t *kv, const admin_request_t *req,
                            admin_response_t *resp) {
    char *start = query_param(req->path, "start");
    char *end   = query_param(req->path, "end");
    if (!start || !end) {
        resp_error(resp, 400, "missing start or end parameter");
        free(start); free(end);
        return;
    }

    char *limit_str = query_param(req->path, "limit");
    size_t limit = limit_str ? (size_t)atoi(limit_str) : 100;
    free(limit_str);

    char sbuf[512], ebuf[512];
    const char *actual_start = kv_prefixed_key(req->kv_prefix, start,
                                                sbuf, sizeof(sbuf));
    const char *actual_end = kv_prefixed_key(req->kv_prefix, end,
                                              ebuf, sizeof(ebuf));
    free(start); free(end);

    if (!actual_start || !actual_end) {
        resp_error(resp, 400, "key too long");
        return;
    }

    kv_range_result_t result;
    int rc = kv_range(kv, actual_start, actual_end, limit, &result);
    if (rc != 0) {
        resp_error(resp, 500, "range query failed");
        return;
    }

    /* Build JSON array of {key, value_len} objects.
     * Strip tenant prefix from keys in output. */
    size_t pfx_len = req->kv_prefix ? strlen(req->kv_prefix) : 0;
    size_t buf_cap = 256 + result.count * 128;
    char *buf = malloc(buf_cap);
    if (!buf) {
        kv_range_free(&result);
        resp_error(resp, 500, "out of memory");
        return;
    }

    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, buf_cap - pos, "[");
    for (size_t i = 0; i < result.count; i++) {
        const char *k = result.entries[i].key;
        if (pfx_len > 0 && strncmp(k, req->kv_prefix, pfx_len) == 0)
            k += pfx_len;

        if (pos + 128 + strlen(k) > buf_cap) {
            buf_cap *= 2;
            char *nb = realloc(buf, buf_cap);
            if (!nb) break;
            buf = nb;
        }
        pos += (size_t)snprintf(buf + pos, buf_cap - pos,
                                "%s{\"key\":\"%s\",\"size\":%zu}",
                                i > 0 ? "," : "", k,
                                result.entries[i].value_len);
    }
    pos += (size_t)snprintf(buf + pos, buf_cap - pos, "]");

    kv_range_free(&result);
    resp_json(resp, 200, buf, (uint32_t)pos);
}

static void handle_kv(const admin_services_t *svc, const admin_request_t *req,
                      admin_response_t *resp) {
    kvstore_t *kv = svc->kv;
    if (path_starts_with(req->path, "/_admin/kv/range")) {
        handle_kv_range(kv, req, resp);
    } else if (strcmp(req->method, "GET") == 0) {
        handle_kv_get(kv, req, resp);
    } else if (strcmp(req->method, "PUT") == 0 ||
               strcmp(req->method, "POST") == 0) {
        handle_kv_put(kv, req, resp);
    } else if (strcmp(req->method, "DELETE") == 0) {
        handle_kv_delete(kv, req, resp);
    } else {
        resp_error(resp, 405, "method not allowed");
    }
}

/* ======================================================================
 * /_admin/domain — domain mapping
 * ====================================================================== */

static void handle_domain(const admin_services_t *svc, const admin_request_t *req,
                          admin_response_t *resp) {
    kvstore_t *kv = svc->kv;
    if (strcmp(req->method, "PUT") == 0 ||
        strcmp(req->method, "POST") == 0) {
        char *host = query_param(req->path, "host");
        char *id   = query_param(req->path, "id");
        if (!host || !id) {
            resp_error(resp, 400, "missing host or id parameter");
            free(host); free(id);
            return;
        }

        char key[256];
        snprintf(key, sizeof(key), "__domains/%s", host);
        int rc = kv_put(kv, key, id, strlen(id));
        free(host); free(id);

        if (rc != 0) {
            resp_error(resp, 500, "write failed");
            return;
        }
        char *body = strdup("{\"ok\":true}");
        resp_json(resp, 200, body, body ? 11 : 0);

    } else if (strcmp(req->method, "DELETE") == 0) {
        char *host = query_param(req->path, "host");
        if (!host) {
            resp_error(resp, 400, "missing host parameter");
            return;
        }

        char key[256];
        snprintf(key, sizeof(key), "__domains/%s", host);
        kv_delete(kv, key);
        free(host);

        char *body = strdup("{\"ok\":true}");
        resp_json(resp, 200, body, body ? 11 : 0);

    } else {
        resp_error(resp, 405, "method not allowed");
    }
}

/* ======================================================================
 * /_admin/cert — TLS certificate storage
 * ====================================================================== */

static void handle_cert(const admin_services_t *svc, const admin_request_t *req,
                        admin_response_t *resp) {
    kvstore_t *kv = svc->kv;
    if (strcmp(req->method, "PUT") != 0 &&
        strcmp(req->method, "POST") != 0) {
        resp_error(resp, 405, "method not allowed");
        return;
    }

    char *name = query_param(req->path, "name");
    char *type = query_param(req->path, "type"); /* "cert" or "key" */
    if (!name || !type) {
        resp_error(resp, 400, "missing name or type parameter");
        free(name); free(type);
        return;
    }

    if (strcmp(type, "cert") != 0 && strcmp(type, "key") != 0) {
        resp_error(resp, 400, "type must be cert or key");
        free(name); free(type);
        return;
    }

    if (!req->body || req->body_len == 0) {
        resp_error(resp, 400, "missing request body (PEM data)");
        free(name); free(type);
        return;
    }

    char key[256];
    snprintf(key, sizeof(key), "__certs/%s/%s", name, type);
    int rc = kv_put(kv, key, req->body, req->body_len);
    free(name); free(type);

    if (rc != 0) {
        resp_error(resp, 500, "write failed");
        return;
    }

    char *body = strdup("{\"ok\":true}");
    resp_json(resp, 200, body, body ? 11 : 0);
}

/* ======================================================================
 * Public API
 * ====================================================================== */

int admin_match(const char *path, uint32_t path_len) {
    if (path_len >= 8 && strncmp(path, "/_admin/", 8) == 0) return 1;
    if (path_len >= 7 && strncmp(path, "/upload", 7) == 0) return 1;
    if (path_len >= 7 && strncmp(path, "/deploy", 7) == 0) return 1;
    return 0;
}

/* ======================================================================
 * /_admin/code — upload and deploy
 * ====================================================================== */

static int handle_code_upload(const admin_services_t *svc,
                              const admin_request_t *req,
                              admin_response_t *resp) {
    if (!svc->code_db) return ADMIN_NEEDS_PROXY;
    char *path = query_param(req->path, "path");
    if (!path) {
        resp_error(resp, 400, "missing path parameter");
        return 0;
    }
    if (!req->body || req->body_len == 0) {
        resp_error(resp, 400, "missing request body");
        free(path);
        return 0;
    }

    int rc = code_db_put_file(svc->code_db, "default",
                              path, req->body, req->body_len);
    free(path);

    if (rc != 0) {
        resp_error(resp, 500, "upload failed");
        return 0;
    }
    char *body = strdup("{\"ok\":true}");
    resp_json(resp, 200, body, body ? 11 : 0);
    return 0;
}

static int handle_code_deploy(const admin_services_t *svc,
                              admin_response_t *resp) {
    if (!svc->code_db || !svc->code_store) return ADMIN_NEEDS_PROXY;

    int rc = code_server_deploy(svc->code_db, "default", svc->code_store);
    if (rc != 0) {
        resp_error(resp, 500, "deploy failed");
        return 0;
    }
    char *body = strdup("{\"ok\":true}");
    resp_json(resp, 200, body, body ? 11 : 0);
    return 0;
}

static int handle_code(const admin_services_t *svc, const admin_request_t *req,
                       admin_response_t *resp) {
    if (path_starts_with(req->path, "/_admin/code/upload") ||
        path_starts_with(req->path, "/upload")) {
        if (strcmp(req->method, "PUT") == 0 || strcmp(req->method, "POST") == 0)
            return handle_code_upload(svc, req, resp);
        resp_error(resp, 405, "method not allowed");
    } else if (path_starts_with(req->path, "/_admin/code/deploy") ||
               path_starts_with(req->path, "/deploy")) {
        if (strcmp(req->method, "POST") == 0)
            return handle_code_deploy(svc, resp);
        resp_error(resp, 405, "method not allowed");
    } else {
        resp_error(resp, 404, "unknown code endpoint");
    }
    return 0;
}

/* ======================================================================
 * Public dispatch
 * ====================================================================== */

int admin_dispatch(const admin_services_t *svc, const admin_request_t *req,
                   admin_response_t *resp) {
    memset(resp, 0, sizeof(*resp));

    if (path_starts_with(req->path, "/_admin/kv")) {
        handle_kv(svc, req, resp);
    } else if (path_starts_with(req->path, "/_admin/code") ||
               path_starts_with(req->path, "/upload") ||
               path_starts_with(req->path, "/deploy")) {
        return handle_code(svc, req, resp);
    } else if (path_starts_with(req->path, "/_admin/domain")) {
        handle_domain(svc, req, resp);
    } else if (path_starts_with(req->path, "/_admin/cert")) {
        handle_cert(svc, req, resp);
    } else {
        resp_error(resp, 404, "unknown admin endpoint");
    }

    return 0;
}
