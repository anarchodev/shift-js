#include "push.h"

#include <nghttp2/nghttp2.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ======================================================================
 * Blocking h2c client using nghttp2 mem_send/mem_recv
 * ====================================================================== */

typedef struct {
    int       fd;
    int       last_status;    /* HTTP status from most recent response */
    int       streams_active;
    bool      goaway;
} h2_client_t;

/* ---- socket helpers ---- */

static int tcp_connect(const char *host, const char *port) {
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int send_all(int fd, const void *data, size_t len) {
    const char *p = data;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static ssize_t recv_some(int fd, void *buf, size_t len) {
    return recv(fd, buf, len, 0);
}

/* ---- nghttp2 callbacks ---- */

static int on_frame_recv(nghttp2_session *session,
                          const nghttp2_frame *frame, void *user_data) {
    h2_client_t *c = user_data;
    if (frame->hd.type == NGHTTP2_GOAWAY)
        c->goaway = true;
    return 0;
}

static int on_stream_close(nghttp2_session *session, int32_t stream_id,
                            uint32_t error_code, void *user_data) {
    h2_client_t *c = user_data;
    c->streams_active--;
    return 0;
}

static int on_header(nghttp2_session *session, const nghttp2_frame *frame,
                      const uint8_t *name, size_t namelen,
                      const uint8_t *value, size_t valuelen,
                      uint8_t flags, void *user_data) {
    h2_client_t *c = user_data;
    if (namelen == 7 && memcmp(name, ":status", 7) == 0) {
        c->last_status = atoi((const char *)value);
    }
    return 0;
}

/* Flush all pending nghttp2 output to the socket */
static int h2_flush(nghttp2_session *session, int fd) {
    for (;;) {
        const uint8_t *data;
        ssize_t len = nghttp2_session_mem_send(session, &data);
        if (len < 0) return -1;
        if (len == 0) break;
        if (send_all(fd, data, (size_t)len) != 0) return -1;
    }
    return 0;
}

/* Read data from socket and feed to nghttp2 */
static int h2_recv(nghttp2_session *session, int fd) {
    uint8_t buf[16384];
    ssize_t n = recv_some(fd, buf, sizeof(buf));
    if (n <= 0) return -1;
    ssize_t rv = nghttp2_session_mem_recv(session, buf, (size_t)n);
    if (rv < 0) return -1;
    return 0;
}

/* Drive the session until all active streams complete */
static int h2_drain(nghttp2_session *session, h2_client_t *c) {
    while (c->streams_active > 0 && !c->goaway) {
        if (h2_flush(session, c->fd) != 0) return -1;
        if (h2_recv(session, c->fd) != 0) return -1;
    }
    /* Final flush for any pending frames */
    h2_flush(session, c->fd);
    return 0;
}

/* ---- data provider for POST bodies ---- */

typedef struct {
    const void *data;
    size_t      len;
    size_t      pos;
} data_source_t;

static ssize_t data_read_callback(nghttp2_session *session, int32_t stream_id,
                                   uint8_t *buf, size_t length,
                                   uint32_t *data_flags,
                                   nghttp2_data_source *source,
                                   void *user_data) {
    data_source_t *ds = source->ptr;
    size_t remaining = ds->len - ds->pos;
    size_t n = remaining < length ? remaining : length;
    memcpy(buf, (const char *)ds->data + ds->pos, n);
    ds->pos += n;
    if (ds->pos >= ds->len)
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return (ssize_t)n;
}

/* Submit a POST request. Returns stream_id or -1. */
static int32_t submit_post(nghttp2_session *session, const char *path,
                            data_source_t *ds) {
    nghttp2_nv hdrs[] = {
        { (uint8_t *)":method", (uint8_t *)"POST", 7, 4, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":path", (uint8_t *)path, 5, strlen(path), NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":scheme", (uint8_t *)"http", 7, 4, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":authority", (uint8_t *)"localhost", 10, 9, NGHTTP2_NV_FLAG_NONE },
    };

    nghttp2_data_provider prd = {
        .source.ptr = ds,
        .read_callback = data_read_callback,
    };

    int32_t sid = nghttp2_submit_request(session, NULL, hdrs, 4,
                                          ds ? &prd : NULL, NULL);
    return sid;
}

/* ======================================================================
 * Push implementation
 * ====================================================================== */

static int push_to_server(code_db_t *db, const char *tree_hash,
                           const char *host, const char *port,
                           char **error) {
    /* Get tree modules */
    size_t mod_count;
    code_tree_module_t *mods = code_db_get_tree_modules(db, tree_hash, &mod_count);
    if (!mods || mod_count == 0) {
        *error = strdup("no modules in tree");
        free(mods);
        return -1;
    }

    /* Connect */
    int fd = tcp_connect(host, port);
    if (fd < 0) {
        asprintf(error, "connect to %s:%s failed", host, port);
        for (size_t i = 0; i < mod_count; i++) code_tree_module_free(&mods[i]);
        free(mods);
        return -1;
    }

    /* Create nghttp2 client session */
    nghttp2_session_callbacks *callbacks;
    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header);

    h2_client_t client = { .fd = fd };
    nghttp2_session *session;
    nghttp2_session_client_new(&session, callbacks, &client);
    nghttp2_session_callbacks_del(callbacks);

    /* Send client connection preface (settings) */
    nghttp2_settings_entry settings[] = {
        { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100 },
    };
    nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, settings, 1);
    if (h2_flush(session, fd) != 0) {
        *error = strdup("failed to send h2 preface");
        goto fail;
    }

    /* Read server settings */
    if (h2_recv(session, fd) != 0) {
        *error = strdup("failed to receive server settings");
        goto fail;
    }
    h2_flush(session, fd);

    /* Push each module */
    for (size_t i = 0; i < mod_count; i++) {
        char path[512];
        snprintf(path, sizeof(path), "/_mgmt/tree/%s/%s",
                 tree_hash, mods[i].module_path);

        data_source_t ds = {
            .data = mods[i].bytecode,
            .len = mods[i].bytecode_len,
            .pos = 0,
        };

        client.last_status = 0;
        int32_t sid = submit_post(session, path, &ds);
        if (sid < 0) {
            asprintf(error, "submit_request failed for %s", mods[i].module_path);
            goto fail;
        }
        client.streams_active++;

        /* Flush and drain this stream before submitting next
         * (simpler than managing multiple concurrent streams) */
        if (h2_drain(session, &client) != 0) {
            asprintf(error, "push failed for %s", mods[i].module_path);
            goto fail;
        }

        if (client.last_status != 200) {
            asprintf(error, "push %s returned status %d",
                     mods[i].module_path, client.last_status);
            goto fail;
        }
    }

    /* Activate */
    {
        char path[512];
        snprintf(path, sizeof(path), "/_mgmt/tree/%s/activate", tree_hash);

        client.last_status = 0;
        int32_t sid = submit_post(session, path, NULL);
        if (sid < 0) {
            *error = strdup("submit activate failed");
            goto fail;
        }
        client.streams_active++;

        if (h2_drain(session, &client) != 0) {
            *error = strdup("activate drain failed");
            goto fail;
        }

        if (client.last_status != 200) {
            asprintf(error, "activate returned status %d", client.last_status);
            goto fail;
        }
    }

    /* Cleanup */
    for (size_t i = 0; i < mod_count; i++) code_tree_module_free(&mods[i]);
    free(mods);
    nghttp2_session_del(session);
    close(fd);
    return 0;

fail:
    for (size_t i = 0; i < mod_count; i++) code_tree_module_free(&mods[i]);
    free(mods);
    nghttp2_session_del(session);
    close(fd);
    return -1;
}

push_result_t *push_tree(code_db_t *db, const char *tree_hash,
                          const char **servers, size_t server_count,
                          const char *mgmt_secret, size_t mgmt_secret_len) {
    (void)mgmt_secret;
    (void)mgmt_secret_len;

    push_result_t *results = calloc(server_count, sizeof(*results));

    for (size_t i = 0; i < server_count; i++) {
        results[i].server = strdup(servers[i]);

        /* Parse host:port */
        char *dup = strdup(servers[i]);
        char *colon = strrchr(dup, ':');
        if (!colon) {
            results[i].status = -1;
            results[i].error = strdup("invalid server address (need host:port)");
            free(dup);
            continue;
        }
        *colon = '\0';
        const char *host = dup;
        const char *port = colon + 1;

        char *err = NULL;
        int rc = push_to_server(db, tree_hash, host, port, &err);
        results[i].status = rc;
        results[i].error = err;
        free(dup);
    }

    return results;
}

void push_result_free(push_result_t *r) {
    if (!r) return;
    free(r->server);
    free(r->error);
}
