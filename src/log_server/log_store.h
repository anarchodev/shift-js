#pragma once

#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    sqlite3 *db;
    sqlite3_stmt *insert_log;
    sqlite3_stmt *insert_replay;
    sqlite3_stmt *query_logs;
    sqlite3_stmt *get_replay;
    sqlite3_stmt *list_replays;
} log_store_t;

/* Single log entry for batch insert. */
typedef struct {
    int64_t  timestamp;
    int      level;       /* 0=log, 1=warn, 2=error */
    char    *message;
} log_entry_t;

/* Open or create the log database. Returns 0 on success. */
int log_store_open(log_store_t *ls, const char *path);

/* Close the database. */
void log_store_close(log_store_t *ls);

/* Insert a batch of log entries in a single transaction. */
int log_store_insert_logs(log_store_t *ls, int64_t tenant_id,
                          int worker_id, uint64_t request_id,
                          const char *session_id,
                          const log_entry_t *entries, size_t count);

/* Insert a replay capture. All string params are JSON. */
int log_store_insert_replay(log_store_t *ls, int64_t tenant_id,
                            uint64_t request_id,
                            const char *request_data,
                            const char *response_data,
                            const char *kv_tape,
                            const void *random_tape, size_t random_tape_len,
                            const char *date_tape,
                            const char *math_random_tape,
                            const char *module_tree,
                            const char *source_maps);

/* Query parameters for log search. */
typedef struct {
    int64_t  tenant_id;
    uint64_t request_id;   /* 0 = no filter */
    const char *session_id; /* NULL = no filter */
    int      level;        /* -1 = no filter */
    int64_t  before;       /* 0 = no filter (nanoseconds) */
    int64_t  after;        /* 0 = no filter */
    int      limit;        /* default 100, max 1000 */
} log_query_t;

/* Log query result entry. */
typedef struct {
    int64_t  timestamp;
    int      worker_id;
    uint64_t request_id;
    char    *session_id;
    int      level;
    char    *message;
} log_result_t;

/* Query logs. Returns malloc'd array, sets *count. Caller frees. */
log_result_t *log_store_query(log_store_t *ls, const log_query_t *q,
                               size_t *count);

/* Free a single log result's strings. */
void log_result_free(log_result_t *r);

/* Get replay capture by request_id. Returns malloc'd JSON strings.
 * Returns 0 on success, -1 if not found. Caller frees all strings. */
int log_store_get_replay(log_store_t *ls, uint64_t request_id,
                         char **request_data, char **response_data,
                         char **kv_tape, void **random_tape,
                         size_t *random_tape_len,
                         char **date_tape, char **math_random_tape,
                         char **module_tree, char **source_maps);

/* List recent replays for a tenant. Returns malloc'd array, sets *count. */
typedef struct {
    uint64_t request_id;
    int64_t  created_at;
} replay_summary_t;

replay_summary_t *log_store_list_replays(log_store_t *ls, int64_t tenant_id,
                                          int limit, size_t *count);

/* Passive WAL checkpoint. */
int log_store_checkpoint(log_store_t *ls);
