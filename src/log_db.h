#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
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

/* Complete log record for one request (heap-owned, survives arena reset).
 * Accumulated across requests, then flushed in a single transaction. */
typedef struct {
    uint64_t     request_id;
    int          worker_id;
    char        *session_id;      /* strdup'd, may be NULL */

    /* console.log entries (heap-copied from per-request batch) */
    log_pending_t *log_entries;   /* malloc'd array */
    uint32_t       log_count;

    /* replay capture (all heap-owned, may be NULL if no log_db) */
    char    *req_json;
    char    *kv_tape;
    uint8_t *random_tape;
    size_t   random_tape_len;
    char    *date_tape;
    char    *math_random_tape;
    char    *module_tree;
    int      status_code;     /* HTTP response status (e.g. 200, 404) */
} sjs_log_record_t;

void sjs_log_record_free(sjs_log_record_t *rec);

/* Per-worker log DB handle.
 * Console.log entries go to SQLite. Replay captures go to an append-only
 * binary log file with a tiny SQLite index for lookups. */
typedef struct log_db {
    sqlite3      *db;
    sqlite3_stmt *insert_stmt;        /* console.log insert */
    sqlite3_stmt *replay_index_stmt;  /* replay index insert */
    FILE         *replay_file;        /* append-only binary log */
} log_db_t;

int  log_db_open(log_db_t *ldb, const char *db_path, const char *replay_path);
void log_db_close(log_db_t *ldb);

/* Flush a batch of log entries for one request.
 * Wraps all inserts in a single transaction. */
int  log_db_flush(log_db_t *ldb, int worker_id, uint64_t request_id,
                  const char *session_id, const log_batch_t *batch);

/* Query replay capture for a request from a reader.
 * Caller frees all returned strings. Returns 0 on success, -1 on not found. */
int  log_db_get_replay(sqlite3 *db, FILE *replay_file, uint64_t request_id,
                       char **request_data,
                       char **kv_tape,
                       uint8_t **random_tape, size_t *random_tape_len,
                       char **date_tape,
                       char **math_random_tape,
                       char **module_tree);

/* List recent requests from replay index.
 * Caller must free returned strings and the array itself. */
typedef struct {
    uint64_t request_id;
    char    *request_data;   /* JSON */
    int      status_code;    /* HTTP response status */
} log_db_request_entry_t;

int  log_db_list_requests(sqlite3 *db, FILE *replay_file, int limit,
                          log_db_request_entry_t **out, size_t *out_count);
void log_db_free_request_entries(log_db_request_entry_t *entries, size_t count);

/* Flush an array of log records in a single transaction. */
int  log_db_flush_records(log_db_t *ldb, sjs_log_record_t *records, size_t count);

/* Transaction-split flush: begin/one/commit for inline iteration. */
void log_db_flush_begin(log_db_t *ldb);
void log_db_flush_one(log_db_t *ldb, sjs_log_record_t *rec);
void log_db_flush_commit(log_db_t *ldb);

/* Read-only multi-DB reader for cross-worker log queries. */
typedef struct {
    sqlite3 **dbs;
    FILE    **replay_files;
    int       count;
} log_db_reader_t;

int  log_db_reader_open(log_db_reader_t *r, int num_workers, const char *db_path);
void log_db_reader_close(log_db_reader_t *r);
