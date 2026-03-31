#include "log_db.h"
#include <stdio.h>
#include <stdlib.h>
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
    "CREATE INDEX IF NOT EXISTS idx_logs_ts ON logs(timestamp);"
    "CREATE TABLE IF NOT EXISTS replay_captures ("
    "  request_id       INTEGER PRIMARY KEY,"
    "  request_data     TEXT NOT NULL,"
    "  response_data    TEXT,"
    "  kv_tape          TEXT NOT NULL,"
    "  random_tape      BLOB,"
    "  date_tape        TEXT NOT NULL,"
    "  math_random_tape TEXT NOT NULL,"
    "  module_tree      TEXT NOT NULL,"
    "  source_maps      TEXT"
    ");";

static const char *INSERT_SQL =
    "INSERT INTO logs (timestamp, worker_id, request_id, session_id, level, message) "
    "VALUES (?, ?, ?, ?, ?, ?)";

static const char *REPLAY_INSERT_SQL =
    "INSERT OR REPLACE INTO replay_captures "
    "(request_id, request_data, response_data, kv_tape, random_tape, "
    "date_tape, math_random_tape, module_tree, source_maps) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";

int log_db_open(log_db_t *ldb, const char *path) {
    int rc = sqlite3_open(path, &ldb->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "log_db: open failed: %s\n", sqlite3_errmsg(ldb->db));
        return -1;
    }

    /* WAL mode, no fsync. Each worker has its own DB so no write contention. */
    sqlite3_busy_timeout(ldb->db, 5000);
    sqlite3_exec(ldb->db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(ldb->db, "PRAGMA synchronous=OFF", NULL, NULL, NULL);

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

    /* Prepare insert statements */
    rc = sqlite3_prepare_v2(ldb->db, INSERT_SQL, -1, &ldb->insert_stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "log_db: prepare failed: %s\n", sqlite3_errmsg(ldb->db));
        sqlite3_close(ldb->db);
        ldb->db = NULL;
        return -1;
    }

    rc = sqlite3_prepare_v2(ldb->db, REPLAY_INSERT_SQL, -1,
                            &ldb->replay_insert_stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "log_db: replay prepare failed: %s\n",
                sqlite3_errmsg(ldb->db));
        sqlite3_finalize(ldb->insert_stmt);
        ldb->insert_stmt = NULL;
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
    if (ldb->replay_insert_stmt) {
        sqlite3_finalize(ldb->replay_insert_stmt);
        ldb->replay_insert_stmt = NULL;
    }
    if (ldb->db) {
        sqlite3_close(ldb->db);
        ldb->db = NULL;
    }
}

void sjs_log_record_free(sjs_log_record_t *rec) {
    for (uint32_t i = 0; i < rec->log_count; i++)
        free((void *)rec->log_entries[i].msg);
    free(rec->log_entries);
    free(rec->session_id);
    free(rec->req_json);
    free(rec->kv_tape);
    free(rec->random_tape);
    free(rec->date_tape);
    free(rec->math_random_tape);
    free(rec->module_tree);
}

void log_db_flush_begin(log_db_t *ldb) {
    if (ldb->db)
        sqlite3_exec(ldb->db, "BEGIN", NULL, NULL, NULL);
}

void log_db_flush_one(log_db_t *ldb, sjs_log_record_t *rec) {
    if (!ldb->db) return;

    /* console.log entries */
    if (ldb->insert_stmt) {
        for (uint32_t i = 0; i < rec->log_count; i++) {
            const log_pending_t *e = &rec->log_entries[i];
            sqlite3_stmt *s = ldb->insert_stmt;
            sqlite3_bind_int64(s, 1, (sqlite3_int64)e->timestamp_ns);
            sqlite3_bind_int(s, 2, rec->worker_id);
            sqlite3_bind_int64(s, 3, (sqlite3_int64)rec->request_id);
            if (rec->session_id)
                sqlite3_bind_text(s, 4, rec->session_id, -1, SQLITE_STATIC);
            else
                sqlite3_bind_null(s, 4);
            sqlite3_bind_int(s, 5, (int)e->level);
            sqlite3_bind_text(s, 6, e->msg, (int)e->msg_len, SQLITE_STATIC);
            sqlite3_step(s);
            sqlite3_reset(s);
        }
    }

    /* replay capture */
    if (ldb->replay_insert_stmt && rec->req_json) {
        sqlite3_stmt *s = ldb->replay_insert_stmt;
        sqlite3_bind_int64(s, 1, (sqlite3_int64)rec->request_id);
        sqlite3_bind_text(s, 2, rec->req_json, -1, SQLITE_STATIC);
        sqlite3_bind_null(s, 3);  /* response_data — replay recreates it */
        sqlite3_bind_text(s, 4, rec->kv_tape, -1, SQLITE_STATIC);
        if (rec->random_tape && rec->random_tape_len > 0)
            sqlite3_bind_blob(s, 5, rec->random_tape,
                              (int)rec->random_tape_len, SQLITE_STATIC);
        else
            sqlite3_bind_null(s, 5);
        sqlite3_bind_text(s, 6, rec->date_tape, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 7, rec->math_random_tape, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 8, rec->module_tree, -1, SQLITE_STATIC);
        sqlite3_bind_null(s, 9);  /* source_maps */
        sqlite3_step(s);
        sqlite3_reset(s);
    }
}

void log_db_flush_commit(log_db_t *ldb) {
    if (ldb->db)
        sqlite3_exec(ldb->db, "COMMIT", NULL, NULL, NULL);
}

int log_db_flush_records(log_db_t *ldb, sjs_log_record_t *records, size_t count) {
    if (!ldb->db || count == 0) return 0;

    log_db_flush_begin(ldb);
    for (size_t r = 0; r < count; r++)
        log_db_flush_one(ldb, &records[r]);
    log_db_flush_commit(ldb);
    return 0;
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

int log_db_flush_replay(log_db_t *ldb, uint64_t request_id,
                        const char *request_data,
                        const char *response_data,
                        const char *kv_tape,
                        const uint8_t *random_tape, size_t random_tape_len,
                        const char *date_tape,
                        const char *math_random_tape,
                        const char *module_tree,
                        const char *source_maps) {
    if (!ldb->db || !ldb->replay_insert_stmt)
        return -1;

    sqlite3_exec(ldb->db, "BEGIN", NULL, NULL, NULL);

    sqlite3_stmt *s = ldb->replay_insert_stmt;
    sqlite3_bind_int64(s, 1, (sqlite3_int64)request_id);
    sqlite3_bind_text(s, 2, request_data, -1, SQLITE_STATIC);
    if (response_data)
        sqlite3_bind_text(s, 3, response_data, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 3);
    sqlite3_bind_text(s, 4, kv_tape, -1, SQLITE_STATIC);
    if (random_tape && random_tape_len > 0)
        sqlite3_bind_blob(s, 5, random_tape, (int)random_tape_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 5);
    sqlite3_bind_text(s, 6, date_tape, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 7, math_random_tape, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 8, module_tree, -1, SQLITE_STATIC);
    if (source_maps)
        sqlite3_bind_text(s, 9, source_maps, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 9);

    sqlite3_step(s);
    sqlite3_reset(s);

    sqlite3_exec(ldb->db, "COMMIT", NULL, NULL, NULL);
    return 0;
}

int log_db_get_replay(log_db_t *ldb, uint64_t request_id,
                      char **request_data,
                      char **response_data,
                      char **kv_tape,
                      uint8_t **random_tape, size_t *random_tape_len,
                      char **date_tape,
                      char **math_random_tape,
                      char **module_tree,
                      char **source_maps) {
    if (!ldb->db) return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(ldb->db,
        "SELECT request_data, response_data, kv_tape, random_tape, "
        "date_tape, math_random_tape, module_tree, source_maps "
        "FROM replay_captures WHERE request_id = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)request_id);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;
    }

    *request_data = strdup((const char *)sqlite3_column_text(stmt, 0));
    const char *resp = (const char *)sqlite3_column_text(stmt, 1);
    *response_data = resp ? strdup(resp) : NULL;
    *kv_tape = strdup((const char *)sqlite3_column_text(stmt, 2));

    const void *blob = sqlite3_column_blob(stmt, 3);
    int blob_len = sqlite3_column_bytes(stmt, 3);
    if (blob && blob_len > 0) {
        *random_tape = malloc((size_t)blob_len);
        memcpy(*random_tape, blob, (size_t)blob_len);
        *random_tape_len = (size_t)blob_len;
    } else {
        *random_tape = NULL;
        *random_tape_len = 0;
    }

    *date_tape = strdup((const char *)sqlite3_column_text(stmt, 4));
    *math_random_tape = strdup((const char *)sqlite3_column_text(stmt, 5));
    *module_tree = strdup((const char *)sqlite3_column_text(stmt, 6));
    const char *sm = (const char *)sqlite3_column_text(stmt, 7);
    *source_maps = sm ? strdup(sm) : NULL;

    sqlite3_finalize(stmt);
    return 0;
}

int log_db_list_requests(log_db_t *ldb, int limit,
                         log_db_request_entry_t **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;
    if (!ldb->db) return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(ldb->db,
        "SELECT request_id, request_data, response_data "
        "FROM replay_captures ORDER BY request_id DESC LIMIT ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_int(stmt, 1, limit);

    /* First pass: count rows */
    size_t cap = 64;
    log_db_request_entry_t *entries = malloc(cap * sizeof(*entries));
    size_t count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            entries = realloc(entries, cap * sizeof(*entries));
        }
        entries[count].request_id = (uint64_t)sqlite3_column_int64(stmt, 0);
        entries[count].request_data =
            strdup((const char *)sqlite3_column_text(stmt, 1));
        const char *resp = (const char *)sqlite3_column_text(stmt, 2);
        entries[count].response_data = resp ? strdup(resp) : NULL;
        count++;
    }

    sqlite3_finalize(stmt);
    *out = entries;
    *out_count = count;
    return 0;
}

void log_db_free_request_entries(log_db_request_entry_t *entries, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(entries[i].request_data);
        free(entries[i].response_data);
    }
    free(entries);
}

int log_db_reader_open(log_db_reader_t *r, int num_workers) {
    r->dbs = calloc((size_t)num_workers, sizeof(sqlite3 *));
    if (!r->dbs) return -1;
    r->count = num_workers;

    for (int i = 0; i < num_workers; i++) {
        char path[64];
        snprintf(path, sizeof(path), "logs_%d.db", i);
        int rc = sqlite3_open_v2(path, &r->dbs[i],
                                 SQLITE_OPEN_READONLY | SQLITE_OPEN_WAL, NULL);
        if (rc != SQLITE_OK) {
            /* DB may not exist yet if worker hasn't started — that's OK */
            if (r->dbs[i]) sqlite3_close(r->dbs[i]);
            r->dbs[i] = NULL;
            continue;
        }
        sqlite3_busy_timeout(r->dbs[i], 1000);
    }
    return 0;
}

void log_db_reader_close(log_db_reader_t *r) {
    if (!r->dbs) return;
    for (int i = 0; i < r->count; i++) {
        if (r->dbs[i]) sqlite3_close(r->dbs[i]);
    }
    free(r->dbs);
    r->dbs = NULL;
    r->count = 0;
}


