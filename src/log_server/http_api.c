#include "http_api.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================
 * Helpers (same pattern as code server)
 * ====================================================================== */

static const char *find_header(const sh2_header_field_t *fields, uint32_t count,
                                const char *name, uint32_t *out_len) {
    size_t nlen = strlen(name);
    for (uint32_t i = 0; i < count; i++) {
        if (fields[i].name_len == nlen &&
            memcmp(fields[i].name, name, nlen) == 0) {
            if (out_len) *out_len = fields[i].value_len;
            return fields[i].value;
        }
    }
    return NULL;
}

static char *header_to_str(const char *val, uint32_t len) {
    char *s = malloc(len + 1);
    memcpy(s, val, len);
    s[len] = '\0';
    return s;
}

typedef struct {
    shift_t              *sh;
    shift_entity_t        e;
    sh2_component_ids_t  *comp;
    shift_collection_id_t response_in;
} resp_ctx_t;

static void respond(resp_ctx_t *r, int status, const char *content_type,
                    const char *body, size_t body_len) {
    sh2_status_t *st = NULL;
    shift_entity_get_component(r->sh, r->e, r->comp->status, (void **)&st);
    st->code = (uint16_t)status;

    sh2_resp_body_t *rb = NULL;
    shift_entity_get_component(r->sh, r->e, r->comp->resp_body, (void **)&rb);
    if (body && body_len > 0) {
        rb->data = malloc(body_len);
        memcpy(rb->data, body, body_len);
        rb->len = (uint32_t)body_len;
    } else {
        rb->data = NULL; rb->len = 0;
    }

    sh2_resp_headers_t *rh = NULL;
    shift_entity_get_component(r->sh, r->e, r->comp->resp_headers, (void **)&rh);
    if (content_type) {
        rh->fields = malloc(sizeof(sh2_header_field_t));
        rh->fields[0].name = strdup("content-type");
        rh->fields[0].name_len = 12;
        rh->fields[0].value = strdup(content_type);
        rh->fields[0].value_len = (uint32_t)strlen(content_type);
        rh->count = 1;
    } else {
        rh->fields = NULL; rh->count = 0;
    }

    shift_entity_move_one(r->sh, r->e, r->response_in);
}

static void respond_json(resp_ctx_t *r, int status, const char *json) {
    respond(r, status, "application/json", json, json ? strlen(json) : 0);
}

static void respond_error(resp_ctx_t *r, int status, const char *msg) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    respond_json(r, status, buf);
}

static char *extract_cookie(const char *cookie_header, uint32_t header_len,
                             const char *name) {
    size_t nlen = strlen(name);
    const char *p = cookie_header;
    const char *end = cookie_header + header_len;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == ';')) p++;
        if (p + nlen + 1 >= end) break;
        if (memcmp(p, name, nlen) == 0 && p[nlen] == '=') {
            p += nlen + 1;
            const char *val_end = memchr(p, ';', (size_t)(end - p));
            if (!val_end) val_end = end;
            return strndup(p, (size_t)(val_end - p));
        }
        while (p < end && *p != ';') p++;
    }
    return NULL;
}

static int authenticate(log_server_ctx_t *ctx,
                         const sh2_header_field_t *fields, uint32_t count,
                         sjs_auth_claims_t *claims) {
    uint32_t cookie_len = 0;
    const char *cookie_hdr = find_header(fields, count, "cookie", &cookie_len);
    if (!cookie_hdr) return -1;
    char *token = extract_cookie(cookie_hdr, cookie_len, "_sjs_auth");
    if (!token) return -1;
    int rc = sjs_auth_verify(token, strlen(token),
                              ctx->auth_secret, ctx->auth_secret_len, claims);
    free(token);
    return rc;
}

/* Extract a query parameter value. Returns malloc'd string or NULL. */
static char *query_param(const char *qs, const char *name) {
    if (!qs) return NULL;
    size_t nlen = strlen(name);
    const char *p = qs;
    while (*p) {
        if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            p += nlen + 1;
            const char *end = strchr(p, '&');
            if (!end) end = p + strlen(p);
            return strndup(p, (size_t)(end - p));
        }
        const char *amp = strchr(p, '&');
        if (!amp) break;
        p = amp + 1;
    }
    return NULL;
}

/* ======================================================================
 * JSON helpers for parsing POST bodies
 * ====================================================================== */

/* Minimal: extract a string value for a key from a JSON object.
 * Returns malloc'd string or NULL. Only handles flat keys. */
static char *json_str(const char *json, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p += strlen(search);
    const char *end = strchr(p, '"');
    if (!end) return NULL;
    return strndup(p, (size_t)(end - p));
}

static int64_t json_int(const char *json, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    return strtoll(p + strlen(search), NULL, 10);
}

/* ======================================================================
 * Route handlers
 * ====================================================================== */

/* POST /logs — body: {"tenant_id":N,"worker_id":N,"request_id":"N",
 *                      "session_id":"...","entries":[{"timestamp":N,"level":N,"message":"..."}]} */
static void handle_post_logs(log_server_ctx_t *ctx, resp_ctx_t *r,
                              int64_t tenant_id,
                              const char *body_raw, size_t body_len) {
    if (!body_raw || body_len == 0) { respond_error(r, 400, "body required"); return; }

    /* Make null-terminated copy for string functions */
    char *body = malloc(body_len + 1);
    memcpy(body, body_raw, body_len);
    body[body_len] = '\0';

    int worker_id = (int)json_int(body, "worker_id");
    uint64_t request_id = (uint64_t)json_int(body, "request_id");
    char *session_id = json_str(body, "session_id");

    /* Parse entries array — simplified: count "message" occurrences */
    const char *p = strstr(body, "\"entries\":");
    if (!p) {
        respond_json(r, 200, "{\"ok\":true,\"inserted\":0}");
        free(session_id);
        free(body);
        return;
    }

    /* Count entries */
    size_t entry_count = 0;
    const char *scan = p;
    while ((scan = strstr(scan, "\"message\"")) != NULL) {
        entry_count++;
        scan++;
    }

    log_entry_t *entries = calloc(entry_count ? entry_count : 1,
                                   sizeof(log_entry_t));
    size_t idx = 0;

    /* Parse each entry */
    const char *entry = p;
    while ((entry = strstr(entry, "{\"timestamp\"")) != NULL ||
           (entry = strstr(entry ? entry : p, "{\"level\"")) != NULL) {
        if (!entry) break;
        if (idx >= entry_count) break;
        entries[idx].timestamp = json_int(entry, "timestamp");
        entries[idx].level = (int)json_int(entry, "level");
        entries[idx].message = json_str(entry, "message");
        idx++;
        entry++;
    }

    log_store_insert_logs(ctx->ls, tenant_id, worker_id, request_id,
                          session_id, entries, idx);

    for (size_t i = 0; i < idx; i++) free(entries[i].message);
    free(entries);
    free(session_id);

    char resp_buf[64];
    free(body);

    snprintf(resp_buf, sizeof(resp_buf), "{\"ok\":true,\"inserted\":%zu}", idx);
    respond_json(r, 200, resp_buf);
}

/* POST /replay — body is the full replay JSON */
static void handle_post_replay(log_server_ctx_t *ctx, resp_ctx_t *r,
                                int64_t tenant_id,
                                const char *body_raw, size_t body_len) {
    if (!body_raw || body_len == 0) { respond_error(r, 400, "body required"); return; }

    char *body = malloc(body_len + 1);
    memcpy(body, body_raw, body_len);
    body[body_len] = '\0';

    uint64_t request_id = (uint64_t)json_int(body, "request_id");
    char *request_data = json_str(body, "request_data");
    char *response_data = json_str(body, "response_data");
    char *kv_tape = json_str(body, "kv_tape");
    char *date_tape = json_str(body, "date_tape");
    char *math_random_tape = json_str(body, "math_random_tape");
    char *module_tree = json_str(body, "module_tree");
    char *source_maps = json_str(body, "source_maps");

    /* For the simplified version, store the raw JSON body parts.
     * A production version would parse more carefully. */
    log_store_insert_replay(ctx->ls, tenant_id, request_id,
                            request_data ? request_data : "{}",
                            response_data,
                            kv_tape ? kv_tape : "[]",
                            NULL, 0,
                            date_tape ? date_tape : "[]",
                            math_random_tape ? math_random_tape : "[]",
                            module_tree ? module_tree : "[]",
                            source_maps);

    free(request_data); free(response_data); free(kv_tape);
    free(date_tape); free(math_random_tape); free(module_tree);
    free(source_maps);
    free(body);

    respond_json(r, 200, "{\"ok\":true}");
}

/* GET /logs?request_id=X&level=Y&limit=N&before=T&after=T&session_id=S */
static void handle_query_logs(log_server_ctx_t *ctx, resp_ctx_t *r,
                               int64_t tenant_id, const char *query) {
    log_query_t q = { .tenant_id = tenant_id, .level = -1, .limit = 100 };

    char *v;
    if ((v = query_param(query, "request_id"))) { q.request_id = strtoull(v, NULL, 10); free(v); }
    if ((v = query_param(query, "session_id"))) { q.session_id = v; }
    if ((v = query_param(query, "level")))      { q.level = atoi(v); free(v); }
    if ((v = query_param(query, "before")))     { q.before = strtoll(v, NULL, 10); free(v); }
    if ((v = query_param(query, "after")))      { q.after = strtoll(v, NULL, 10); free(v); }
    if ((v = query_param(query, "limit")))      { q.limit = atoi(v); free(v); }

    size_t count;
    log_result_t *results = log_store_query(ctx->ls, &q, &count);
    free((char *)q.session_id);

    size_t cap = 128 + count * 256;
    char *json = malloc(cap);
    size_t pos = 0;
    json[pos++] = '[';

    for (size_t i = 0; i < count; i++) {
        if (i > 0) json[pos++] = ',';
        /* Ensure we have space */
        size_t needed = 256 + (results[i].message ? strlen(results[i].message) * 2 : 0);
        if (pos + needed > cap) { cap = (pos + needed) * 2; json = realloc(json, cap); }

        pos += (size_t)snprintf(json + pos, cap - pos,
            "{\"timestamp\":%" PRId64 ",\"worker_id\":%d,"
            "\"request_id\":\"%" PRIu64 "\","
            "\"session_id\":%s%s%s,"
            "\"level\":%d,\"message\":",
            results[i].timestamp, results[i].worker_id,
            results[i].request_id,
            results[i].session_id ? "\"" : "",
            results[i].session_id ? results[i].session_id : "null",
            results[i].session_id ? "\"" : "",
            results[i].level);

        /* JSON-escape the message */
        json[pos++] = '"';
        if (results[i].message) {
            for (const char *mp = results[i].message; *mp; mp++) {
                if (pos + 6 >= cap) { cap *= 2; json = realloc(json, cap); }
                switch (*mp) {
                case '"':  json[pos++] = '\\'; json[pos++] = '"'; break;
                case '\\': json[pos++] = '\\'; json[pos++] = '\\'; break;
                case '\n': json[pos++] = '\\'; json[pos++] = 'n'; break;
                case '\r': json[pos++] = '\\'; json[pos++] = 'r'; break;
                case '\t': json[pos++] = '\\'; json[pos++] = 't'; break;
                default:   json[pos++] = *mp;
                }
            }
        }
        json[pos++] = '"';
        json[pos++] = '}';

        log_result_free(&results[i]);
    }
    free(results);

    json[pos++] = ']';
    json[pos] = '\0';
    respond(r, 200, "application/json", json, pos);
    free(json);
}

/* GET /replay/:request_id */
static void handle_get_replay(log_server_ctx_t *ctx, resp_ctx_t *r,
                               const char *id_str) {
    uint64_t request_id = strtoull(id_str, NULL, 10);

    char *request_data = NULL, *response_data = NULL, *kv_tape = NULL;
    void *random_tape = NULL;
    size_t random_tape_len = 0;
    char *date_tape = NULL, *math_random_tape = NULL;
    char *module_tree = NULL, *source_maps = NULL;

    if (log_store_get_replay(ctx->ls, request_id,
                              &request_data, &response_data, &kv_tape,
                              &random_tape, &random_tape_len,
                              &date_tape, &math_random_tape,
                              &module_tree, &source_maps) != 0) {
        respond_error(r, 404, "replay not found");
        return;
    }

    /* Build response JSON */
    size_t cap = 256 + strlen(request_data) + strlen(kv_tape) +
                 strlen(date_tape) + strlen(math_random_tape) +
                 strlen(module_tree) +
                 (response_data ? strlen(response_data) : 4) +
                 (source_maps ? strlen(source_maps) : 4);
    char *json = malloc(cap);
    size_t pos = (size_t)snprintf(json, cap,
        "{\"request_id\":\"%" PRIu64 "\","
        "\"request_data\":%s,"
        "\"response_data\":%s,"
        "\"kv_tape\":%s,"
        "\"date_tape\":%s,"
        "\"math_random_tape\":%s,"
        "\"module_tree\":%s,"
        "\"source_maps\":%s}",
        request_id,
        request_data,
        response_data ? response_data : "null",
        kv_tape, date_tape, math_random_tape, module_tree,
        source_maps ? source_maps : "null");

    respond(r, 200, "application/json", json, pos);

    free(json);
    free(request_data); free(response_data); free(kv_tape);
    free(random_tape); free(date_tape); free(math_random_tape);
    free(module_tree); free(source_maps);
}

/* GET /replays?limit=N */
static void handle_list_replays(log_server_ctx_t *ctx, resp_ctx_t *r,
                                 int64_t tenant_id, const char *query) {
    int limit = 50;
    char *v = query_param(query, "limit");
    if (v) { limit = atoi(v); free(v); }

    size_t count;
    replay_summary_t *list = log_store_list_replays(ctx->ls, tenant_id,
                                                      limit, &count);

    size_t cap = 64 + count * 80;
    char *json = malloc(cap);
    size_t pos = 0;
    json[pos++] = '[';

    for (size_t i = 0; i < count; i++) {
        if (i > 0) json[pos++] = ',';
        pos += (size_t)snprintf(json + pos, cap - pos,
            "{\"request_id\":\"%" PRIu64 "\",\"created_at\":%" PRId64 "}",
            list[i].request_id, list[i].created_at);
    }
    free(list);

    json[pos++] = ']';
    json[pos] = '\0';
    respond(r, 200, "application/json", json, pos);
    free(json);
}

/* ======================================================================
 * Router
 * ====================================================================== */

void log_server_handle_request(
    log_server_ctx_t *ctx,
    shift_t *sh,
    shift_entity_t e,
    sh2_component_ids_t *comp,
    shift_collection_id_t response_in) {

    sh2_req_headers_t *rqh = NULL;
    sh2_req_body_t    *rqb = NULL;
    shift_entity_get_component(sh, e, comp->req_headers, (void **)&rqh);
    shift_entity_get_component(sh, e, comp->req_body, (void **)&rqb);

    uint32_t method_len = 0, path_len = 0;
    const char *method_raw = find_header(rqh->fields, rqh->count,
                                          ":method", &method_len);
    const char *path_raw = find_header(rqh->fields, rqh->count,
                                        ":path", &path_len);

    resp_ctx_t r = { .sh = sh, .e = e, .comp = comp,
                     .response_in = response_in };

    if (!method_raw || !path_raw) {
        respond_error(&r, 400, "bad request");
        return;
    }

    char *method = header_to_str(method_raw, method_len);
    char *path   = header_to_str(path_raw, path_len);
    char *query  = strchr(path, '?');
    if (query) *query++ = '\0';

    /* POST ingestion endpoints (service-to-service) don't require auth.
     * Tenant ID is included in the request body.
     * GET query endpoints require auth. */
    bool is_post = (strcmp(method, "POST") == 0);

    if (is_post && strcmp(path, "/logs") == 0) {
        /* Extract tenant_id from body JSON */
        char *body_copy = NULL;
        int64_t tenant_id = 0;
        if (rqb->data && rqb->len > 0) {
            body_copy = malloc(rqb->len + 1);
            memcpy(body_copy, rqb->data, rqb->len);
            body_copy[rqb->len] = '\0';
            const char *tid = strstr(body_copy, "\"tenant_id\":");
            if (tid) tenant_id = strtoll(tid + 12, NULL, 10);
            free(body_copy);
        }
        handle_post_logs(ctx, &r, tenant_id, rqb->data, rqb->len);

    } else if (is_post && strcmp(path, "/replay") == 0) {
        char *body_copy = NULL;
        int64_t tenant_id = 0;
        if (rqb->data && rqb->len > 0) {
            body_copy = malloc(rqb->len + 1);
            memcpy(body_copy, rqb->data, rqb->len);
            body_copy[rqb->len] = '\0';
            const char *tid = strstr(body_copy, "\"tenant_id\":");
            if (tid) tenant_id = strtoll(tid + 12, NULL, 10);
            free(body_copy);
        }
        handle_post_replay(ctx, &r, tenant_id, rqb->data, rqb->len);

    } else {
        /* All other endpoints require auth */
        sjs_auth_claims_t claims = {0};
        if (authenticate(ctx, rqh->fields, rqh->count, &claims) != 0) {
            respond_error(&r, 401, "unauthorized");
            free(method); free(path);
            return;
        }
        int64_t tenant_id = claims.tid;

        if (strcmp(path, "/logs") == 0 && strcmp(method, "GET") == 0) {
            handle_query_logs(ctx, &r, tenant_id, query);
        } else if (strncmp(path, "/replay/", 8) == 0 && strcmp(method, "GET") == 0) {
            handle_get_replay(ctx, &r, path + 8);
        } else if (strcmp(path, "/replays") == 0 && strcmp(method, "GET") == 0) {
            handle_list_replays(ctx, &r, tenant_id, query);
        } else {
            respond_error(&r, 404, "not found");
        }

        sjs_auth_claims_free(&claims);
    }
    free(method);
    free(path);
}
