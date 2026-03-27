#include "log_db.h"
#include <stdio.h>
#include <string.h>

static const char *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS logs ("
    "  timestamp  INTEGER NOT NULL,"
    "  worker_id  INTEGER NOT NULL,"
    "  request_id INTEGER NOT NULL,"
    "  session_id TEXT,"
    "  level      INTEGER NOT NULL,"
    "  message    TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_logs_request ON logs(request_id);"
    "CREATE INDEX IF NOT EXISTS idx_logs_ts ON logs(timestamp);";

static const char *INSERT_SQL =
    "INSERT INTO logs (timestamp, worker_id, request_id, session_id, level, message) "
    "VALUES (?, ?, ?, ?, ?, ?)";

int log_db_open(log_db_t *ldb, const char *path) {
    int rc = sqlite3_open(path, &ldb->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "log_db: open failed: %s\n", sqlite3_errmsg(ldb->db));
        return -1;
    }

    /* WAL mode, no fsync, no auto-checkpoint */
    sqlite3_exec(ldb->db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(ldb->db, "PRAGMA synchronous=OFF", NULL, NULL, NULL);
    sqlite3_wal_autocheckpoint(ldb->db, 0);

    /* Create schema */
    char *err = NULL;
    rc = sqlite3_exec(ldb->db, SCHEMA_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "log_db: schema failed: %s\n", err);
        sqlite3_free(err);
        sqlite3_close(ldb->db);
        ldb->db = NULL;
        return -1;
    }

    /* Prepare insert statement */
    rc = sqlite3_prepare_v2(ldb->db, INSERT_SQL, -1, &ldb->insert_stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "log_db: prepare failed: %s\n", sqlite3_errmsg(ldb->db));
        sqlite3_close(ldb->db);
        ldb->db = NULL;
        return -1;
    }

    return 0;
}

void log_db_close(log_db_t *ldb) {
    if (ldb->insert_stmt) {
        sqlite3_finalize(ldb->insert_stmt);
        ldb->insert_stmt = NULL;
    }
    if (ldb->db) {
        sqlite3_close(ldb->db);
        ldb->db = NULL;
    }
}

int log_db_flush(log_db_t *ldb, int worker_id, uint64_t request_id,
                 const char *session_id, const log_batch_t *batch) {
    if (!ldb->db || !ldb->insert_stmt || batch->count == 0)
        return 0;

    sqlite3_exec(ldb->db, "BEGIN", NULL, NULL, NULL);

    for (uint32_t i = 0; i < batch->count; i++) {
        const log_pending_t *e = &batch->entries[i];
        sqlite3_stmt *s = ldb->insert_stmt;

        sqlite3_bind_int64(s, 1, (sqlite3_int64)e->timestamp_ns);
        sqlite3_bind_int(s, 2, worker_id);
        sqlite3_bind_int64(s, 3, (sqlite3_int64)request_id);
        if (session_id)
            sqlite3_bind_text(s, 4, session_id, -1, SQLITE_STATIC);
        else
            sqlite3_bind_null(s, 4);
        sqlite3_bind_int(s, 5, (int)e->level);
        sqlite3_bind_text(s, 6, e->msg, (int)e->msg_len, SQLITE_STATIC);

        sqlite3_step(s);
        sqlite3_reset(s);
    }

    sqlite3_exec(ldb->db, "COMMIT", NULL, NULL, NULL);
    return 0;
}

int log_db_checkpoint(log_db_t *ldb) {
    if (!ldb->db) return -1;
    int rc = sqlite3_wal_checkpoint_v2(ldb->db, NULL,
                                        SQLITE_CHECKPOINT_PASSIVE,
                                        NULL, NULL);
    return (rc == SQLITE_OK) ? 0 : -1;
}
