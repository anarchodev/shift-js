#include "http_api.h"

#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================
 * Helpers
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

static void sha256_hex(const void *data, size_t len, char out[65]) {
    unsigned char hash[32];
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(mdctx, data, len);
    EVP_DigestFinal_ex(mdctx, hash, NULL);
    EVP_MD_CTX_free(mdctx);
    for (int i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", hash[i]);
    out[64] = '\0';
}

/* Simple response helpers */

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
        rb->data = NULL;
        rb->len = 0;
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
        rh->fields = NULL;
        rh->count = 0;
    }

    shift_entity_move_one(r->sh, r->e, r->response_in);
}

static void respond_str(resp_ctx_t *r, int status, const char *ct,
                         const char *body) {
    respond(r, status, ct, body, body ? strlen(body) : 0);
}

static void respond_json(resp_ctx_t *r, int status, const char *json) {
    respond_str(r, status, "application/json", json);
}

static void respond_error(resp_ctx_t *r, int status, const char *msg) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    respond_json(r, status, buf);
}

/* Extract cookie value from cookie header. Returns malloc'd string or NULL. */
static char *extract_cookie(const char *cookie_header, uint32_t header_len,
                             const char *name) {
    size_t nlen = strlen(name);
    const char *p = cookie_header;
    const char *end = cookie_header + header_len;

    while (p < end) {
        /* Skip whitespace */
        while (p < end && (*p == ' ' || *p == ';')) p++;
        if (p + nlen + 1 >= end) break;
        if (memcmp(p, name, nlen) == 0 && p[nlen] == '=') {
            p += nlen + 1;
            const char *val_end = memchr(p, ';', (size_t)(end - p));
            if (!val_end) val_end = end;
            size_t vlen = (size_t)(val_end - p);
            char *val = malloc(vlen + 1);
            memcpy(val, p, vlen);
            val[vlen] = '\0';
            return val;
        }
        /* Skip to next ; */
        while (p < end && *p != ';') p++;
    }
    return NULL;
}

/* Authenticate request. Returns 0 on success with claims populated. */
static int authenticate(code_server_ctx_t *sctx,
                         const sh2_header_field_t *fields, uint32_t count,
                         sjs_auth_claims_t *claims) {
    uint32_t cookie_len = 0;
    const char *cookie_hdr = find_header(fields, count, "cookie", &cookie_len);
    if (!cookie_hdr) return -1;

    char *token = extract_cookie(cookie_hdr, cookie_len, "_sjs_auth");
    if (!token) return -1;

    int rc = sjs_auth_verify(token, strlen(token),
                              sctx->auth_secret, sctx->auth_secret_len,
                              claims);
    free(token);
    return rc;
}

/* ======================================================================
 * Route handlers
 * ====================================================================== */

/* POST /modules — body is JSON: {"path":"...","source":"..."}
 * For simplicity, we accept raw source with path in query: ?path=foo.ts */
static void handle_put_module(code_server_ctx_t *sctx, resp_ctx_t *r,
                               int64_t tenant_id,
                               const char *path_info,
                               const char *body, size_t body_len) {
    /* path_info is everything after /modules/ */
    if (!path_info || !*path_info) {
        respond_error(r, 400, "path required");
        return;
    }
    if (!body || body_len == 0) {
        respond_error(r, 400, "body required");
        return;
    }

    /* Determine extension from path */
    const char *dot = strrchr(path_info, '.');
    if (!dot) {
        respond_error(r, 400, "path must have extension");
        return;
    }

    char hash[65];
    sha256_hex(body, body_len, hash);

    int rc = code_db_put_module(sctx->db, tenant_id, path_info, dot,
                                 body, body_len, hash);
    if (rc != 0) {
        respond_error(r, 500, "database error");
        return;
    }

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"path\":\"%s\",\"content_hash\":\"%s\"}", path_info, hash);
    respond_json(r, 200, resp);
}

/* GET /modules — list all modules */
static void handle_list_modules(code_server_ctx_t *sctx, resp_ctx_t *r,
                                 int64_t tenant_id) {
    size_t count;
    code_module_t *list = code_db_list_modules(sctx->db, tenant_id, &count);

    /* Build JSON array */
    size_t cap = 256 + count * 256;
    char *json = malloc(cap);
    size_t pos = 0;
    json[pos++] = '[';

    for (size_t i = 0; i < count; i++) {
        if (i > 0) json[pos++] = ',';
        pos += (size_t)snprintf(json + pos, cap - pos,
            "{\"path\":\"%s\",\"extension\":\"%s\",\"size\":%zu,"
            "\"content_hash\":\"%s\",\"updated_at\":%lld}",
            list[i].path, list[i].extension, list[i].source_len,
            list[i].content_hash, (long long)list[i].updated_at);
        code_module_free(&list[i]);
    }
    free(list);

    json[pos++] = ']';
    json[pos] = '\0';

    respond(r, 200, "application/json", json, pos);
    free(json);
}

/* GET /modules/:path — get module source */
static void handle_get_module(code_server_ctx_t *sctx, resp_ctx_t *r,
                               int64_t tenant_id, const char *path_info) {
    code_module_t m;
    if (code_db_get_module(sctx->db, tenant_id, path_info, &m) != 0) {
        respond_error(r, 404, "not found");
        return;
    }
    respond(r, 200, "application/octet-stream", m.source, m.source_len);
    code_module_free(&m);
}

/* DELETE /modules/:path */
static void handle_delete_module(code_server_ctx_t *sctx, resp_ctx_t *r,
                                  int64_t tenant_id, const char *path_info) {
    if (code_db_delete_module(sctx->db, tenant_id, path_info) != 0) {
        respond_error(r, 404, "not found");
        return;
    }
    respond_json(r, 200, "{\"deleted\":true}");
}

/* POST /compile — compile all modules into a tree */
static void handle_compile(code_server_ctx_t *sctx, resp_ctx_t *r,
                            int64_t tenant_id) {
    compile_tree_result_t tr = compile_tree(sctx->cc, sctx->db, tenant_id);

    if (tr.error) {
        char *resp = NULL;
        asprintf(&resp, "{\"error\":\"%s\"}", tr.error);
        respond_json(r, 400, resp);
        free(resp);
        compile_tree_result_free(&tr);
        return;
    }

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"tree_hash\":\"%s\",\"module_count\":%zu}",
             tr.tree_hash, tr.module_count);
    respond_json(r, 200, resp);
    compile_tree_result_free(&tr);
}

/* POST /push — push tree to request servers.
 * Body: {"tree_hash":"...","servers":["host:port",...]} */
static void handle_push(code_server_ctx_t *sctx, resp_ctx_t *r,
                         const char *body_raw, size_t body_len) {
    if (!body_raw || body_len == 0) {
        respond_error(r, 400, "body required");
        return;
    }
    char *body = malloc(body_len + 1);
    memcpy(body, body_raw, body_len);
    body[body_len] = '\0';

    /* Extract tree_hash */
    const char *th = strstr(body, "\"tree_hash\":\"");
    if (!th) { respond_error(r, 400, "tree_hash required"); free(body); return; }
    th += 13;
    char tree_hash[65];
    size_t i = 0;
    while (*th && *th != '"' && i < 64) tree_hash[i++] = *th++;
    tree_hash[i] = '\0';

    /* Extract servers array — simplified parsing */
    const char *sa = strstr(body, "\"servers\":[");
    if (!sa) { respond_error(r, 400, "servers required"); free(body); return; }
    sa += 11;

    /* Count and extract server strings */
    const char **servers = NULL;
    size_t server_count = 0;
    size_t server_cap = 4;
    servers = malloc(server_cap * sizeof(char *));

    const char *p = sa;
    while (*p && *p != ']') {
        if (*p == '"') {
            p++;
            const char *end = strchr(p, '"');
            if (!end) break;
            if (server_count >= server_cap) {
                server_cap *= 2;
                servers = realloc(servers, server_cap * sizeof(char *));
            }
            servers[server_count++] = strndup(p, (size_t)(end - p));
            p = end + 1;
        } else {
            p++;
        }
    }
    free(body);

    if (server_count == 0) {
        respond_error(r, 400, "no servers specified");
        free(servers);
        return;
    }

    push_result_t *results = push_tree(sctx->db, tree_hash,
                                        servers, server_count, NULL, 0);

    /* Build response */
    size_t cap = 128 + server_count * 256;
    char *json = malloc(cap);
    size_t pos = 0;
    pos += (size_t)snprintf(json, cap, "{\"tree_hash\":\"%s\",\"results\":[", tree_hash);

    for (size_t si = 0; si < server_count; si++) {
        if (si > 0) json[pos++] = ',';
        if (results[si].status == 0) {
            pos += (size_t)snprintf(json + pos, cap - pos,
                "{\"server\":\"%s\",\"status\":\"ok\"}", results[si].server);
        } else {
            pos += (size_t)snprintf(json + pos, cap - pos,
                "{\"server\":\"%s\",\"status\":\"error\",\"error\":\"%s\"}",
                results[si].server,
                results[si].error ? results[si].error : "unknown");
        }
        push_result_free(&results[si]);
    }
    free(results);

    for (size_t si = 0; si < server_count; si++) free((char *)servers[si]);
    free(servers);

    pos += (size_t)snprintf(json + pos, cap - pos, "]}");
    respond(r, 200, "application/json", json, pos);
    free(json);
}

/* GET /trees — list compiled trees */
static void handle_list_trees(code_server_ctx_t *sctx, resp_ctx_t *r,
                               int64_t tenant_id) {
    size_t count;
    code_tree_info_t *trees = code_db_list_trees(sctx->db, tenant_id, &count);

    size_t cap = 128 + count * 128;
    char *json = malloc(cap);
    size_t pos = 0;
    json[pos++] = '[';

    for (size_t i = 0; i < count; i++) {
        if (i > 0) json[pos++] = ',';
        pos += (size_t)snprintf(json + pos, cap - pos,
            "{\"hash\":\"%s\",\"created_at\":%lld,\"module_count\":%d}",
            trees[i].hash, (long long)trees[i].created_at,
            trees[i].module_count);
        free(trees[i].hash);
    }
    free(trees);

    json[pos++] = ']';
    json[pos] = '\0';
    respond(r, 200, "application/json", json, pos);
    free(json);
}

/* GET /trees/:hash — tree details with module list */
static void handle_get_tree(code_server_ctx_t *sctx, resp_ctx_t *r,
                             const char *tree_hash) {
    size_t count;
    code_tree_module_t *tms = code_db_get_tree_modules(sctx->db, tree_hash,
                                                         &count);
    if (!tms || count == 0) {
        respond_error(r, 404, "tree not found");
        free(tms);
        return;
    }

    size_t cap = 128 + count * 192;
    char *json = malloc(cap);
    size_t pos = 0;
    pos += (size_t)snprintf(json, cap,
        "{\"hash\":\"%s\",\"modules\":[", tree_hash);

    for (size_t i = 0; i < count; i++) {
        if (i > 0) json[pos++] = ',';
        pos += (size_t)snprintf(json + pos, cap - pos,
            "{\"module_path\":\"%s\",\"content_hash\":\"%s\","
            "\"bytecode_len\":%zu,\"has_source_map\":%s}",
            tms[i].module_path, tms[i].content_hash, tms[i].bytecode_len,
            tms[i].source_map ? "true" : "false");
        code_tree_module_free(&tms[i]);
    }
    free(tms);

    pos += (size_t)snprintf(json + pos, cap - pos, "]}");
    respond(r, 200, "application/json", json, pos);
    free(json);
}

/* GET /source_blobs/:content_hash */
static void handle_get_blob(code_server_ctx_t *sctx, resp_ctx_t *r,
                             const char *content_hash) {
    size_t len;
    char *ext = NULL;
    void *blob = code_db_get_blob(sctx->db, content_hash, &len, &ext);
    if (!blob) {
        respond_error(r, 404, "blob not found");
        return;
    }
    respond(r, 200, "application/octet-stream", blob, len);
    free(blob);
    free(ext);
}

/* GET /sourcemaps/:path?tree_hash=X */
static void handle_get_sourcemap(code_server_ctx_t *sctx, resp_ctx_t *r,
                                  const char *module_path,
                                  const char *query_string) {
    /* Extract tree_hash from query string */
    const char *th = query_string ? strstr(query_string, "tree_hash=") : NULL;
    if (!th) {
        respond_error(r, 400, "tree_hash query param required");
        return;
    }
    th += 10; /* skip "tree_hash=" */
    char tree_hash[65];
    size_t i = 0;
    while (*th && *th != '&' && i < 64)
        tree_hash[i++] = *th++;
    tree_hash[i] = '\0';

    char *sm = code_db_get_sourcemap(sctx->db, tree_hash, module_path);
    if (!sm) {
        respond_error(r, 404, "source map not found");
        return;
    }
    respond_str(r, 200, "application/json", sm);
    free(sm);
}

/* ======================================================================
 * Router
 * ====================================================================== */

/* Match path prefix, return pointer to remainder (after prefix + optional /).
 * Returns NULL if no match. */
static const char *match_prefix(const char *path, const char *prefix) {
    size_t plen = strlen(prefix);
    if (strncmp(path, prefix, plen) != 0) return NULL;
    if (path[plen] == '/') return path + plen + 1;
    if (path[plen] == '\0') return path + plen;
    if (path[plen] == '?') return path + plen;
    return NULL;
}

void code_server_handle_request(
    code_server_ctx_t *sctx,
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

    /* Split query string */
    char *query = strchr(path, '?');
    if (query) *query++ = '\0';

    /* Authenticate */
    sjs_auth_claims_t claims = {0};
    if (authenticate(sctx, rqh->fields, rqh->count, &claims) != 0) {
        respond_error(&r, 401, "unauthorized");
        free(method);
        free(path);
        return;
    }
    int64_t tenant_id = claims.tid;

    /* Route */
    const char *rest;
    bool handled = false;

    if ((rest = match_prefix(path, "/modules")) != NULL) {
        if (*rest == '\0') {
            /* /modules */
            if (strcmp(method, "GET") == 0) {
                handle_list_modules(sctx, &r, tenant_id);
                handled = true;
            }
        } else if (*rest != '\0') {
            /* /modules/:path */
            if (strcmp(method, "GET") == 0) {
                handle_get_module(sctx, &r, tenant_id, rest);
                handled = true;
            } else if (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) {
                handle_put_module(sctx, &r, tenant_id, rest,
                                   rqb->data, rqb->len);
                handled = true;
            } else if (strcmp(method, "DELETE") == 0) {
                handle_delete_module(sctx, &r, tenant_id, rest);
                handled = true;
            }
        }
    } else if (strcmp(path, "/compile") == 0 && strcmp(method, "POST") == 0) {
        handle_compile(sctx, &r, tenant_id);
        handled = true;
    } else if (strcmp(path, "/push") == 0 && strcmp(method, "POST") == 0) {
        handle_push(sctx, &r, rqb->data, rqb->len);
        handled = true;
    } else if ((rest = match_prefix(path, "/trees")) != NULL) {
        if (*rest == '\0' && strcmp(method, "GET") == 0) {
            handle_list_trees(sctx, &r, tenant_id);
            handled = true;
        } else if (*rest != '\0' && strcmp(method, "GET") == 0) {
            handle_get_tree(sctx, &r, rest);
            handled = true;
        }
    } else if ((rest = match_prefix(path, "/source_blobs")) != NULL && *rest) {
        if (strcmp(method, "GET") == 0) {
            handle_get_blob(sctx, &r, rest);
            handled = true;
        }
    } else if ((rest = match_prefix(path, "/sourcemaps")) != NULL && *rest) {
        if (strcmp(method, "GET") == 0) {
            handle_get_sourcemap(sctx, &r, rest, query);
            handled = true;
        }
    }

    if (!handled) {
        respond_error(&r, 404, "not found");
    }

    sjs_auth_claims_free(&claims);
    free(method);
    free(path);
}
