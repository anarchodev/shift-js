#pragma once

#include "log_db.h"
#include "replay_capture.h"
#include <shift_h2.h>
#include <stdint.h>

/* Per-worker log forwarder — maintains a persistent h2c connection
 * to the log server and sends log batches + replay captures. */
typedef struct {
    int   fd;                /* persistent TCP socket, -1 if not connected */
    char *host;
    char *port;
    void *ng_session;        /* nghttp2_session* */
    int   streams_active;
    int   last_status;
    bool  goaway;
} log_forwarder_t;

/* Initialize the forwarder. server is "host:port". */
int log_forwarder_init(log_forwarder_t *lf, const char *server);

/* Close the connection and free resources. */
void log_forwarder_close(log_forwarder_t *lf);

/* Send a log batch to the log server.
 * Fire-and-forget: errors are logged to stderr, not propagated. */
void log_forwarder_send_logs(log_forwarder_t *lf,
                              int64_t tenant_id,
                              int worker_id,
                              uint64_t request_id,
                              const char *session_id,
                              const log_batch_t *batch);

/* Send a replay capture to the log server. */
void log_forwarder_send_replay(log_forwarder_t *lf,
                                int64_t tenant_id,
                                uint64_t request_id,
                                const char *request_data,
                                const char *response_data,
                                const sjs_replay_capture_t *cap,
                                const void *random_tape,
                                size_t random_tape_len);
