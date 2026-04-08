#define _GNU_SOURCE

#include "worker.h"
#include "admin.h"
#include "crypto.h"
#include "kvstore.h"
#include "js_runtime.h"
#include "router.h"
#include "raft_thread.h"

#include <shift_h2.h>
#include <shift.h>

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdatomic.h>
#include <time.h>

/* Static file extension → content-type mapping */
typedef struct {
    const char *ext;
    const char *content_type;
} static_mime_t;

static const static_mime_t STATIC_MIMES[] = {
    { ".js",    "application/javascript; charset=utf-8" },
    { ".mjs",   "application/javascript; charset=utf-8" },
    { ".css",   "text/css; charset=utf-8" },
    { ".html",  "text/html; charset=utf-8" },
    { ".json",  "application/json; charset=utf-8" },
    { ".svg",   "image/svg+xml; charset=utf-8" },
    { ".png",   "image/png" },
    { ".jpg",   "image/jpeg" },
    { ".jpeg",  "image/jpeg" },
    { ".gif",   "image/gif" },
    { ".ico",   "image/x-icon" },
    { ".woff",  "font/woff" },
    { ".woff2", "font/woff2" },
    { ".ttf",   "font/ttf" },
    { ".map",   "application/json; charset=utf-8" },
    { ".xml",   "application/xml; charset=utf-8" },
    { ".txt",   "text/plain; charset=utf-8" },
    { ".wasm",  "application/wasm" },
};

static const size_t STATIC_MIME_COUNT =
    sizeof(STATIC_MIMES) / sizeof(STATIC_MIMES[0]);

/* Check if a URL path has a static file extension. Returns the content-type
 * if it matches, or NULL if it's not a static file path. */
static const char *static_content_type(const char *path, uint32_t path_len) {
    /* Find the last '.' in the path (ignoring query string) */
    const char *qmark = memchr(path, '?', path_len);
    uint32_t check_len = qmark ? (uint32_t)(qmark - path) : path_len;

    const char *dot = NULL;
    for (int i = (int)check_len - 1; i >= 0; i--) {
        if (path[i] == '.') { dot = &path[i]; break; }
        if (path[i] == '/') break;
    }
    if (!dot) return NULL;

    size_t ext_len = (size_t)(&path[check_len] - dot);
    for (size_t i = 0; i < STATIC_MIME_COUNT; i++) {
        if (strlen(STATIC_MIMES[i].ext) == ext_len &&
            !strncasecmp(STATIC_MIMES[i].ext, dot, ext_len))
            return STATIC_MIMES[i].content_type;
    }
    return NULL;
}

#define MAX_CONNECTIONS 4096
#define MAX_STREAMS     (MAX_CONNECTIONS * 128)
#define BUF_COUNT       8192
#define BUF_SIZE        (64 * 1024)
#define RING_ENTRIES    8192
#define BACKLOG         4096

static void pin_to_core(int core) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

/* Extract a pseudo-header value from sh2 request headers */
static const char *find_header(const sh2_header_field_t *fields,
                               uint32_t count,
                               const char *name, uint32_t *out_len) {
    size_t nlen = strlen(name);
    for (uint32_t i = 0; i < count; i++) {
        if (fields[i].name_len == nlen &&
            !memcmp(fields[i].name, name, nlen)) {
            if (out_len) *out_len = fields[i].value_len;
            return fields[i].value;
        }
    }
    return NULL;
}

/* Copy a header value to a NUL-terminated string */
static char *header_to_str(const char *val, uint32_t len) {
    char *s = malloc(len + 1);
    if (s) { memcpy(s, val, len); s[len] = '\0'; }
    return s;
}

/* Build tenant KV prefix from tenant ID. Returns NULL for ID 0 (system). */
static const char *tenant_prefix(uint64_t tenant_id,
                                 char *buf, size_t bufsize) {
    if (tenant_id == 0) return NULL;
    snprintf(buf, bufsize, "tenants/%lu/", (unsigned long)tenant_id);
    return buf;
}

/* ======================================================================
 * Domain cache — per-worker, hybrid in-memory cache of KV
 * ====================================================================== */

#ifdef SH2_HAS_TLS

#define MAX_DOMAIN_CACHE 256
#define MAX_HOSTNAME_LEN 256

typedef struct {
    char          hostname[MAX_HOSTNAME_LEN];
    uint64_t      tenant_id;
    sh2_cert_id_t cert_id;
} domain_entry_t;

typedef struct {
    domain_entry_t    entries[MAX_DOMAIN_CACHE];
    size_t            count;
    kvstore_t        *kv;
    sh2_tls_config_t *tls;
    sh2_cert_id_t     default_cert;
} domain_cache_t;

/* Look up a domain in cache by hostname. Returns entry or NULL. */
static domain_entry_t *domain_cache_find(domain_cache_t *dc,
                                          const char *hostname,
                                          uint32_t hostname_len) {
    for (size_t i = 0; i < dc->count; i++) {
        if (strlen(dc->entries[i].hostname) == hostname_len &&
            memcmp(dc->entries[i].hostname, hostname, hostname_len) == 0)
            return &dc->entries[i];
    }
    return NULL;
}

/* Resolve a hostname via KV lookup and cache the result. */
static domain_entry_t *domain_cache_resolve(domain_cache_t *dc,
                                             const char *hostname,
                                             uint32_t hostname_len) {
    /* Check cache first */
    domain_entry_t *cached = domain_cache_find(dc, hostname, hostname_len);
    if (cached) return cached;

    /* Cache miss — look up domains/<hostname> in KV */
    char domain_key[256 + 8];
    char host_str[MAX_HOSTNAME_LEN];
    uint32_t copy_len = hostname_len < MAX_HOSTNAME_LEN - 1
                        ? hostname_len : MAX_HOSTNAME_LEN - 1;
    memcpy(host_str, hostname, copy_len);
    host_str[copy_len] = '\0';
    snprintf(domain_key, sizeof(domain_key), "domains/%s", host_str);

    void  *value = NULL;
    size_t vlen  = 0;
    if (kv_get(dc->kv, domain_key, &value, &vlen) != 0)
        return NULL;  /* unknown domain */

    /* Parse tenant ID */
    char *endptr;
    uint64_t tenant_id = strtoull(value, &endptr, 10);
    free(value);

    /* Try loading tenant-specific cert from KV */
    sh2_cert_id_t cert_id = dc->default_cert;
    if (tenant_id != 0) {
        char cert_key[128], key_key[128];
        snprintf(cert_key, sizeof(cert_key),
                 "tenants/%lu/__certs/cert.pem", (unsigned long)tenant_id);
        snprintf(key_key, sizeof(key_key),
                 "tenants/%lu/__certs/key.pem", (unsigned long)tenant_id);

        void *cert_raw = NULL, *key_raw = NULL;
        size_t cert_len = 0, key_len = 0;

        if (kv_get(dc->kv, cert_key, &cert_raw, &cert_len) == 0 &&
            kv_get(dc->kv, key_key, &key_raw, &key_len) == 0) {
            /* NUL-terminate for OpenSSL PEM parsing */
            char *cp = malloc(cert_len + 1);
            char *kp = malloc(key_len + 1);
            if (cp && kp) {
                memcpy(cp, cert_raw, cert_len); cp[cert_len] = '\0';
                memcpy(kp, key_raw,  key_len);  kp[key_len]  = '\0';
                sh2_cert_id_t tid;
                if (sh2_tls_config_add_cert(dc->tls, cp, kp,
                                             &tid) == sh2_ok)
                    cert_id = tid;
            }
            free(cp);
            free(kp);
        }
        free(cert_raw);
        free(key_raw);
    }

    /* Add to cache */
    if (dc->count < MAX_DOMAIN_CACHE) {
        domain_entry_t *e = &dc->entries[dc->count++];
        memcpy(e->hostname, host_str, copy_len + 1);
        e->tenant_id = tenant_id;
        e->cert_id   = cert_id;
        return e;
    }

    return NULL;  /* cache full */
}

/* SNI callback — called by sh2 during TLS handshake */
static sh2_sni_result_t sni_callback(const char *hostname,
                                      uint32_t hostname_len,
                                      void *user_data) {
    domain_cache_t *dc = user_data;
    domain_entry_t *entry = domain_cache_resolve(dc, hostname, hostname_len);

    if (entry) {
        return (sh2_sni_result_t){
            .cert_id    = entry->cert_id,
            .domain_tag = entry->tenant_id,
        };
    }

    /* Unknown domain — use default cert, system tenant */
    return (sh2_sni_result_t){
        .cert_id    = dc->default_cert,
        .domain_tag = 0,
    };
}

/* Load default cert from KV and set up TLS config.
 * Returns NULL if TLS not requested or setup fails. */
static sh2_tls_config_t *setup_tls(kvstore_t *kv, domain_cache_t *dc,
                                    int worker_id) {
    sh2_tls_config_t *tls = NULL;
    if (sh2_tls_config_create(&tls) != sh2_ok) {
        fprintf(stderr, "Worker %d: sh2_tls_config_create failed\n", worker_id);
        return NULL;
    }

    /* Load default cert from KV (must NUL-terminate for OpenSSL PEM parsing) */
    void *cert_raw = NULL, *key_raw = NULL;
    size_t cert_len = 0, key_len = 0;

    if (kv_get(kv, "__certs/default/cert.pem", &cert_raw, &cert_len) != 0 ||
        kv_get(kv, "__certs/default/key.pem", &key_raw, &key_len) != 0) {
        fprintf(stderr, "Worker %d: default cert not found in KV "
                "(expected __certs/default/cert.pem and __certs/default/key.pem)\n",
                worker_id);
        free(cert_raw);
        free(key_raw);
        sh2_tls_config_destroy(tls);
        return NULL;
    }

    char *cert_pem = malloc(cert_len + 1);
    char *key_pem  = malloc(key_len + 1);
    if (!cert_pem || !key_pem) {
        free(cert_raw); free(key_raw);
        free(cert_pem); free(key_pem);
        sh2_tls_config_destroy(tls);
        return NULL;
    }
    memcpy(cert_pem, cert_raw, cert_len); cert_pem[cert_len] = '\0';
    memcpy(key_pem,  key_raw,  key_len);  key_pem[key_len]   = '\0';
    free(cert_raw);
    free(key_raw);

    sh2_cert_id_t default_cert;
    if (sh2_tls_config_add_cert(tls, cert_pem, key_pem,
                                 &default_cert) != sh2_ok) {
        fprintf(stderr, "Worker %d: failed to register default cert\n",
                worker_id);
        free(cert_pem);
        free(key_pem);
        sh2_tls_config_destroy(tls);
        return NULL;
    }
    free(cert_pem);
    free(key_pem);

    dc->tls          = tls;
    dc->default_cert = default_cert;

    sh2_tls_config_set_sni_callback(tls, sni_callback, dc);

    return tls;
}

#endif /* SH2_HAS_TLS */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Free response headers and body for a completed/drained entity. */
static void cleanup_response(shift_t *sh, shift_entity_t e,
                              shift_component_id_t hdr_id,
                              shift_component_id_t body_id) {
    sh2_resp_headers_t *rh = NULL;
    shift_entity_get_component(sh, e, hdr_id, (void **)&rh);
    if (rh && rh->fields) {
        for (uint32_t h = 0; h < rh->count; h++) {
            free((void *)rh->fields[h].name);
            free((void *)rh->fields[h].value);
        }
        free(rh->fields);
        rh->fields = NULL;
        rh->count  = 0;
    }

    sh2_resp_body_t *rb = NULL;
    shift_entity_get_component(sh, e, body_id, (void **)&rb);
    if (rb && rb->data) {
        free(rb->data);
        rb->data = NULL;
        rb->len  = 0;
    }
}

/* ======================================================================
 * Proxy subsystem — forwards admin requests to code server
 * Follows the h2c_proxy.c example pattern from shift-h2.
 * ====================================================================== */

typedef struct {
    shift_entity_t peer;  /* client-side entity ID */
} proxy_peer_t;

typedef struct {
    shift_t                *sh;
    sh2_component_ids_t     comp;
    shift_component_id_t    proxy_peer;

    /* sh2-owned server collections */
    shift_collection_id_t   response_in;

    /* proxy-owned collections */
    shift_collection_id_t   pending;
    shift_collection_id_t   inflight;

    /* client-side collections */
    shift_collection_id_t   connect_in;
    shift_collection_id_t   connect_out;
    shift_collection_id_t   disconnect_in;
    shift_collection_id_t   client_request_in;
    shift_collection_id_t   client_cancel_in;
    shift_collection_id_t   client_response_out;

    /* backend state */
    shift_entity_t          backend_session;
    bool                    backend_connecting;
    const char             *backend_host;
    uint16_t                backend_port;
    struct sockaddr_in      backend_addr;
} proxy_t;

static bool proxy_backend_connected(const proxy_t *p) {
    return !shift_entity_is_stale(p->sh, p->backend_session);
}

/* Create a client request entity from a server request entity */
static shift_entity_t submit_upstream(proxy_t *p, shift_entity_t server_e) {
    sh2_req_headers_t *rqh = NULL;
    sh2_req_body_t    *rqb = NULL;
    shift_entity_get_component(p->sh, server_e, p->comp.req_headers,
                               (void **)&rqh);
    shift_entity_get_component(p->sh, server_e, p->comp.req_body,
                               (void **)&rqb);

    shift_entity_t client_e;
    shift_entity_create_one_begin(p->sh, p->client_request_in, &client_e);

    /* bind to backend session */
    sh2_session_t *rsess = NULL;
    shift_entity_get_component(p->sh, client_e, p->comp.session,
                               (void **)&rsess);
    rsess->entity = p->backend_session;

    /* copy request headers, replacing :authority and rewriting :path */
    sh2_header_field_t *fields = calloc(rqh->count, sizeof(sh2_header_field_t));
    for (uint32_t j = 0; j < rqh->count; j++) {
        const sh2_header_field_t *f = &rqh->fields[j];
        if (f->name_len == 10 && memcmp(f->name, ":authority", 10) == 0) {
            char *auth = malloc(strlen(p->backend_host) + 8);
            int auth_len = sprintf(auth, "%s:%u",
                                   p->backend_host, p->backend_port);
            fields[j] = (sh2_header_field_t){
                .name = strdup(":authority"), .name_len = 10,
                .value = auth, .value_len = (uint32_t)auth_len,
            };
        } else if (f->name_len == 5 && memcmp(f->name, ":path", 5) == 0 &&
                   f->value_len >= 13 &&
                   memcmp(f->value, "/_admin/code/", 13) == 0) {
            /* strip /_admin/code prefix: /_admin/code/upload → /upload */
            uint32_t new_len = f->value_len - 12;
            char *v = malloc(new_len);
            memcpy(v, f->value + 12, new_len);
            fields[j] = (sh2_header_field_t){
                .name = strdup(":path"), .name_len = 5,
                .value = v, .value_len = new_len,
            };
        } else {
            char *n = malloc(f->name_len);
            char *v = malloc(f->value_len);
            memcpy(n, f->name, f->name_len);
            memcpy(v, f->value, f->value_len);
            fields[j] = (sh2_header_field_t){
                .name = n, .name_len = f->name_len,
                .value = v, .value_len = f->value_len,
            };
        }
    }

    sh2_req_headers_t *crh = NULL;
    shift_entity_get_component(p->sh, client_e, p->comp.req_headers,
                               (void **)&crh);
    crh->fields = fields;
    crh->count  = rqh->count;

    /* copy request body */
    if (rqb && rqb->data && rqb->len > 0) {
        sh2_req_body_t *crb = NULL;
        shift_entity_get_component(p->sh, client_e, p->comp.req_body,
                                   (void **)&crb);
        crb->data = malloc(rqb->len);
        memcpy(crb->data, rqb->data, rqb->len);
        crb->len = rqb->len;
    }

    shift_entity_create_one_end(p->sh, client_e);
    return client_e;
}

/* System: consume connect results */
static void system_proxy_connect(proxy_t *p) {
    if (!p->backend_connecting) return;

    shift_entity_t *entities = NULL;
    size_t count = 0;
    shift_collection_get_entities(p->sh, p->connect_out, &entities, &count);

    for (size_t i = 0; i < count; i++) {
        sh2_io_result_t *io = NULL;
        shift_entity_get_component(p->sh, entities[i], p->comp.io_result,
                                   (void **)&io);
        if (io && io->error == 0) {
            sh2_session_t *sess = NULL;
            shift_entity_get_component(p->sh, entities[i], p->comp.session,
                                       (void **)&sess);
            p->backend_session = sess->entity;
            p->backend_connecting = false;
            printf("Proxy: backend connected to %s:%u\n",
                   p->backend_host, p->backend_port);
        } else {
            fprintf(stderr, "Proxy: backend connect failed: %d\n",
                    io ? io->error : -1);
            p->backend_connecting = false;
        }
        shift_entity_destroy_one(p->sh, entities[i]);
    }
}

/* System: flush pending requests once backend connects */
static void system_proxy_flush_pending(proxy_t *p) {
    if (!proxy_backend_connected(p)) return;

    shift_entity_t *entities = NULL;
    size_t count = 0;
    shift_collection_get_entities(p->sh, p->pending, &entities, &count);

    for (size_t i = 0; i < count; i++) {
        shift_entity_t client_e = submit_upstream(p, entities[i]);

        /* set cross-reference: server → client.
         * Entity is in pending (has proxy_peer). Client entity travels
         * through sh2 internal collections that lack proxy_peer, so the
         * reference must live on the server side. */
        proxy_peer_t *pp = NULL;
        shift_entity_get_component(p->sh, entities[i], p->proxy_peer,
                                   (void **)&pp);
        pp->peer = client_e;

        shift_entity_move_one(p->sh, entities[i], p->inflight);
    }
}

/* System: map upstream responses back to downstream */
static void system_proxy_map_responses(proxy_t *p) {
    shift_entity_t *entities = NULL;
    size_t count = 0;
    shift_collection_get_entities(p->sh, p->client_response_out,
                                  &entities, &count);

    /* get inflight server entities and their peer components for scanning */
    shift_entity_t *inflight_ents = NULL;
    proxy_peer_t   *inflight_peers = NULL;
    size_t          inflight_count = 0;
    shift_collection_get_entities(p->sh, p->inflight,
                                  &inflight_ents, &inflight_count);
    if (inflight_count > 0)
        shift_collection_get_component_array(p->sh, p->inflight, p->proxy_peer,
                                             (void **)&inflight_peers, NULL);

    for (size_t i = 0; i < count; i++) {
        shift_entity_t client_e = entities[i];

        /* scan inflight for the server entity whose peer matches client_e */
        shift_entity_t server_e = {0};
        for (size_t j = 0; j < inflight_count; j++) {
            if (inflight_peers[j].peer.index == client_e.index &&
                inflight_peers[j].peer.generation == client_e.generation) {
                server_e = inflight_ents[j];
                break;
            }
        }
        if (server_e.index == 0 && server_e.generation == 0) {
            fprintf(stderr, "Proxy: orphan response (no matching inflight)\n");
            shift_entity_destroy_one(p->sh, client_e);
            continue;
        }

        /* check for upstream errors */
        sh2_io_result_t *uio = NULL;
        shift_entity_get_component(p->sh, client_e, p->comp.io_result,
                                   (void **)&uio);
        if (uio && uio->error != 0) {
            fprintf(stderr, "Proxy: upstream error %d, sending 502\n",
                    uio->error);

            sh2_status_t *st = NULL;
            shift_entity_get_component(p->sh, server_e, p->comp.status,
                                       (void **)&st);
            st->code = 502;

            sh2_resp_headers_t *rh = NULL;
            shift_entity_get_component(p->sh, server_e, p->comp.resp_headers,
                                       (void **)&rh);
            rh->fields = NULL;
            rh->count  = 0;

            sh2_resp_body_t *rb = NULL;
            shift_entity_get_component(p->sh, server_e, p->comp.resp_body,
                                       (void **)&rb);
            rb->data = strdup("Bad Gateway\n");
            rb->len  = 12;

            shift_entity_move_one(p->sh, server_e, p->response_in);
            shift_entity_destroy_one(p->sh, client_e);
            continue;
        }

        /* copy status */
        sh2_status_t *ust = NULL;
        shift_entity_get_component(p->sh, client_e, p->comp.status,
                                   (void **)&ust);
        sh2_status_t *st = NULL;
        shift_entity_get_component(p->sh, server_e, p->comp.status,
                                   (void **)&st);
        st->code = ust ? ust->code : 502;

        /* copy response headers */
        sh2_resp_headers_t *urh = NULL;
        shift_entity_get_component(p->sh, client_e, p->comp.resp_headers,
                                   (void **)&urh);
        sh2_resp_headers_t *rh = NULL;
        shift_entity_get_component(p->sh, server_e, p->comp.resp_headers,
                                   (void **)&rh);
        if (urh && urh->count > 0) {
            sh2_header_field_t *dst = malloc(urh->count * sizeof(*dst));
            for (uint32_t j = 0; j < urh->count; j++) {
                char *n = malloc(urh->fields[j].name_len);
                char *v = malloc(urh->fields[j].value_len);
                memcpy(n, urh->fields[j].name, urh->fields[j].name_len);
                memcpy(v, urh->fields[j].value, urh->fields[j].value_len);
                dst[j] = (sh2_header_field_t){
                    .name = n, .name_len = urh->fields[j].name_len,
                    .value = v, .value_len = urh->fields[j].value_len,
                };
            }
            rh->fields = dst;
            rh->count  = urh->count;
        } else {
            rh->fields = NULL;
            rh->count  = 0;
        }

        /* copy response body */
        sh2_resp_body_t *urb = NULL;
        shift_entity_get_component(p->sh, client_e, p->comp.resp_body,
                                   (void **)&urb);
        sh2_resp_body_t *rb = NULL;
        shift_entity_get_component(p->sh, server_e, p->comp.resp_body,
                                   (void **)&rb);
        if (urb && urb->data && urb->len > 0) {
            rb->data = malloc(urb->len);
            memcpy(rb->data, urb->data, urb->len);
            rb->len = urb->len;
        } else {
            rb->data = NULL;
            rb->len  = 0;
        }

        /* moves last */
        shift_entity_move_one(p->sh, server_e, p->response_in);
        shift_entity_destroy_one(p->sh, client_e);
    }
}

/* System: reconnect if backend disconnected */
static void system_proxy_reconnect(proxy_t *p) {
    if (proxy_backend_connected(p) || p->backend_connecting)
        return;

    shift_entity_t ce;
    shift_entity_create_one_begin(p->sh, p->connect_in, &ce);
    sh2_connect_target_t *tgt = NULL;
    shift_entity_get_component(p->sh, ce, p->comp.connect_target,
                               (void **)&tgt);
    tgt->addr         = p->backend_addr;
    tgt->hostname     = p->backend_host;
    tgt->hostname_len = (uint32_t)strlen(p->backend_host);
    shift_entity_create_one_end(p->sh, ce);
    p->backend_connecting = true;
    printf("Proxy: reconnecting to %s:%u...\n",
           p->backend_host, p->backend_port);
}

void *sjs_worker_fn(void *arg) {
    sjs_worker_config_t *wcfg = arg;

    pin_to_core(wcfg->worker_core);
    printf("shift-js worker %d: pinned to core %d\n",
           wcfg->worker_id, wcfg->worker_core);

    /* ---- KV store (per-worker connection) ---- */
    kvstore_t *kv = NULL;
    if (kv_open(wcfg->db_path, &kv) != 0) {
        fprintf(stderr, "Worker %d: kv_open failed\n", wcfg->worker_id);
        return NULL;
    }

    /* When raft is active, the raft thread controls WAL checkpointing.
     * Disable auto-checkpoint on worker connections so uncommitted writes
     * stay in the WAL until raft commits and checkpoints explicitly. */
    if (wcfg->raft)
        kv_disable_auto_checkpoint(kv);

    /* ---- TLS setup (per-worker) ---- */
    sh2_tls_config_t *tls = NULL;
#ifdef SH2_HAS_TLS
    domain_cache_t dcache = { .kv = kv };
    if (wcfg->tls) {
        tls = setup_tls(kv, &dcache, wcfg->worker_id);
        if (!tls) {
            kv_close(kv);
            return NULL;
        }
    }
#endif

    /* ---- QuickJS runtime (per-worker) ---- */
    sjs_runtime_t sjs = {0};
    if (sjs_runtime_init(&sjs, kv, wcfg->preprocessors) != 0) {
        fprintf(stderr, "Worker %d: sjs_runtime_init failed\n", wcfg->worker_id);
#ifdef SH2_HAS_TLS
        if (tls) sh2_tls_config_destroy(tls);
#endif
        kv_close(kv);
        return NULL;
    }
    sjs.code_db    = wcfg->code_db;
    sjs.code_store = wcfg->code_store;

    /* ---- Log DB (per-worker file, separate from KV) ---- */
    log_db_t log_db = {0};
    if (!wcfg->no_log) {
        char log_path[256], replay_path[256];
        snprintf(log_path, sizeof(log_path), "%s.logs_%d.db", wcfg->db_path, wcfg->worker_id);
        snprintf(replay_path, sizeof(replay_path), "%s.replay_%d.log", wcfg->db_path, wcfg->worker_id);
        if (log_db_open(&log_db, log_path, replay_path) != 0)
            fprintf(stderr, "Worker %d: log_db_open failed (logging disabled)\n",
                    wcfg->worker_id);
    }
    uint64_t request_counter = 0;

    /* ---- Log DB reader (read-only handles to all worker DBs) ---- */
    log_db_reader_t log_reader = {0};
    if (log_db.db)
        log_db_reader_open(&log_reader, wcfg->num_workers, wcfg->db_path);

    /* ---- Shift context ---- */
    shift_t *sh = NULL;
    shift_config_t sh_cfg = {
        .max_entities            = MAX_CONNECTIONS * 16 + MAX_STREAMS + 1024,
        .max_components          = 32,
        .max_collections         = 64,
        .deferred_queue_capacity = MAX_CONNECTIONS * 256,
    };
    if (shift_context_create(&sh_cfg, &sh) != shift_ok) {
        fprintf(stderr, "Worker %d: shift_context_create failed\n", wcfg->worker_id);
        sjs_runtime_free(&sjs);
#ifdef SH2_HAS_TLS
        if (tls) sh2_tls_config_destroy(tls);
#endif
        kv_close(kv);
        return NULL;
    }

    /* ---- Register sh2 components ---- */
    sh2_component_ids_t comp;
    if (sh2_register_components(sh, &comp) != sh2_ok) {
        fprintf(stderr, "Worker %d: sh2_register_components failed\n", wcfg->worker_id);
        shift_context_destroy(sh);
        sjs_runtime_free(&sjs);
#ifdef SH2_HAS_TLS
        if (tls) sh2_tls_config_destroy(tls);
#endif
        kv_close(kv);
        return NULL;
    }

    /* ---- Register sjs components ---- */
    sjs_component_ids_t sjs_comp;
    if (sjs_register_components(sh, &sjs_comp) != 0) {
        fprintf(stderr, "Worker %d: sjs_register_components failed\n", wcfg->worker_id);
        shift_context_destroy(sh);
        sjs_runtime_free(&sjs);
#ifdef SH2_HAS_TLS
        if (tls) sh2_tls_config_destroy(tls);
#endif
        kv_close(kv);
        return NULL;
    }

    /* ---- Create user collections (sh2 + sjs components) ---- */
    shift_component_id_t all_comps[] = {
        /* sh2 components */
        comp.stream_id, comp.session, comp.req_headers, comp.req_body,
        comp.resp_headers, comp.resp_body, comp.status, comp.io_result,
        comp.domain_tag, comp.peer_cert,
        /* sjs components */
        sjs_comp.resp_headers, sjs_comp.session, sjs_comp.random_tape,
        sjs_comp.route, sjs_comp.bytecode, sjs_comp.resp_status,
        sjs_comp.raft_seq, sjs_comp.log_record,
    };
    size_t ncomps = sizeof(all_comps) / sizeof(all_comps[0]);

    shift_collection_id_t request_out, response_in, response_out;
    shift_collection_info_t ci_req  = { .name = "request_out",        .comp_ids = all_comps, .comp_count = ncomps };
    shift_collection_info_t ci_resp = { .name = "response_in",        .comp_ids = all_comps, .comp_count = ncomps };
    shift_collection_info_t ci_res  = { .name = "response_out", .comp_ids = all_comps, .comp_count = ncomps };
    if (shift_collection_register(sh, &ci_req,  &request_out) != shift_ok ||
        shift_collection_register(sh, &ci_resp, &response_in) != shift_ok ||
        shift_collection_register(sh, &ci_res,  &response_out) != shift_ok) {
        fprintf(stderr, "Worker %d: collection register failed\n", wcfg->worker_id);
        shift_context_destroy(sh);
        sjs_runtime_free(&sjs);
#ifdef SH2_HAS_TLS
        if (tls) sh2_tls_config_destroy(tls);
#endif
        kv_close(kv);
        return NULL;
    }

    /* ---- Proxy: collections (if code server configured) ---- */
    bool proxy_enabled = wcfg->code_server_host != NULL;
    proxy_t proxy = {0};

    if (proxy_enabled) {
        /* Register proxy_peer component */
        shift_component_info_t peer_info = {
            .element_size = sizeof(proxy_peer_t),
        };
        shift_component_register(sh, &peer_info, &proxy.proxy_peer);

        /* sh2-only components (for client-side collections) */
        shift_component_id_t sh2_comps[] = {
            comp.stream_id, comp.session, comp.req_headers, comp.req_body,
            comp.resp_headers, comp.resp_body, comp.status, comp.io_result,
            comp.domain_tag, comp.peer_cert,
        };
        const size_t sh2_comps_count = sizeof(sh2_comps) / sizeof(sh2_comps[0]);

        /* proxy-owned collections add proxy_peer for cross-referencing */
        shift_component_id_t proxy_comps[] = {
            comp.stream_id, comp.session, comp.req_headers, comp.req_body,
            comp.resp_headers, comp.resp_body, comp.status, comp.io_result,
            comp.domain_tag, comp.peer_cert, proxy.proxy_peer,
        };
        const size_t proxy_comps_count = sizeof(proxy_comps) / sizeof(proxy_comps[0]);

        shift_component_id_t connect_comps[] = {
            comp.connect_target, comp.session, comp.io_result,
        };

        shift_collection_info_t colls[] = {
            { .name = "proxy_pending",         .comp_ids = proxy_comps,   .comp_count = proxy_comps_count },
            { .name = "proxy_inflight",        .comp_ids = proxy_comps,   .comp_count = proxy_comps_count },
            { .name = "proxy_connect_in",      .comp_ids = connect_comps, .comp_count = 3 },
            { .name = "proxy_connect_out",     .comp_ids = sh2_comps,     .comp_count = sh2_comps_count },
            { .name = "proxy_disconnect_in",   .comp_ids = sh2_comps,     .comp_count = sh2_comps_count },
            { .name = "proxy_request_in",      .comp_ids = sh2_comps,     .comp_count = sh2_comps_count },
            { .name = "proxy_cancel_in",       .comp_ids = sh2_comps,     .comp_count = sh2_comps_count },
            { .name = "proxy_response_out",    .comp_ids = sh2_comps,     .comp_count = sh2_comps_count },
        };
        shift_collection_id_t *ids[] = {
            &proxy.pending, &proxy.inflight,
            &proxy.connect_in, &proxy.connect_out, &proxy.disconnect_in,
            &proxy.client_request_in, &proxy.client_cancel_in,
            &proxy.client_response_out,
        };
        for (int c = 0; c < 8; c++) {
            if (shift_collection_register(sh, &colls[c], ids[c]) != shift_ok) {
                fprintf(stderr, "Worker %d: proxy collection register failed: %s\n",
                        wcfg->worker_id, colls[c].name);
            }
        }

        proxy.sh           = sh;
        proxy.comp         = comp;
        proxy.response_in  = response_in;
        proxy.backend_host = wcfg->code_server_host;
        proxy.backend_port = wcfg->code_server_port;
    }

    /* ---- Create sh2 context ---- */
    sh2_context_t *h2 = NULL;
    sh2_config_t h2cfg = {
        .shift               = sh,
        .comp_ids            = comp,
        .max_connections     = MAX_CONNECTIONS,
        .ring_entries        = RING_ENTRIES,
        .buf_count           = BUF_COUNT,
        .buf_size            = BUF_SIZE,
        .request_out         = request_out,
        .response_in         = response_in,
        .response_out        = response_out,
#ifdef SH2_HAS_TLS
        .tls                 = tls,
#endif
        .enable_connect      = proxy_enabled,
    };
    if (proxy_enabled) {
        h2cfg.client_colls = (sh2_client_collection_ids_t){
            .connect_in      = proxy.connect_in,
            .connect_out     = proxy.connect_out,
            .disconnect_in   = proxy.disconnect_in,
            .request_in      = proxy.client_request_in,
            .cancel_in       = proxy.client_cancel_in,
            .response_out    = proxy.client_response_out,
        };
    }
    if (sh2_context_create(&h2cfg, &h2) != sh2_ok) {
        fprintf(stderr, "Worker %d: sh2_context_create failed: errno=%d (%s)\n",
                wcfg->worker_id, errno, strerror(errno));
        shift_context_destroy(sh);
        sjs_runtime_free(&sjs);
#ifdef SH2_HAS_TLS
        if (tls) sh2_tls_config_destroy(tls);
#endif
        kv_close(kv);
        return NULL;
    }

    if (sh2_listen(h2, wcfg->port, BACKLOG) != sh2_ok) {
        fprintf(stderr, "Worker %d: sh2_listen failed on port %d\n",
                wcfg->worker_id, wcfg->port);
        sh2_context_destroy(h2);
        shift_context_destroy(sh);
        sjs_runtime_free(&sjs);
#ifdef SH2_HAS_TLS
        if (tls) sh2_tls_config_destroy(tls);
#endif
        kv_close(kv);
        return NULL;
    }

    printf("shift-js worker %d: listening on port %d (%s)\n",
           wcfg->worker_id, wcfg->port, tls ? "h2 TLS" : "h2c");

    /* ---- Raft pending collection ---- */
    /* Only sh2 response components + raft_seq. Moving here from request_out
     * causes shift to destruct the sjs components (session, tape, route,
     * bytecode, resp_headers, resp_status) that are not in this collection. */
    shift_collection_id_t raft_pending = (shift_collection_id_t)-1;
    if (wcfg->raft) {
        shift_component_id_t pending_comps[] = {
            comp.stream_id, comp.session, comp.req_headers, comp.req_body,
            comp.resp_headers, comp.resp_body, comp.status, comp.io_result,
            comp.domain_tag,
            sjs_comp.raft_seq, sjs_comp.log_record,
        };
        shift_collection_info_t ci_rp = {
            .name       = "raft_pending",
            .comp_ids   = pending_comps,
            .comp_count = sizeof(pending_comps) / sizeof(pending_comps[0]),
        };
        if (shift_collection_register(sh, &ci_rp, &raft_pending) != shift_ok) {
            fprintf(stderr, "Worker %d: raft_pending collection register failed\n",
                    wcfg->worker_id);
        }
    }

    /* ---- Connect to code server (if proxy enabled) ---- */
    if (proxy_enabled) {
        proxy.backend_addr = (struct sockaddr_in){
            .sin_family = AF_INET,
            .sin_port   = htons(wcfg->code_server_port),
        };
        inet_pton(AF_INET, wcfg->code_server_host, &proxy.backend_addr.sin_addr);

        shift_entity_t ce;
        shift_entity_create_one_begin(sh, proxy.connect_in, &ce);
        sh2_connect_target_t *tgt = NULL;
        shift_entity_get_component(sh, ce, comp.connect_target, (void **)&tgt);
        tgt->addr         = proxy.backend_addr;
        tgt->hostname     = proxy.backend_host;
        tgt->hostname_len = (uint32_t)strlen(proxy.backend_host);
        shift_entity_create_one_end(sh, ce);
        proxy.backend_connecting = true;

        printf("shift-js worker %d: connecting to code server %s:%d\n",
               wcfg->worker_id, proxy.backend_host, proxy.backend_port);
    }

    /* ---- Event loop ---- */
    while (*wcfg->running) {
        if (sh2_poll(h2, 0) != sh2_ok)
            break;

        /* ---- Proxy: consume connect results ---- */
        if (proxy_enabled) system_proxy_connect(&proxy);
        shift_flush(sh);

        /* ---- Process incoming requests ---- */
        {
            shift_entity_t *entities = NULL;
            size_t          count    = 0;
            shift_collection_get_entities(sh, request_out, &entities, &count);

            /* Mark worker idle when not processing requests so the raft
             * thread's safe_seq isn't held back by stale watermarks. */
            if (count == 0 && wcfg->raft)
                raft_worker_idle(wcfg->raft, wcfg->worker_id);

            for (size_t i = 0; i < count; i++) {
                shift_entity_t e = entities[i];

                sh2_req_headers_t *rqh = NULL;
                sh2_req_body_t    *rqb = NULL;
                shift_entity_get_component(sh, e, comp.req_headers, (void **)&rqh);
                shift_entity_get_component(sh, e, comp.req_body, (void **)&rqb);

                /* Extract :method and :path pseudo-headers */
                uint32_t method_len = 0, path_len = 0;
                const char *method_val = find_header(rqh->fields, rqh->count,
                                                     ":method", &method_len);
                const char *path_val = find_header(rqh->fields, rqh->count,
                                                   ":path", &path_len);

                if (!method_val || !path_val) {
                    /* Malformed request — respond 400 */
                    sh2_status_t *st = NULL;
                    shift_entity_get_component(sh, e, comp.status, (void **)&st);
                    st->code = 400;

                    sh2_resp_body_t *rb = NULL;
                    shift_entity_get_component(sh, e, comp.resp_body, (void **)&rb);
                    rb->data = strdup("Bad Request");
                    rb->len  = 11;

                    sh2_resp_headers_t *rh = NULL;
                    shift_entity_get_component(sh, e, comp.resp_headers, (void **)&rh);
                    rh->fields = NULL;
                    rh->count  = 0;

                    shift_entity_move_one(sh, e, response_in);
                    continue;
                }

                /* Resolve tenant from domain_tag */
                sh2_domain_tag_t *dtag = NULL;
                shift_entity_get_component(sh, e, comp.domain_tag, (void **)&dtag);
                uint64_t tenant_id = dtag ? dtag->tag : 0;

                char prefix_buf[64];
                const char *prefix = tenant_prefix(tenant_id,
                                                    prefix_buf,
                                                    sizeof(prefix_buf));

                char *method_str = header_to_str(method_val, method_len);
                char *path_str   = header_to_str(path_val, path_len);

                /* ---- Static file fast path ---- */
                const char *mime = static_content_type(path_val, path_len);
                if (mime) {
                    /* Strip leading slashes and query string to build key */
                    const char *p = path_val;
                    uint32_t plen = path_len;
                    while (plen > 0 && *p == '/') { p++; plen--; }
                    const char *qm = memchr(p, '?', plen);
                    if (qm) plen = (uint32_t)(qm - p);

                    void *sval = NULL;
                    size_t sval_len = 0;
                    int static_found = 0;

                    /* Try tree-hash path first if deployed. */
                    if (sjs.current_tree_hash[0]) {
                        char tree_key[1024];
                        snprintf(tree_key, sizeof(tree_key),
                                 "database/default/files/%s/%.*s",
                                 sjs.current_tree_hash, (int)plen, p);
                        if (kv_get(kv, tree_key, &sval, &sval_len) == 0)
                            static_found = 1;
                    }

                    /* Legacy __static/ fallback. */
                    if (!static_found) {
                        char static_raw[512];
                        snprintf(static_raw, sizeof(static_raw),
                                 "__static/%.*s", (int)plen, p);
                        char static_key[512];
                        const char *sk = kv_prefixed_key(prefix, static_raw,
                                                          static_key, sizeof(static_key));
                        if (sk && kv_get(kv, sk, &sval, &sval_len) == 0)
                            static_found = 1;
                    }

                    if (static_found) {
                        sh2_status_t *st = NULL;
                        shift_entity_get_component(sh, e, comp.status, (void **)&st);
                        st->code = 200;

                        sh2_resp_body_t *rb = NULL;
                        shift_entity_get_component(sh, e, comp.resp_body, (void **)&rb);
                        rb->data = sval;
                        rb->len  = (uint32_t)sval_len;

                        sh2_resp_headers_t *rh = NULL;
                        shift_entity_get_component(sh, e, comp.resp_headers, (void **)&rh);
                        sh2_header_field_t *hf = malloc(sizeof(sh2_header_field_t));
                        char *ct_name = strdup("content-type");
                        char *ct_val  = strdup(mime);
                        if (!hf || !ct_name || !ct_val) {
                            free(hf); free(ct_name); free(ct_val);
                            free(sval);
                            rb->data = NULL; rb->len = 0;
                        } else {
                            hf[0] = (sh2_header_field_t){
                                .name      = ct_name,
                                .name_len  = 12,
                                .value     = ct_val,
                                .value_len = (uint32_t)strlen(mime),
                            };
                            rh->fields = hf;
                            rh->count  = 1;

                            shift_entity_move_one(sh, e, response_in);
                            free(method_str);
                            free(path_str);
                            continue;
                        }
                    }
                    /* Not found in __static/ — fall through to normal dispatch */
                }

                /* ---- Admin API fast path ---- */
                if (admin_match(path_val, path_len)) {
                    admin_request_t areq = {
                        .method     = method_str,
                        .path       = path_str,
                        .body       = rqb ? rqb->data : NULL,
                        .body_len   = rqb ? rqb->len : 0,
                        .kv_prefix  = prefix,
                    };
                    admin_response_t aresp;
                    admin_services_t admin_svc = {
                        .kv         = kv,
                        .code_db    = sjs.code_db,
                        .code_store = sjs.code_store,
                    };
                    int admin_rc = admin_dispatch(&admin_svc, &areq, &aresp);

                    if (admin_rc == ADMIN_NEEDS_PROXY && proxy_enabled) {
                        /* Park in proxy pending — system_proxy_flush_pending
                         * will create the upstream request after flush. */
                        shift_entity_move_one(sh, e, proxy.pending);
                        free(method_str);
                        free(path_str);
                        continue;
                    }

                    if (admin_rc == ADMIN_NEEDS_PROXY) {
                        /* No proxy available */
                        aresp.status = 503;
                        aresp.body = strdup("{\"error\":\"code server not connected\"}");
                        aresp.body_len = aresp.body ? (uint32_t)strlen(aresp.body) : 0;
                        aresp.content_type = "application/json; charset=utf-8";
                    }

                    sh2_status_t *st = NULL;
                    shift_entity_get_component(sh, e, comp.status, (void **)&st);
                    st->code = aresp.status;

                    sh2_resp_body_t *rb = NULL;
                    shift_entity_get_component(sh, e, comp.resp_body, (void **)&rb);
                    rb->data = aresp.body;
                    rb->len  = aresp.body_len;

                    sh2_resp_headers_t *rh = NULL;
                    shift_entity_get_component(sh, e, comp.resp_headers, (void **)&rh);
                    if (aresp.content_type) {
                        sh2_header_field_t *hf = malloc(sizeof(sh2_header_field_t));
                        char *ct_name = strdup("content-type");
                        char *ct_val  = strdup(aresp.content_type);
                        if (hf && ct_name && ct_val) {
                            hf[0] = (sh2_header_field_t){
                                .name      = ct_name,
                                .name_len  = 12,
                                .value     = ct_val,
                                .value_len = (uint32_t)strlen(aresp.content_type),
                            };
                            rh->fields = hf;
                            rh->count  = 1;
                        } else {
                            free(hf); free(ct_name); free(ct_val);
                            rh->fields = NULL;
                            rh->count  = 0;
                        }
                    } else {
                        rh->fields = NULL;
                        rh->count  = 0;
                    }

                    shift_entity_move_one(sh, e, response_in);
                    free(method_str);
                    free(path_str);
                    continue;
                }

                /* Get sjs component pointers (initialized by constructors) */
                sjs_resp_headers_t *resp_hdrs = NULL;
                sjs_resp_status_t  *resp_st   = NULL;
                sjs_session_t      *session   = NULL;
                sjs_random_tape_t  *tape      = NULL;
                sjs_route_info_t   *route     = NULL;
                sjs_bytecode_t     *bc        = NULL;
                shift_entity_get_component(sh, e, sjs_comp.resp_headers, (void **)&resp_hdrs);
                shift_entity_get_component(sh, e, sjs_comp.resp_status,  (void **)&resp_st);
                shift_entity_get_component(sh, e, sjs_comp.session,      (void **)&session);
                shift_entity_get_component(sh, e, sjs_comp.random_tape,  (void **)&tape);
                shift_entity_get_component(sh, e, sjs_comp.route,        (void **)&route);
                shift_entity_get_component(sh, e, sjs_comp.bytecode,     (void **)&bc);

                /* Back-pressure: block until raft pipeline has capacity.
                 * This slows the client down rather than rejecting,
                 * matching baseline SQLite busy_timeout behavior. */
                bool raft_active = (wcfg->raft != NULL);
                if (raft_active) {
                    while (!raft_has_capacity(wcfg->raft) && *wcfg->running)
                        sched_yield();
                }

                /* Signal raft we're about to process a request */
                if (raft_active)
                    raft_worker_begin(wcfg->raft, wcfg->worker_id);

                /* Build view struct for JS dispatch */
                raft_write_set_t ws = {0};
                if (raft_active)
                    raft_write_set_init(&ws);

                log_batch_t log_batch = {0};
                sjs_replay_capture_t replay_cap;
                replay_capture_init(&replay_cap);
                uint64_t rid = ((uint64_t)wcfg->worker_id << 48) |
                               (request_counter++);

                sjs_log_record_t *log_rec = NULL;
                shift_entity_get_component(sh, e, sjs_comp.log_record, (void **)&log_rec);

                sjs_request_ctx_t req = {
                    .method       = method_str,
                    .path         = path_str,
                    .headers      = rqh->fields,
                    .header_count = rqh->count,
                    .body         = rqb->data,
                    .body_len     = rqb->len,
                    .kv_prefix    = prefix,
                    .resp_hdrs    = resp_hdrs,
                    .resp_st      = resp_st,
                    .session      = session,
                    .tape         = tape,
                    .write_set    = raft_active ? &ws : NULL,
                    .raft         = wcfg->raft,
                    .log_db       = log_db.db ? &log_db : NULL,
                    .log_reader   = log_reader.dbs ? &log_reader : NULL,
                    .log_batch    = &log_batch,
                    .request_id   = rid,
                    .replay_capture = &replay_cap,
                    .log_record   = log_rec,
                };

                uint32_t body_len = 0;
                char *body = sjs_dispatch(&sjs, &req, route, bc, &body_len);
                replay_capture_free(&replay_cap);

                /* log_rec lives on the response entity — flushed during drain */

                /* Submit write-set to Raft if there are writes.
                 * ws.seq was set by kv_next_seq inside the committed txn. */
                uint64_t raft_seq = 0;
                bool needs_raft_wait = false;
                if (raft_active && ws.op_count > 0 && ws.seq > 0) {
                    if (raft_handle_is_leader(wcfg->raft)) {
                        raft_seq = ws.seq;
                        raft_propose_writeset(wcfg->raft, &ws);
                        raft_worker_committed(wcfg->raft, wcfg->worker_id,
                                              raft_seq);
                        needs_raft_wait = true;
                    } else {
                        /* Not leader — 307 redirect */
                        resp_st->code = 307;
                        raft_write_set_free(&ws);
                    }
                } else if (raft_active) {
                    raft_write_set_free(&ws);
                    /* Read-only request — mark worker idle */
                    raft_worker_idle(wcfg->raft, wcfg->worker_id);
                }

                /* Build sh2 response */
                sh2_status_t *st = NULL;
                shift_entity_get_component(sh, e, comp.status, (void **)&st);
                st->code = resp_st->code;
                if (log_rec) log_rec->status_code = resp_st->code;

                /* Transfer response headers to sh2 (string ownership moves) */
                uint32_t nhdr = resp_hdrs->count;
                sh2_header_field_t *resp_fields = NULL;
                if (nhdr > 0) {
                    resp_fields = malloc(nhdr * sizeof(sh2_header_field_t));
                    if (!resp_fields) nhdr = 0;
                    for (uint32_t h = 0; h < nhdr; h++) {
                        resp_fields[h] = (sh2_header_field_t){
                            .name      = resp_hdrs->names[h],
                            .name_len  = (uint32_t)strlen(resp_hdrs->names[h]),
                            .value     = resp_hdrs->values[h],
                            .value_len = (uint32_t)strlen(resp_hdrs->values[h]),
                        };
                    }
                }
                /* Null out sjs component to prevent destructor double-free.
                 * String ownership is now with resp_fields / sh2. */
                resp_hdrs->count = 0;
                free(resp_hdrs->names);  resp_hdrs->names  = NULL;
                free(resp_hdrs->values); resp_hdrs->values = NULL;

                sh2_resp_headers_t *rh = NULL;
                shift_entity_get_component(sh, e, comp.resp_headers, (void **)&rh);
                rh->fields = resp_fields;
                rh->count  = nhdr;

                sh2_resp_body_t *rb = NULL;
                shift_entity_get_component(sh, e, comp.resp_body, (void **)&rb);
                rb->data = body;
                rb->len  = body_len;

                if (needs_raft_wait && raft_pending != (shift_collection_id_t)-1) {
                    /* Park entity in raft_pending until committed.
                     * raft_pending only has sh2 response components + raft_seq,
                     * so shift will call destructors on the dropped sjs
                     * components (session, tape, route, bytecode, etc). */
                    sjs_raft_seq_t *rseq = NULL;
                    shift_entity_get_component(sh, e, sjs_comp.raft_seq,
                                                (void **)&rseq);
                    rseq->seq = raft_seq;
                    shift_entity_move_one(sh, e, raft_pending);
                } else {
                    shift_entity_move_one(sh, e, response_in);
                }

                free(method_str);
                free(path_str);
                /* No manual cleanup needed — sjs component destructors
                 * handle route, session, tape, bytecode on entity destroy. */
            }
        }
        shift_flush(sh);

        /* ---- Release Raft-committed pending responses ---- */
        if (wcfg->raft && raft_pending != (shift_collection_id_t)-1) {
            uint64_t committed = raft_committed_seq(wcfg->raft);
            uint64_t faulted   = raft_faulted_seq(wcfg->raft);

            shift_entity_t *pents = NULL;
            size_t pcount = 0;
            shift_collection_get_entities(sh, raft_pending, &pents, &pcount);

            for (size_t p = 0; p < pcount; p++) {
                shift_entity_t pe = pents[p];
                sjs_raft_seq_t *rseq = NULL;
                shift_entity_get_component(sh, pe, sjs_comp.raft_seq,
                                            (void **)&rseq);
                if (!rseq) continue;

                if (rseq->seq <= committed) {
                    /* Committed — send response */
                    shift_entity_move_one(sh, pe, response_in);
                } else if (faulted > 0 && rseq->seq <= faulted) {
                    /* Leader lost — 503 */
                    sh2_status_t *pst = NULL;
                    shift_entity_get_component(sh, pe, comp.status,
                                                (void **)&pst);
                    pst->code = 503;

                    sh2_resp_body_t *prb = NULL;
                    shift_entity_get_component(sh, pe, comp.resp_body,
                                                (void **)&prb);
                    free(prb->data);
                    prb->data = strdup("Service Unavailable (leader lost)");
                    prb->len  = 34;

                    shift_entity_move_one(sh, pe, response_in);
                }
            }
        }
        shift_flush(sh);

        /* ---- Proxy: flush pending → inflight ---- */
        if (proxy_enabled) system_proxy_flush_pending(&proxy);
        shift_flush(sh);

        /* ---- Proxy: map upstream responses back to downstream ---- */
        if (proxy_enabled) system_proxy_map_responses(&proxy);
        shift_flush(sh);

        /* ---- Drain completed responses & flush logs ---- */
        {
            shift_entity_t *entities = NULL;
            size_t          count    = 0;
            shift_collection_get_entities(sh, response_out,
                                          &entities, &count);

            if (log_db.db)
                log_db_flush_begin(&log_db);

            for (size_t i = 0; i < count; i++) {
                shift_entity_t e = entities[i];

                sh2_io_result_t *io = NULL;
                shift_entity_get_component(sh, e, comp.io_result, (void **)&io);
                if (io && io->error != 0) {
                    fprintf(stderr, "Worker %d: response send failed: %d\n",
                            wcfg->worker_id, io->error);
                }

                /* Flush log record before destroy (SQLITE_STATIC is safe —
                 * sqlite3_step completes before entity destructor frees data) */
                if (log_db.db) {
                    sjs_log_record_t *lr = NULL;
                    shift_entity_get_component(sh, e, sjs_comp.log_record, (void **)&lr);
                    if (lr && (lr->log_count > 0 || lr->req_json))
                        log_db_flush_one(&log_db, lr);
                }

                /* Free owned response data before destroy — sh2 destructors
                 * only free the fields array and body pointer, not the
                 * individual header name/value strings we strdup'd. */
                cleanup_response(sh, e, comp.resp_headers, comp.resp_body);
                shift_entity_destroy_one(sh, e);
            }

            if (log_db.db)
                log_db_flush_commit(&log_db);
        }

        /* ---- Proxy: reconnect if backend disconnected ---- */
        if (proxy_enabled) system_proxy_reconnect(&proxy);
        shift_flush(sh);
    }

    /* ---- Shutdown ---- */
    sh2_context_destroy(h2);
    shift_flush(sh);

    /* Drain remaining entities — flush logs and free response data before destroy */
    shift_collection_id_t drain[8];
    int drain_count = 0;
    drain[drain_count++] = request_out;
    drain[drain_count++] = response_in;
    drain[drain_count++] = response_out;
    if (raft_pending != (shift_collection_id_t)-1)
        drain[drain_count++] = raft_pending;
    if (proxy_enabled) {
        drain[drain_count++] = proxy.pending;
        drain[drain_count++] = proxy.inflight;
    }
    if (log_db.db)
        log_db_flush_begin(&log_db);
    for (int c = 0; c < drain_count; c++) {
        shift_entity_t *ents = NULL;
        size_t cnt = 0;
        shift_collection_get_entities(sh, drain[c], &ents, &cnt);
        for (size_t i = 0; i < cnt; i++) {
            if (log_db.db) {
                sjs_log_record_t *lr = NULL;
                shift_entity_get_component(sh, ents[i], sjs_comp.log_record, (void **)&lr);
                if (lr && (lr->log_count > 0 || lr->req_json))
                    log_db_flush_one(&log_db, lr);
            }
            cleanup_response(sh, ents[i], comp.resp_headers, comp.resp_body);
            shift_entity_destroy_one(sh, ents[i]);
        }
    }
    if (log_db.db)
        log_db_flush_commit(&log_db);
    shift_flush(sh);

    shift_context_destroy(sh);
    sjs_runtime_free(&sjs);
    log_db_reader_close(&log_reader);
    log_db_close(&log_db);
#ifdef SH2_HAS_TLS
    if (tls) sh2_tls_config_destroy(tls);
#endif
    kv_close(kv);

    printf("shift-js worker %d: shutdown complete\n", wcfg->worker_id);
    return NULL;
}
