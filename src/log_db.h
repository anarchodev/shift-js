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
    sqlite3_stmt *replay_insert_stmt;
} log_db_t;

int  log_db_open(log_db_t *ldb, const char *path);
void log_db_close(log_db_t *ldb);

/* Flush a batch of log entries for one request.
 * Wraps all inserts in a single transaction. */
int  log_db_flush(log_db_t *ldb, int worker_id, uint64_t request_id,
                  const char *session_id, const log_batch_t *batch);

/* Flush replay capture data for one request. */
int  log_db_flush_replay(log_db_t *ldb, uint64_t request_id,
                         const char *request_data,
                         const char *response_data,
                         const char *kv_tape,
                         const uint8_t *random_tape, size_t random_tape_len,
                         const char *date_tape,
                         const char *math_random_tape,
                         const char *module_tree,
                         const char *source_maps);

/* Query replay capture for a request. Caller frees all returned strings.
 * Returns 0 on success, -1 on not found. */
int  log_db_get_replay(log_db_t *ldb, uint64_t request_id,
                       char **request_data,
                       char **response_data,
                       char **kv_tape,
                       uint8_t **random_tape, size_t *random_tape_len,
                       char **date_tape,
                       char **math_random_tape,
                       char **module_tree,
                       char **source_maps);

/* List recent requests from replay_captures.
 * Returns an array of (request_id, request_data, response_data) rows.
 * Caller must free returned strings and the array itself.
 * Returns count on success, -1 on error. */
typedef struct {
    uint64_t request_id;
    char    *request_data;   /* JSON */
    char    *response_data;  /* JSON, may be NULL */
} log_db_request_entry_t;

int  log_db_list_requests(log_db_t *ldb, int limit,
                          log_db_request_entry_t **out, size_t *out_count);
void log_db_free_request_entries(log_db_request_entry_t *entries, size_t count);

/* Passive WAL checkpoint — non-blocking. */
int  log_db_checkpoint(log_db_t *ldb);
