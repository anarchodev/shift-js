#define _GNU_SOURCE

#include "code_client.h"
#include "code_store.h"

#include <shift_h2.h>
#include <shift.h>

#include <arpa/inet.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* ======================================================================
 * HTTP/2 client (shift entity system)
 * ====================================================================== */

typedef struct {
    shift_t             *sh;
    sh2_context_t       *ctx;
    sh2_component_ids_t  comp;

    shift_collection_id_t request_out;
    shift_collection_id_t response_in;
    shift_collection_id_t response_out;
    shift_collection_id_t conn_close_out;

    shift_collection_id_t connect_in;
    shift_collection_id_t connect_out;
    shift_collection_id_t disconnect_in;
    shift_collection_id_t cli_conn_close_out;
    shift_collection_id_t client_request_in;
    shift_collection_id_t client_cancel_in;
    shift_collection_id_t client_response_out;

    shift_entity_t session_entity;
    bool           connected;
} h2_client_t;

static int h2_client_init(h2_client_t *c) {
    memset(c, 0, sizeof(*c));

    shift_config_t sh_cfg = {
        .max_entities = 4096, .max_components = 32,
        .max_collections = 64, .deferred_queue_capacity = 4096,
    };
    if (shift_context_create(&sh_cfg, &c->sh) != shift_ok) return -1;

    if (sh2_register_components(c->sh, &c->comp) != sh2_ok) {
        shift_context_destroy(c->sh);
        return -1;
    }

    shift_component_id_t all[] = {
        c->comp.stream_id, c->comp.session, c->comp.req_headers,
        c->comp.req_body, c->comp.resp_headers, c->comp.resp_body,
        c->comp.status, c->comp.io_result, c->comp.domain_tag,
        c->comp.peer_cert,
    };
    uint32_t nall = sizeof(all) / sizeof(all[0]);
    shift_component_id_t conn[] = {
        c->comp.connect_target, c->comp.session, c->comp.io_result,
    };

    shift_collection_info_t ci;
#define REG(name_str, ids, n, out) \
    ci = (shift_collection_info_t){ .name = name_str, .comp_ids = ids, .comp_count = n }; \
    shift_collection_register(c->sh, &ci, &c->out)

    REG("request_out",              all,  nall, request_out);
    REG("response_in",              all,  nall, response_in);
    REG("response_out",             all,  nall, response_out);
    /* connection_close_out not in server config — skip */
    REG("connect_in",               conn, 3,    connect_in);
    REG("connect_out",              all,  nall, connect_out);
    REG("disconnect_in",            all,  nall, disconnect_in);
    REG("cli_connection_close_out", all,  nall, cli_conn_close_out);
    REG("client_request_in",        all,  nall, client_request_in);
    REG("client_cancel_in",         all,  nall, client_cancel_in);
    REG("client_response_out",      all,  nall, client_response_out);
#undef REG

    sh2_config_t cfg = {
        .shift = c->sh, .comp_ids = c->comp,
        .max_connections = 4, .ring_entries = 64,
        .buf_count = 64, .buf_size = 64 * 1024,
        .request_out = c->request_out,
        .response_in = c->response_in,
        .response_out = c->response_out,
        .enable_connect = true,
        .client_colls = {
            .connect_in          = c->connect_in,
            .connect_out         = c->connect_out,
            .disconnect_in       = c->disconnect_in,
            .connection_close_out = c->cli_conn_close_out,
            .request_in          = c->client_request_in,
            .cancel_in           = c->client_cancel_in,
            .response_out        = c->client_response_out,
        },
    };

    if (sh2_context_create(&cfg, &c->ctx) != sh2_ok) {
        shift_context_destroy(c->sh);
        return -1;
    }
    return 0;
}

static int h2_client_connect(h2_client_t *c, const char *host, uint16_t port) {
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    inet_pton(AF_INET, host, &addr.sin_addr);

    shift_entity_t ce;
    shift_entity_create_one_begin(c->sh, c->connect_in, &ce);
    sh2_connect_target_t *tgt = NULL;
    shift_entity_get_component(c->sh, ce, c->comp.connect_target, (void **)&tgt);
    tgt->addr         = addr;
    tgt->hostname     = host;
    tgt->hostname_len = (uint32_t)strlen(host);
    shift_entity_create_one_end(c->sh, ce);

    while (!c->connected) {
        if (sh2_poll(c->ctx, 0) != sh2_ok) return -1;

        shift_entity_t *entities = NULL;
        size_t count = 0;
        shift_collection_get_entities(c->sh, c->connect_out,
                                      &entities, &count);
        for (size_t i = 0; i < count; i++) {
            sh2_io_result_t *io = NULL;
            shift_entity_get_component(c->sh, entities[i], c->comp.io_result,
                                       (void **)&io);
            if (io && io->error == 0) {
                sh2_session_t *sess = NULL;
                shift_entity_get_component(c->sh, entities[i], c->comp.session,
                                           (void **)&sess);
                c->session_entity = sess->entity;
                c->connected = true;
            } else {
                shift_entity_destroy_one(c->sh, entities[i]);
                shift_flush(c->sh);
                return -1;
            }
            shift_entity_destroy_one(c->sh, entities[i]);
        }
        shift_flush(c->sh);
    }
    return 0;
}

typedef struct {
    uint16_t  status;
    char     *body;
    size_t    body_len;
} h2_response_t;

static int h2_client_get(h2_client_t *c, const char *host, const char *path,
                         h2_response_t *resp) {
    memset(resp, 0, sizeof(*resp));

    sh2_header_field_t *fields = calloc(4, sizeof(sh2_header_field_t));
    fields[0] = (sh2_header_field_t){ .name = ":method", .name_len = 7,
                                      .value = "GET", .value_len = 3 };
    fields[1] = (sh2_header_field_t){ .name = ":path", .name_len = 5,
                                      .value = path, .value_len = (uint32_t)strlen(path) };
    fields[2] = (sh2_header_field_t){ .name = ":scheme", .name_len = 7,
                                      .value = "http", .value_len = 4 };
    fields[3] = (sh2_header_field_t){ .name = ":authority", .name_len = 10,
                                      .value = host, .value_len = (uint32_t)strlen(host) };

    shift_entity_t re;
    shift_entity_create_one_begin(c->sh, c->client_request_in, &re);

    sh2_session_t *rsess = NULL;
    shift_entity_get_component(c->sh, re, c->comp.session, (void **)&rsess);
    rsess->entity = c->session_entity;

    sh2_req_headers_t *rh = NULL;
    shift_entity_get_component(c->sh, re, c->comp.req_headers, (void **)&rh);
    rh->fields = fields;
    rh->count  = 4;

    shift_entity_create_one_end(c->sh, re);

    bool done = false;
    while (!done) {
        if (sh2_poll(c->ctx, 0) != sh2_ok) return -1;

        shift_entity_t *entities = NULL;
        size_t count = 0;
        shift_collection_get_entities(c->sh, c->client_response_out,
                                      &entities, &count);
        for (size_t i = 0; i < count; i++) {
            sh2_status_t *st = NULL;
            shift_entity_get_component(c->sh, entities[i], c->comp.status,
                                       (void **)&st);
            sh2_resp_body_t *rb = NULL;
            shift_entity_get_component(c->sh, entities[i], c->comp.resp_body,
                                       (void **)&rb);

            resp->status = st ? st->code : 0;
            if (rb && rb->data && rb->len > 0) {
                resp->body = malloc(rb->len + 1);
                memcpy(resp->body, rb->data, rb->len);
                resp->body[rb->len] = '\0';
                resp->body_len = rb->len;
            }

            shift_entity_destroy_one(c->sh, entities[i]);
            done = true;
        }
        shift_flush(c->sh);
    }
    return 0;
}

static void h2_client_destroy(h2_client_t *c) {
    if (c->ctx) sh2_context_destroy(c->ctx);
    if (c->sh) {
        shift_flush(c->sh);
        shift_context_destroy(c->sh);
    }
}

/* ======================================================================
 * Simple JSON parsing helpers (for code server responses)
 * ====================================================================== */

/* Extract a string value for a key from a flat JSON object.
 * Returns malloc'd string or NULL. */
static char *json_get_string(const char *json, const char *key) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *start = strstr(json, pattern);
    if (!start) return NULL;
    start += strlen(pattern);
    const char *end = strchr(start, '"');
    if (!end) return NULL;
    size_t len = (size_t)(end - start);
    char *out = malloc(len + 1);
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

/* ======================================================================
 * Code client thread
 * ====================================================================== */

struct code_client {
    pthread_t             thread;
    code_client_config_t  cfg;
    h2_client_t           h2;
    bool                  initial_done;
    pthread_mutex_t       init_mutex;
    pthread_cond_t        init_cond;
    int                   init_result;
};

/* Parse tree JSON array and fetch blobs to populate code_store.
 * Tree format: [{"path":"foo.mjs","sha1":"abc..."},...]  */
static int pull_tree(code_client_t *cc, const char *tree_json,
                     const char *tree_hash) {
    code_store_update_begin(cc->cfg.store);
    code_store_update_set_hash(cc->cfg.store, tree_hash);

    /* Simple parse — find each {"path":"...","sha1":"..."} */
    const char *p = tree_json;
    int count = 0, errors = 0;

    while ((p = strstr(p, "\"path\":\"")) != NULL) {
        p += 8; /* skip "path":" */
        const char *path_end = strchr(p, '"');
        if (!path_end) break;
        size_t path_len = (size_t)(path_end - p);
        char *path = malloc(path_len + 1);
        memcpy(path, p, path_len);
        path[path_len] = '\0';
        p = path_end + 1;

        /* Find sha1 */
        const char *sha1_start = strstr(p, "\"sha1\":\"");
        if (!sha1_start) { free(path); break; }
        sha1_start += 8;
        char sha1[41] = {0};
        memcpy(sha1, sha1_start, 40);

        /* Fetch compiled bytecode from code server */
        char blob_path[512];
        snprintf(blob_path, sizeof(blob_path), "/bytecode/%s", path);

        h2_response_t resp;
        if (h2_client_get(&cc->h2, cc->cfg.host, blob_path, &resp) == 0 &&
            resp.status == 200 && resp.body) {
            code_store_update_put(cc->cfg.store, path,
                                  resp.body, resp.body_len, sha1);
            count++;
        } else {
            fprintf(stderr, "code_client: failed to fetch blob %s for %s\n",
                    sha1, path);
            errors++;
        }
        free(resp.body);
        free(path);
    }

    code_store_update_end(cc->cfg.store);
    printf("code_client: pulled %d entries (%d errors)\n", count, errors);
    return errors > 0 ? -1 : 0;
}

static void *code_client_thread(void *arg) {
    code_client_t *cc = arg;

    /* Connect to code server */
    if (h2_client_init(&cc->h2) != 0 ||
        h2_client_connect(&cc->h2, cc->cfg.host, cc->cfg.port) != 0) {
        fprintf(stderr, "code_client: failed to connect to %s:%d\n",
                cc->cfg.host, cc->cfg.port);
        pthread_mutex_lock(&cc->init_mutex);
        cc->init_result = -1;
        cc->initial_done = true;
        pthread_cond_signal(&cc->init_cond);
        pthread_mutex_unlock(&cc->init_mutex);
        return NULL;
    }

    printf("code_client: connected to %s:%d\n", cc->cfg.host, cc->cfg.port);

    /* Pull initial snapshot */
    h2_response_t snap_resp;
    if (h2_client_get(&cc->h2, cc->cfg.host, "/snapshot", &snap_resp) != 0 ||
        snap_resp.status != 200) {
        fprintf(stderr, "code_client: failed to get snapshot\n");
        free(snap_resp.body);
        pthread_mutex_lock(&cc->init_mutex);
        cc->init_result = -1;
        cc->initial_done = true;
        pthread_cond_signal(&cc->init_cond);
        pthread_mutex_unlock(&cc->init_mutex);
        h2_client_destroy(&cc->h2);
        return NULL;
    }

    char *tree_hash = json_get_string(snap_resp.body, "tree_hash");
    free(snap_resp.body);

    int rc = 0;
    if (tree_hash && tree_hash[0]) {
        /* Fetch tree listing */
        h2_response_t tree_resp;
        if (h2_client_get(&cc->h2, cc->cfg.host, "/tree", &tree_resp) == 0 &&
            tree_resp.status == 200 && tree_resp.body) {
            rc = pull_tree(cc, tree_resp.body, tree_hash);
        } else {
            fprintf(stderr, "code_client: failed to get tree\n");
            rc = -1;
        }
        free(tree_resp.body);
    } else {
        printf("code_client: no deployment on code server\n");
    }
    free(tree_hash);

    /* Signal main thread that initial pull is done */
    pthread_mutex_lock(&cc->init_mutex);
    cc->init_result = rc;
    cc->initial_done = true;
    pthread_cond_signal(&cc->init_cond);
    pthread_mutex_unlock(&cc->init_mutex);

    /* Poll for deploy changes (long-poll on /snapshot) */
    char last_hash[41] = {0};
    const char *h = code_store_tree_hash(cc->cfg.store);
    if (h) snprintf(last_hash, sizeof(last_hash), "%s", h);

    while (*cc->cfg.running) {
        sleep(2); /* Poll interval */

        h2_response_t resp;
        if (h2_client_get(&cc->h2, cc->cfg.host, "/snapshot", &resp) != 0) {
            fprintf(stderr, "code_client: snapshot poll failed, reconnecting...\n");
            h2_client_destroy(&cc->h2);
            sleep(1);
            if (h2_client_init(&cc->h2) != 0 ||
                h2_client_connect(&cc->h2, cc->cfg.host, cc->cfg.port) != 0) {
                fprintf(stderr, "code_client: reconnect failed\n");
                continue;
            }
            printf("code_client: reconnected\n");
            continue;
        }

        if (resp.status != 200 || !resp.body) {
            free(resp.body);
            continue;
        }

        char *new_hash = json_get_string(resp.body, "tree_hash");
        free(resp.body);

        if (new_hash && new_hash[0] && strcmp(new_hash, last_hash) != 0) {
            printf("code_client: new deployment detected: %s\n", new_hash);

            h2_response_t tree_resp;
            if (h2_client_get(&cc->h2, cc->cfg.host, "/tree", &tree_resp) == 0 &&
                tree_resp.status == 200 && tree_resp.body) {
                pull_tree(cc, tree_resp.body, new_hash);
                snprintf(last_hash, sizeof(last_hash), "%s", new_hash);
            }
            free(tree_resp.body);
        }
        free(new_hash);
    }

    h2_client_destroy(&cc->h2);
    return NULL;
}

/* ======================================================================
 * Public API
 * ====================================================================== */

int code_client_start(const code_client_config_t *cfg,
                      code_client_t **out) {
    code_client_t *cc = calloc(1, sizeof(*cc));
    if (!cc) return -1;

    cc->cfg = *cfg;
    pthread_mutex_init(&cc->init_mutex, NULL);
    pthread_cond_init(&cc->init_cond, NULL);

    if (pthread_create(&cc->thread, NULL, code_client_thread, cc) != 0) {
        free(cc);
        return -1;
    }

    /* Wait for initial snapshot pull to complete */
    pthread_mutex_lock(&cc->init_mutex);
    while (!cc->initial_done)
        pthread_cond_wait(&cc->init_cond, &cc->init_mutex);
    int result = cc->init_result;
    pthread_mutex_unlock(&cc->init_mutex);

    if (result != 0) {
        pthread_join(cc->thread, NULL);
        pthread_mutex_destroy(&cc->init_mutex);
        pthread_cond_destroy(&cc->init_cond);
        free(cc);
        return -1;
    }

    *out = cc;
    return 0;
}

void code_client_stop(code_client_t *client) {
    if (!client) return;
    pthread_join(client->thread, NULL);
    pthread_mutex_destroy(&client->init_mutex);
    pthread_cond_destroy(&client->init_cond);
    free(client);
}
