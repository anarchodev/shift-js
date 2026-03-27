#include "raft_log.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sjs_raft_log {
    sqlite3      *db;
    sqlite3_stmt *stmt_append;
    sqlite3_stmt *stmt_get;
    /* stmt_get_range removed — no longer needed */
    sqlite3_stmt *stmt_trunc_after;
    sqlite3_stmt *stmt_trunc_before;
    sqlite3_stmt *stmt_last;
    sqlite3_stmt *stmt_save_state;
    sqlite3_stmt *stmt_load_state;
};

static const char SQL_CREATE_LOG[] =
    "CREATE TABLE IF NOT EXISTS raft_log ("
    "  idx  INTEGER PRIMARY KEY,"
    "  term INTEGER NOT NULL,"
    "  data BLOB NOT NULL"
    ");";

static const char SQL_CREATE_STATE[] =
    "CREATE TABLE IF NOT EXISTS raft_state ("
    "  id           INTEGER PRIMARY KEY CHECK (id = 1),"
    "  current_term INTEGER NOT NULL DEFAULT 0,"
    "  voted_for    INTEGER NOT NULL DEFAULT -1"
    ");";

static const char SQL_SEED_STATE[] =
    "INSERT OR IGNORE INTO raft_state (id, current_term, voted_for) "
    "VALUES (1, 0, -1);";

static const char SQL_APPEND[] =
    "INSERT OR REPLACE INTO raft_log (idx, term, data) VALUES (?, ?, ?);";

static const char SQL_GET[] =
    "SELECT term, data FROM raft_log WHERE idx = ?;";

static const char SQL_GET_RANGE[] =
    "SELECT idx, term, data FROM raft_log WHERE idx >= ? ORDER BY idx LIMIT ?;";

static const char SQL_TRUNC_AFTER[] =
    "DELETE FROM raft_log WHERE idx > ?;";

static const char SQL_TRUNC_BEFORE[] =
    "DELETE FROM raft_log WHERE idx <= ?;";

static const char SQL_LAST[] =
    "SELECT idx, term FROM raft_log ORDER BY idx DESC LIMIT 1;";

static const char SQL_SAVE_STATE[] =
    "UPDATE raft_state SET current_term = ?, voted_for = ? WHERE id = 1;";

static const char SQL_LOAD_STATE[] =
    "SELECT current_term, voted_for FROM raft_state WHERE id = 1;";

#define PREP(db, sql, stmt) \
    if (sqlite3_prepare_v2(db, sql, -1, stmt, NULL) != SQLITE_OK) goto fail

int raft_log_open(const char *db_path, sjs_raft_log_t **out) {
    sjs_raft_log_t *l = calloc(1, sizeof(*l));
    if (!l) return -1;

    int rc = sqlite3_open_v2(db_path, &l->db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                             SQLITE_OPEN_NOMUTEX, NULL);
    if (rc != SQLITE_OK) goto fail;

    sqlite3_exec(l->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(l->db, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);

    rc = sqlite3_exec(l->db, SQL_CREATE_LOG, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_exec(l->db, SQL_CREATE_STATE, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_exec(l->db, SQL_SEED_STATE, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    PREP(l->db, SQL_APPEND,       &l->stmt_append);
    PREP(l->db, SQL_GET,          &l->stmt_get);
    /* get_range removed */
    PREP(l->db, SQL_TRUNC_AFTER,  &l->stmt_trunc_after);
    PREP(l->db, SQL_TRUNC_BEFORE, &l->stmt_trunc_before);
    PREP(l->db, SQL_LAST,         &l->stmt_last);
    PREP(l->db, SQL_SAVE_STATE,   &l->stmt_save_state);
    PREP(l->db, SQL_LOAD_STATE,   &l->stmt_load_state);

    *out = l;
    return 0;

fail:
    if (l->db) sqlite3_close(l->db);
    free(l);
    return -1;
}

void raft_log_close(sjs_raft_log_t *log) {
    if (!log) return;
    sqlite3_finalize(log->stmt_append);
    sqlite3_finalize(log->stmt_get);
    /* get_range removed */
    sqlite3_finalize(log->stmt_trunc_after);
    sqlite3_finalize(log->stmt_trunc_before);
    sqlite3_finalize(log->stmt_last);
    sqlite3_finalize(log->stmt_save_state);
    sqlite3_finalize(log->stmt_load_state);
    sqlite3_close(log->db);
    free(log);
}

int raft_log_append(sjs_raft_log_t *log, uint64_t index, uint64_t term,
                    const void *data, uint32_t data_len) {
    sqlite3_stmt *st = log->stmt_append;
    sqlite3_reset(st);
    sqlite3_bind_int64(st, 1, (int64_t)index);
    sqlite3_bind_int64(st, 2, (int64_t)term);
    sqlite3_bind_blob(st, 3, data, (int)data_len, SQLITE_STATIC);

    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int raft_log_get(sjs_raft_log_t *log, uint64_t index, sjs_raft_entry_t *out) {
    sqlite3_stmt *st = log->stmt_get;
    sqlite3_reset(st);
    sqlite3_bind_int64(st, 1, (int64_t)index);

    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        out->index = index;
        out->term = (uint64_t)sqlite3_column_int64(st, 0);
        int blen = sqlite3_column_bytes(st, 1);
        out->data = malloc((size_t)blen);
        if (!out->data) { sqlite3_reset(st); return -1; }
        memcpy(out->data, sqlite3_column_blob(st, 1), (size_t)blen);
        out->data_len = (uint32_t)blen;
        sqlite3_reset(st);
        return 0;
    }

    sqlite3_reset(st);
    return (rc == SQLITE_DONE) ? -2 : -1;  /* -2 = not found */
}

int raft_log_truncate_after(sjs_raft_log_t *log, uint64_t after_index) {
    sqlite3_stmt *st = log->stmt_trunc_after;
    sqlite3_reset(st);
    sqlite3_bind_int64(st, 1, (int64_t)after_index);

    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int raft_log_truncate_before(sjs_raft_log_t *log, uint64_t through_index) {
    sqlite3_stmt *st = log->stmt_trunc_before;
    sqlite3_reset(st);
    sqlite3_bind_int64(st, 1, (int64_t)through_index);

    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int raft_log_last(sjs_raft_log_t *log, uint64_t *out_index, uint64_t *out_term) {
    sqlite3_stmt *st = log->stmt_last;
    sqlite3_reset(st);

    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        *out_index = (uint64_t)sqlite3_column_int64(st, 0);
        *out_term  = (uint64_t)sqlite3_column_int64(st, 1);
    } else {
        *out_index = 0;
        *out_term  = 0;
    }

    sqlite3_reset(st);
    return (rc == SQLITE_ROW || rc == SQLITE_DONE) ? 0 : -1;
}

int raft_log_save_state(sjs_raft_log_t *log, uint64_t term, int32_t voted_for) {
    sqlite3_stmt *st = log->stmt_save_state;
    sqlite3_reset(st);
    sqlite3_bind_int64(st, 1, (int64_t)term);
    sqlite3_bind_int(st, 2, voted_for);

    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int raft_log_load_state(sjs_raft_log_t *log, uint64_t *term, int32_t *voted_for) {
    sqlite3_stmt *st = log->stmt_load_state;
    sqlite3_reset(st);

    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        *term      = (uint64_t)sqlite3_column_int64(st, 0);
        *voted_for = (int32_t)sqlite3_column_int(st, 1);
    } else {
        *term      = 0;
        *voted_for = -1;
    }

    sqlite3_reset(st);
    return (rc == SQLITE_ROW || rc == SQLITE_DONE) ? 0 : -1;
}
