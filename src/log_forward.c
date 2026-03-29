#include "log_forward.h"

#include <nghttp2/nghttp2.h>

#include <arpa/inet.h>
#include <inttypes.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ======================================================================
 * TCP + nghttp2 helpers (same pattern as push.c)
 * ====================================================================== */

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

/* nghttp2 callbacks */

static int on_frame_recv(nghttp2_session *session,
                          const nghttp2_frame *frame, void *user_data) {
    log_forwarder_t *lf = user_data;
    if (frame->hd.type == NGHTTP2_GOAWAY)
        lf->goaway = true;
    return 0;
}

static int on_stream_close(nghttp2_session *session, int32_t stream_id,
                            uint32_t error_code, void *user_data) {
    log_forwarder_t *lf = user_data;
    lf->streams_active--;
    return 0;
}

static int on_header(nghttp2_session *session, const nghttp2_frame *frame,
                      const uint8_t *name, size_t namelen,
                      const uint8_t *value, size_t valuelen,
                      uint8_t flags, void *user_data) {
    log_forwarder_t *lf = user_data;
    if (namelen == 7 && memcmp(name, ":status", 7) == 0)
        lf->last_status = atoi((const char *)value);
    return 0;
}

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

static int h2_recv(nghttp2_session *session, int fd) {
    uint8_t buf[16384];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) return -1;
    ssize_t rv = nghttp2_session_mem_recv(session, buf, (size_t)n);
    if (rv < 0) return -1;
    return 0;
}

static int h2_drain(log_forwarder_t *lf) {
    nghttp2_session *session = lf->ng_session;
    while (lf->streams_active > 0 && !lf->goaway) {
        if (h2_flush(session, lf->fd) != 0) return -1;
        if (h2_recv(session, lf->fd) != 0) return -1;
    }
    h2_flush(session, lf->fd);
    return 0;
}

/* Data provider for POST bodies */
typedef struct {
    const char *data;
    size_t      len;
    size_t      pos;
} data_source_t;

static ssize_t data_read_cb(nghttp2_session *session, int32_t stream_id,
                              uint8_t *buf, size_t length,
                              uint32_t *data_flags,
                              nghttp2_data_source *source, void *user_data) {
    data_source_t *ds = source->ptr;
    size_t remaining = ds->len - ds->pos;
    size_t n = remaining < length ? remaining : length;
    memcpy(buf, ds->data + ds->pos, n);
    ds->pos += n;
    if (ds->pos >= ds->len)
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return (ssize_t)n;
}

/* Submit POST and flush (fire-and-forget — don't wait for response). */
static int post_and_flush(log_forwarder_t *lf, const char *path,
                           const char *body, size_t body_len) {
    nghttp2_session *session = lf->ng_session;

    data_source_t ds = { .data = body, .len = body_len, .pos = 0 };

    nghttp2_nv hdrs[] = {
        { (uint8_t *)":method", (uint8_t *)"POST", 7, 4, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":path", (uint8_t *)path, 5, strlen(path), NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":scheme", (uint8_t *)"http", 7, 4, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":authority", (uint8_t *)"localhost", 10, 9, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)"content-type", (uint8_t *)"application/json", 12, 16, NGHTTP2_NV_FLAG_NONE },
    };

    nghttp2_data_provider prd = {
        .source.ptr = &ds,
        .read_callback = data_read_cb,
    };

    int32_t sid = nghttp2_submit_request(session, NULL, hdrs, 5, &prd, NULL);
    if (sid < 0) {
        fprintf(stderr, "log_forward: submit_request failed: %s\n",
                nghttp2_strerror(sid));
        return -1;
    }

    int rc = h2_flush(session, lf->fd);
    if (rc != 0)
        fprintf(stderr, "log_forward: flush failed\n");
    return rc;
}

/* ======================================================================
 * Connection management
 * ====================================================================== */

static int ensure_connected(log_forwarder_t *lf) {
    if (lf->fd >= 0 && !lf->goaway) return 0;

    /* Close stale connection */
    if (lf->ng_session) {
        nghttp2_session_del(lf->ng_session);
        lf->ng_session = NULL;
    }
    if (lf->fd >= 0) {
        close(lf->fd);
        lf->fd = -1;
    }
    lf->goaway = false;
    lf->streams_active = 0;

    /* Connect */
    lf->fd = tcp_connect(lf->host, lf->port);
    if (lf->fd < 0) return -1;

    /* Create h2c client session */
    nghttp2_session_callbacks *callbacks;
    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header);

    nghttp2_session *session;
    nghttp2_session_client_new(&session, callbacks, lf);
    nghttp2_session_callbacks_del(callbacks);
    lf->ng_session = session;

    /* Send settings */
    nghttp2_settings_entry settings[] = {
        { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100 },
    };
    nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, settings, 1);
    if (h2_flush(session, lf->fd) != 0) {
        nghttp2_session_del(session);
        lf->ng_session = NULL;
        close(lf->fd);
        lf->fd = -1;
        return -1;
    }

    return 0;
}

/* ======================================================================
 * Public API
 * ====================================================================== */

int log_forwarder_init(log_forwarder_t *lf, const char *server) {
    memset(lf, 0, sizeof(*lf));
    lf->fd = -1;

    char *dup = strdup(server);
    char *colon = strrchr(dup, ':');
    if (!colon) { free(dup); return -1; }
    *colon = '\0';
    lf->host = strdup(dup);
    lf->port = strdup(colon + 1);
    free(dup);

    return 0;
}

void log_forwarder_close(log_forwarder_t *lf) {
    if (!lf) return;
    if (lf->ng_session) nghttp2_session_del(lf->ng_session);
    if (lf->fd >= 0) close(lf->fd);
    free(lf->host);
    free(lf->port);
    memset(lf, 0, sizeof(*lf));
    lf->fd = -1;
}

/* ---- JSON escaping helper ---- */

static void json_escape_append(char **buf, size_t *pos, size_t *cap,
                                const char *s, size_t slen) {
    for (size_t i = 0; i < slen; i++) {
        if (*pos + 8 >= *cap) {
            *cap *= 2;
            *buf = realloc(*buf, *cap);
        }
        char c = s[i];
        switch (c) {
        case '"':  (*buf)[(*pos)++] = '\\'; (*buf)[(*pos)++] = '"'; break;
        case '\\': (*buf)[(*pos)++] = '\\'; (*buf)[(*pos)++] = '\\'; break;
        case '\n': (*buf)[(*pos)++] = '\\'; (*buf)[(*pos)++] = 'n'; break;
        case '\r': (*buf)[(*pos)++] = '\\'; (*buf)[(*pos)++] = 'r'; break;
        case '\t': (*buf)[(*pos)++] = '\\'; (*buf)[(*pos)++] = 't'; break;
        default:   (*buf)[(*pos)++] = c;
        }
    }
}

void log_forwarder_send_logs(log_forwarder_t *lf,
                              int64_t tenant_id,
                              int worker_id,
                              uint64_t request_id,
                              const char *session_id,
                              const log_batch_t *batch) {
    if (!lf || !batch || batch->count == 0) return;
    if (ensure_connected(lf) != 0) {
        fprintf(stderr, "log_forward: cannot connect to %s:%s\n",
                lf->host, lf->port);
        return;
    }

    /* Build JSON: {"worker_id":N,"request_id":N,"session_id":"...","entries":[...]} */
    size_t cap = 256 + batch->count * 256;
    char *json = malloc(cap);
    size_t pos = 0;

    pos += (size_t)snprintf(json, cap,
        "{\"tenant_id\":%" PRId64 ",\"worker_id\":%d,"
        "\"request_id\":%" PRIu64 ",\"session_id\":",
        tenant_id, worker_id, request_id);

    if (session_id) {
        json[pos++] = '"';
        json_escape_append(&json, &pos, &cap, session_id, strlen(session_id));
        json[pos++] = '"';
    } else {
        memcpy(json + pos, "null", 4); pos += 4;
    }

    memcpy(json + pos, ",\"entries\":[", 12); pos += 12;

    for (uint32_t i = 0; i < batch->count; i++) {
        if (i > 0) json[pos++] = ',';

        /* Ensure space */
        size_t need = 128 + batch->entries[i].msg_len * 2;
        if (pos + need >= cap) { cap = (pos + need) * 2; json = realloc(json, cap); }

        pos += (size_t)snprintf(json + pos, cap - pos,
            "{\"timestamp\":%" PRIu64 ",\"level\":%d,\"message\":\"",
            batch->entries[i].timestamp_ns, batch->entries[i].level);

        json_escape_append(&json, &pos, &cap,
                            batch->entries[i].msg, batch->entries[i].msg_len);

        json[pos++] = '"';
        json[pos++] = '}';
    }

    memcpy(json + pos, "]}", 2); pos += 2;
    json[pos] = '\0';

    if (post_and_flush(lf, "/logs", json, pos) != 0)
        fprintf(stderr, "log_forward: failed to send logs\n");

    free(json);
}

void log_forwarder_send_replay(log_forwarder_t *lf,
                                int64_t tenant_id,
                                uint64_t request_id,
                                const char *request_data,
                                const char *response_data,
                                const sjs_replay_capture_t *cap,
                                const void *random_tape,
                                size_t random_tape_len) {
    if (!lf || !cap) return;
    if (ensure_connected(lf) != 0) {
        fprintf(stderr, "log_forward: cannot connect to %s:%s\n",
                lf->host, lf->port);
        return;
    }

    /* Build JSON with all replay fields */
    size_t kv_len = cap->kv_tape.data ? cap->kv_tape.len : 0;
    size_t dt_len = cap->date_tape.data ? cap->date_tape.len : 0;
    size_t mr_len = cap->math_random_tape.data ? cap->math_random_tape.len : 0;
    size_t mt_len = cap->module_tree.data ? cap->module_tree.len : 0;
    size_t sm_len = cap->source_maps.data ? cap->source_maps.len : 0;
    size_t rd_len = request_data ? strlen(request_data) : 0;
    size_t rs_len = response_data ? strlen(response_data) : 0;

    size_t cap_size = 256 + rd_len * 2 + rs_len * 2 + kv_len + dt_len +
                      mr_len + mt_len + sm_len;
    char *json = malloc(cap_size);
    size_t pos = 0;

    pos += (size_t)snprintf(json, cap_size,
        "{\"tenant_id\":%" PRId64 ",\"request_id\":%" PRIu64
        ",\"request_data\":\"",
        tenant_id, request_id);

    if (request_data)
        json_escape_append(&json, &pos, &cap_size, request_data, rd_len);
    json[pos++] = '"';

    memcpy(json + pos, ",\"response_data\":\"", 18); pos += 18;
    if (response_data)
        json_escape_append(&json, &pos, &cap_size, response_data, rs_len);
    json[pos++] = '"';

    /* Tape fields are already valid JSON arrays, embed raw */
    memcpy(json + pos, ",\"kv_tape\":\"", 12); pos += 12;
    if (cap->kv_tape.data)
        json_escape_append(&json, &pos, &cap_size, cap->kv_tape.data, kv_len);
    json[pos++] = '"';

    memcpy(json + pos, ",\"date_tape\":\"", 14); pos += 14;
    if (cap->date_tape.data)
        json_escape_append(&json, &pos, &cap_size, cap->date_tape.data, dt_len);
    json[pos++] = '"';

    memcpy(json + pos, ",\"math_random_tape\":\"", 21); pos += 21;
    if (cap->math_random_tape.data)
        json_escape_append(&json, &pos, &cap_size,
                            cap->math_random_tape.data, mr_len);
    json[pos++] = '"';

    memcpy(json + pos, ",\"module_tree\":\"", 16); pos += 16;
    if (cap->module_tree.data)
        json_escape_append(&json, &pos, &cap_size,
                            cap->module_tree.data, mt_len);
    json[pos++] = '"';

    memcpy(json + pos, ",\"source_maps\":\"", 16); pos += 16;
    if (cap->source_maps.data)
        json_escape_append(&json, &pos, &cap_size,
                            cap->source_maps.data, sm_len);
    json[pos++] = '"';

    json[pos++] = '}';
    json[pos] = '\0';

    if (post_and_flush(lf, "/replay", json, pos) != 0)
        fprintf(stderr, "log_forward: failed to send replay\n");

    free(json);
}
