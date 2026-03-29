#include "log_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *SCHEMA_SQL =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=OFF;"

    "CREATE TABLE IF NOT EXISTS logs ("
    "  id INTEGER PRIMARY KEY,"
    "  tenant_id INTEGER NOT NULL,"
    "  timestamp INTEGER NOT NULL,"
    "  worker_id INTEGER NOT NULL,"
    "  request_id INTEGER NOT NULL,"
    "  session_id TEXT,"
    "  level INTEGER NOT NULL,"
    "  message TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_logs_request ON logs(request_id);"
    "CREATE INDEX IF NOT EXISTS idx_logs_ts ON logs(tenant_id, timestamp);"
    "CREATE INDEX IF NOT EXISTS idx_logs_session"
    "  ON logs(session_id) WHERE session_id IS NOT NULL;"

    "CREATE TABLE IF NOT EXISTS replay_captures ("
    "  request_id INTEGER PRIMARY KEY,"
    "  tenant_id INTEGER NOT NULL,"
    "  request_data TEXT NOT NULL,"
    "  response_data TEXT,"
    "  kv_tape TEXT NOT NULL,"
    "  random_tape BLOB,"
    "  date_tape TEXT NOT NULL,"
    "  math_random_tape TEXT NOT NULL,"
    "  module_tree TEXT NOT NULL,"
    "  source_maps TEXT,"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_replay_tenant"
    "  ON replay_captures(tenant_id, created_at DESC);";

#define SQL_INSERT_LOG \
    "INSERT INTO logs (tenant_id, timestamp, worker_id, request_id, " \
    "session_id, level, message) VALUES (?, ?, ?, ?, ?, ?, ?)"

#define SQL_INSERT_REPLAY \
    "INSERT OR REPLACE INTO replay_captures " \
    "(request_id, tenant_id, request_data, response_data, kv_tape, " \
    "random_tape, date_tape, math_random_tape, module_tree, source_maps, " \
    "created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"

#define SQL_GET_REPLAY \
    "SELECT request_data, response_data, kv_tape, random_tape, " \
    "date_tape, math_random_tape, module_tree, source_maps " \
    "FROM replay_captures WHERE request_id = ?"

#define SQL_LIST_REPLAYS \
    "SELECT request_id, created_at FROM replay_captures " \
    "WHERE tenant_id = ? ORDER BY created_at DESC LIMIT ?"

static int prepare(sqlite3 *db, const char *sql, sqlite3_stmt **out) {
    int rc = sqlite3_prepare_v2(db, sql, -1, out, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "log_store: prepare failed: %s\n  SQL: %s\n",
                sqlite3_errmsg(db), sql);
        return -1;
    }
    return 0;
}

int log_store_open(log_store_t *ls, const char *path) {
    memset(ls, 0, sizeof(*ls));

    int rc = sqlite3_open(path, &ls->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "log_store: open failed: %s\n", sqlite3_errmsg(ls->db));
        sqlite3_close(ls->db);
        ls->db = NULL;
        return -1;
    }

    char *err = NULL;
    rc = sqlite3_exec(ls->db, SCHEMA_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "log_store: schema failed: %s\n", err);
        sqlite3_free(err);
        sqlite3_close(ls->db);
        ls->db = NULL;
        return -1;
    }

    if (prepare(ls->db, SQL_INSERT_LOG, &ls->insert_log) ||
        prepare(ls->db, SQL_INSERT_REPLAY, &ls->insert_replay) ||
        prepare(ls->db, SQL_GET_REPLAY, &ls->get_replay) ||
        prepare(ls->db, SQL_LIST_REPLAYS, &ls->list_replays)) {
        log_store_close(ls);
        return -1;
    }

    return 0;
}

static void finalize(sqlite3_stmt **s) {
    if (*s) { sqlite3_finalize(*s); *s = NULL; }
}

void log_store_close(log_store_t *ls) {
    if (!ls) return;
    finalize(&ls->insert_log);
    finalize(&ls->insert_replay);
    finalize(&ls->query_logs);
    finalize(&ls->get_replay);
    finalize(&ls->list_replays);
    if (ls->db) { sqlite3_close(ls->db); ls->db = NULL; }
}

int log_store_insert_logs(log_store_t *ls, int64_t tenant_id,
                          int worker_id, uint64_t request_id,
                          const char *session_id,
                          const log_entry_t *entries, size_t count) {
    if (!ls->db || count == 0) return 0;

    sqlite3_exec(ls->db, "BEGIN", NULL, NULL, NULL);

    sqlite3_stmt *s = ls->insert_log;
    for (size_t i = 0; i < count; i++) {
        sqlite3_bind_int64(s, 1, tenant_id);
        sqlite3_bind_int64(s, 2, entries[i].timestamp);
        sqlite3_bind_int(s, 3, worker_id);
        sqlite3_bind_int64(s, 4, (sqlite3_int64)request_id);
        if (session_id)
            sqlite3_bind_text(s, 5, session_id, -1, SQLITE_STATIC);
        else
            sqlite3_bind_null(s, 5);
        sqlite3_bind_int(s, 6, entries[i].level);
        sqlite3_bind_text(s, 7, entries[i].message, -1, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_reset(s);
    }

    sqlite3_exec(ls->db, "COMMIT", NULL, NULL, NULL);
    return 0;
}

int log_store_insert_replay(log_store_t *ls, int64_t tenant_id,
                            uint64_t request_id,
                            const char *request_data,
                            const char *response_data,
                            const char *kv_tape,
                            const void *random_tape, size_t random_tape_len,
                            const char *date_tape,
                            const char *math_random_tape,
                            const char *module_tree,
                            const char *source_maps) {
    if (!ls->db) return -1;
    int64_t now = (int64_t)time(NULL);

    sqlite3_stmt *s = ls->insert_replay;
    sqlite3_bind_int64(s, 1, (sqlite3_int64)request_id);
    sqlite3_bind_int64(s, 2, tenant_id);
    sqlite3_bind_text(s, 3, request_data, -1, SQLITE_STATIC);
    if (response_data)
        sqlite3_bind_text(s, 4, response_data, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 4);
    sqlite3_bind_text(s, 5, kv_tape, -1, SQLITE_STATIC);
    if (random_tape && random_tape_len > 0)
        sqlite3_bind_blob(s, 6, random_tape, (int)random_tape_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 6);
    sqlite3_bind_text(s, 7, date_tape, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 8, math_random_tape, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 9, module_tree, -1, SQLITE_STATIC);
    if (source_maps)
        sqlite3_bind_text(s, 10, source_maps, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 10);
    sqlite3_bind_int64(s, 11, now);

    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

log_result_t *log_store_query(log_store_t *ls, const log_query_t *q,
                               size_t *count) {
    *count = 0;
    if (!ls->db) return NULL;

    /* Build dynamic query */
    char sql[1024];
    int pos = snprintf(sql, sizeof(sql),
        "SELECT timestamp, worker_id, request_id, session_id, level, message "
        "FROM logs WHERE tenant_id = %lld", (long long)q->tenant_id);

    if (q->request_id)
        pos += snprintf(sql + pos, sizeof(sql) - (size_t)pos,
            " AND request_id = %llu", (unsigned long long)q->request_id);
    if (q->session_id)
        pos += snprintf(sql + pos, sizeof(sql) - (size_t)pos,
            " AND session_id = '%s'", q->session_id);
    if (q->level >= 0)
        pos += snprintf(sql + pos, sizeof(sql) - (size_t)pos,
            " AND level = %d", q->level);
    if (q->before > 0)
        pos += snprintf(sql + pos, sizeof(sql) - (size_t)pos,
            " AND timestamp < %lld", (long long)q->before);
    if (q->after > 0)
        pos += snprintf(sql + pos, sizeof(sql) - (size_t)pos,
            " AND timestamp > %lld", (long long)q->after);

    int limit = q->limit > 0 ? q->limit : 100;
    if (limit > 1000) limit = 1000;
    snprintf(sql + pos, sizeof(sql) - (size_t)pos,
        " ORDER BY timestamp DESC LIMIT %d", limit);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(ls->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    size_t cap = 64;
    log_result_t *list = malloc(cap * sizeof(*list));
    size_t n = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (n >= cap) { cap *= 2; list = realloc(list, cap * sizeof(*list)); }
        log_result_t *r = &list[n++];
        r->timestamp  = sqlite3_column_int64(stmt, 0);
        r->worker_id  = sqlite3_column_int(stmt, 1);
        r->request_id = (uint64_t)sqlite3_column_int64(stmt, 2);
        const char *sid = (const char *)sqlite3_column_text(stmt, 3);
        r->session_id = sid ? strdup(sid) : NULL;
        r->level      = sqlite3_column_int(stmt, 4);
        r->message    = strdup((const char *)sqlite3_column_text(stmt, 5));
    }
    sqlite3_finalize(stmt);

    *count = n;
    return list;
}

void log_result_free(log_result_t *r) {
    if (!r) return;
    free(r->session_id);
    free(r->message);
}

int log_store_get_replay(log_store_t *ls, uint64_t request_id,
                         char **request_data, char **response_data,
                         char **kv_tape, void **random_tape,
                         size_t *random_tape_len,
                         char **date_tape, char **math_random_tape,
                         char **module_tree, char **source_maps) {
    if (!ls->db) return -1;

    sqlite3_stmt *s = ls->get_replay;
    sqlite3_bind_int64(s, 1, (sqlite3_int64)request_id);

    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_reset(s);
        return -1;
    }

    *request_data = strdup((const char *)sqlite3_column_text(s, 0));
    const char *resp = (const char *)sqlite3_column_text(s, 1);
    *response_data = resp ? strdup(resp) : NULL;
    *kv_tape = strdup((const char *)sqlite3_column_text(s, 2));

    const void *blob = sqlite3_column_blob(s, 3);
    int blob_len = sqlite3_column_bytes(s, 3);
    if (blob && blob_len > 0) {
        *random_tape = malloc((size_t)blob_len);
        memcpy(*random_tape, blob, (size_t)blob_len);
        *random_tape_len = (size_t)blob_len;
    } else {
        *random_tape = NULL;
        *random_tape_len = 0;
    }

    *date_tape = strdup((const char *)sqlite3_column_text(s, 4));
    *math_random_tape = strdup((const char *)sqlite3_column_text(s, 5));
    *module_tree = strdup((const char *)sqlite3_column_text(s, 6));
    const char *sm = (const char *)sqlite3_column_text(s, 7);
    *source_maps = sm ? strdup(sm) : NULL;

    sqlite3_reset(s);
    return 0;
}

replay_summary_t *log_store_list_replays(log_store_t *ls, int64_t tenant_id,
                                          int limit, size_t *count) {
    *count = 0;
    if (!ls->db) return NULL;
    if (limit <= 0) limit = 50;

    sqlite3_stmt *s = ls->list_replays;
    sqlite3_bind_int64(s, 1, tenant_id);
    sqlite3_bind_int(s, 2, limit);

    size_t cap = 32;
    replay_summary_t *list = malloc(cap * sizeof(*list));
    size_t n = 0;

    while (sqlite3_step(s) == SQLITE_ROW) {
        if (n >= cap) { cap *= 2; list = realloc(list, cap * sizeof(*list)); }
        list[n].request_id = (uint64_t)sqlite3_column_int64(s, 0);
        list[n].created_at = sqlite3_column_int64(s, 1);
        n++;
    }
    sqlite3_reset(s);

    *count = n;
    return list;
}

int log_store_checkpoint(log_store_t *ls) {
    if (!ls->db) return -1;
    return sqlite3_wal_checkpoint_v2(ls->db, NULL,
                                      SQLITE_CHECKPOINT_PASSIVE,
                                      NULL, NULL);
}
