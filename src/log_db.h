#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sqlite3.h>

#define LOG_BATCH_MAX 64  /* max console.log calls per request */

typedef enum {
    LOG_LEVEL_LOG   = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_ERROR = 2,
} log_level_t;

/* Pending log entry (accumulated during request, flushed after handler) */
typedef struct {
    log_level_t level;
    uint64_t    timestamp_ns;
    const char *msg;       /* points into arena — valid until arena reset */
    uint32_t    msg_len;
} log_pending_t;

/* Per-request log accumulator */
typedef struct log_batch {
    log_pending_t entries[LOG_BATCH_MAX];
    uint32_t      count;
} log_batch_t;

/* Per-worker log DB handle */
typedef struct log_db {
    sqlite3      *db;
    sqlite3_stmt *insert_stmt;
} log_db_t;

int  log_db_open(log_db_t *ldb, const char *path);
void log_db_close(log_db_t *ldb);

/* Flush a batch of log entries for one request.
 * Wraps all inserts in a single transaction. */
int  log_db_flush(log_db_t *ldb, int worker_id, uint64_t request_id,
                  const char *session_id, const log_batch_t *batch);

/* Passive WAL checkpoint — non-blocking. */
int  log_db_checkpoint(log_db_t *ldb);
