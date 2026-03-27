#include "kvstore.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct kvstore {
    sqlite3      *db;
    sqlite3_stmt *stmt_get;
    sqlite3_stmt *stmt_put;
    sqlite3_stmt *stmt_put_seq;
    sqlite3_stmt *stmt_del;
    sqlite3_stmt *stmt_range;
    sqlite3_stmt *stmt_begin;
    sqlite3_stmt *stmt_commit;
    sqlite3_stmt *stmt_rollback;
    sqlite3_stmt *stmt_next_seq;
    sqlite3_stmt *stmt_seq_trunc;
    sqlite3_stmt *stmt_delta;
};

static const char SQL_CREATE[] =
    "CREATE TABLE IF NOT EXISTS kv ("
    "  key   TEXT PRIMARY KEY NOT NULL,"
    "  value BLOB NOT NULL,"
    "  seq   INTEGER NOT NULL DEFAULT 0"
    ") WITHOUT ROWID;";

static const char SQL_GET[]      = "SELECT value FROM kv WHERE key = ?;";
static const char SQL_PUT[]      = "INSERT OR REPLACE INTO kv (key, value) VALUES (?, ?);";
static const char SQL_PUT_SEQ[]  = "INSERT OR REPLACE INTO kv (key, value, seq) VALUES (?, ?, ?);";
static const char SQL_DEL[]      = "DELETE FROM kv WHERE key = ?;";
static const char SQL_RANGE[]    = "SELECT key, value FROM kv WHERE key >= ? AND key < ? ORDER BY key LIMIT ?;";
static const char SQL_DELTA[]    = "SELECT key, value FROM kv WHERE seq > ? ORDER BY seq;";
static const char SQL_BEGIN[]    = "BEGIN;";
static const char SQL_COMMIT[]   = "COMMIT;";
static const char SQL_ROLLBACK[] = "ROLLBACK;";

static const char SQL_CREATE_SEQ[] =
    "CREATE TABLE IF NOT EXISTS kv_seq (id INTEGER PRIMARY KEY AUTOINCREMENT);";
static const char SQL_NEXT_SEQ[]   = "INSERT INTO kv_seq DEFAULT VALUES;";
static const char SQL_SEQ_TRUNC[]  = "DELETE FROM kv_seq WHERE id <= ?;";

int kv_open(const char *path, kvstore_t **out) {
    kvstore_t *s = calloc(1, sizeof(*s));
    if (!s) return -2;

    int rc = sqlite3_open_v2(path, &s->db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                             SQLITE_OPEN_NOMUTEX, NULL);
    if (rc != SQLITE_OK) goto fail;

    /* WAL mode for concurrent readers */
    sqlite3_exec(s->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(s->db, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);

    rc = sqlite3_exec(s->db, SQL_CREATE, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_exec(s->db, SQL_CREATE_SEQ, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    if (sqlite3_prepare_v2(s->db, SQL_GET, -1, &s->stmt_get, NULL) != SQLITE_OK)
        goto fail;
    if (sqlite3_prepare_v2(s->db, SQL_PUT, -1, &s->stmt_put, NULL) != SQLITE_OK)
        goto fail;
    if (sqlite3_prepare_v2(s->db, SQL_DEL, -1, &s->stmt_del, NULL) != SQLITE_OK)
        goto fail;
    if (sqlite3_prepare_v2(s->db, SQL_RANGE, -1, &s->stmt_range, NULL) != SQLITE_OK)
        goto fail;
    if (sqlite3_prepare_v2(s->db, SQL_BEGIN, -1, &s->stmt_begin, NULL) != SQLITE_OK)
        goto fail;
    if (sqlite3_prepare_v2(s->db, SQL_COMMIT, -1, &s->stmt_commit, NULL) != SQLITE_OK)
        goto fail;
    if (sqlite3_prepare_v2(s->db, SQL_ROLLBACK, -1, &s->stmt_rollback, NULL) != SQLITE_OK)
        goto fail;
    if (sqlite3_prepare_v2(s->db, SQL_PUT_SEQ, -1, &s->stmt_put_seq, NULL) != SQLITE_OK)
        goto fail;
    if (sqlite3_prepare_v2(s->db, SQL_NEXT_SEQ, -1, &s->stmt_next_seq, NULL) != SQLITE_OK)
        goto fail;
    if (sqlite3_prepare_v2(s->db, SQL_SEQ_TRUNC, -1, &s->stmt_seq_trunc, NULL) != SQLITE_OK)
        goto fail;
    if (sqlite3_prepare_v2(s->db, SQL_DELTA, -1, &s->stmt_delta, NULL) != SQLITE_OK)
        goto fail;

    *out = s;
    return 0;

fail:
    if (s->db) sqlite3_close(s->db);
    free(s);
    return -2;
}

void kv_close(kvstore_t *store) {
    if (!store) return;
    sqlite3_finalize(store->stmt_get);
    sqlite3_finalize(store->stmt_put);
    sqlite3_finalize(store->stmt_del);
    sqlite3_finalize(store->stmt_range);
    sqlite3_finalize(store->stmt_begin);
    sqlite3_finalize(store->stmt_commit);
    sqlite3_finalize(store->stmt_rollback);
    sqlite3_finalize(store->stmt_put_seq);
    sqlite3_finalize(store->stmt_next_seq);
    sqlite3_finalize(store->stmt_seq_trunc);
    sqlite3_finalize(store->stmt_delta);
    sqlite3_close(store->db);
    free(store);
}

int kv_begin(kvstore_t *store) {
    sqlite3_stmt *st = store->stmt_begin;
    sqlite3_reset(st);
    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc == SQLITE_BUSY) return KV_CONFLICT;
    return (rc == SQLITE_DONE) ? 0 : -2;
}

int kv_commit(kvstore_t *store) {
    sqlite3_stmt *st = store->stmt_commit;
    sqlite3_reset(st);
    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc == SQLITE_BUSY) return KV_CONFLICT;
    return (rc == SQLITE_DONE) ? 0 : -2;
}

int kv_rollback(kvstore_t *store) {
    sqlite3_stmt *st = store->stmt_rollback;
    sqlite3_reset(st);
    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    return (rc == SQLITE_DONE) ? 0 : -2;
}

int kv_get(kvstore_t *store, const char *key, void **out_value, size_t *out_len) {
    sqlite3_stmt *st = store->stmt_get;
    sqlite3_reset(st);
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);

    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(st, 0);
        int blen = sqlite3_column_bytes(st, 0);
        void *copy = malloc((size_t)blen);
        if (!copy) { sqlite3_reset(st); return -2; }
        memcpy(copy, blob, (size_t)blen);
        *out_value = copy;
        *out_len = (size_t)blen;
        sqlite3_reset(st);
        return 0;
    }

    sqlite3_reset(st);
    return (rc == SQLITE_DONE) ? -1 : -2;
}

int kv_put(kvstore_t *store, const char *key, const void *value, size_t len) {
    sqlite3_stmt *st = store->stmt_put;
    sqlite3_reset(st);
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_blob(st, 2, value, (int)len, SQLITE_STATIC);

    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    return (rc == SQLITE_DONE) ? 0 : -2;
}

int kv_put_seq(kvstore_t *store, const char *key, const void *value, size_t len,
               uint64_t seq) {
    sqlite3_stmt *st = store->stmt_put_seq;
    sqlite3_reset(st);
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_blob(st, 2, value, (int)len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, (int64_t)seq);

    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    return (rc == SQLITE_DONE) ? 0 : -2;
}

int kv_delete(kvstore_t *store, const char *key) {
    sqlite3_stmt *st = store->stmt_del;
    sqlite3_reset(st);
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);

    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    return (rc == SQLITE_DONE) ? 0 : -2;
}

const char *kv_prefixed_key(const char *prefix, const char *key,
                            char *buf, size_t bufsize) {
    if (!prefix || prefix[0] == '\0') return key;
    int n = snprintf(buf, bufsize, "%s%s", prefix, key);
    if (n < 0 || (size_t)n >= bufsize) return NULL;
    return buf;
}

int kv_range(kvstore_t *store, const char *start, const char *end,
             size_t count, kv_range_result_t *out) {
    out->entries = NULL;
    out->count = 0;

    sqlite3_stmt *st = store->stmt_range;
    sqlite3_reset(st);
    sqlite3_bind_text(st, 1, start, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, end, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, (int64_t)count);

    size_t cap = 16;
    kv_entry_t *entries = malloc(cap * sizeof(kv_entry_t));
    if (!entries) { sqlite3_reset(st); return -2; }

    size_t n = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == cap) {
            cap *= 2;
            kv_entry_t *tmp = realloc(entries, cap * sizeof(kv_entry_t));
            if (!tmp) { goto oom; }
            entries = tmp;
        }

        const char *k = (const char *)sqlite3_column_text(st, 0);
        const void *v = sqlite3_column_blob(st, 1);
        int vlen = sqlite3_column_bytes(st, 1);

        entries[n].key = strdup(k);
        entries[n].value = malloc((size_t)vlen);
        if (!entries[n].key || !entries[n].value) goto oom;
        memcpy(entries[n].value, v, (size_t)vlen);
        entries[n].value_len = (size_t)vlen;
        n++;
    }

    sqlite3_reset(st);
    out->entries = entries;
    out->count = n;
    return 0;

oom:
    for (size_t i = 0; i < n; i++) {
        free(entries[i].key);
        free(entries[i].value);
    }
    free(entries);
    sqlite3_reset(st);
    return -2;
}

void kv_range_free(kv_range_result_t *result) {
    if (!result) return;
    for (size_t i = 0; i < result->count; i++) {
        free(result->entries[i].key);
        free(result->entries[i].value);
    }
    free(result->entries);
    result->entries = NULL;
    result->count = 0;
}

uint64_t kv_next_seq(kvstore_t *store) {
    sqlite3_stmt *st = store->stmt_next_seq;
    sqlite3_reset(st);

    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) return 0;

    return (uint64_t)sqlite3_last_insert_rowid(store->db);
}

int kv_seq_truncate(kvstore_t *store, uint64_t through_seq) {
    sqlite3_stmt *st = store->stmt_seq_trunc;
    sqlite3_reset(st);
    sqlite3_bind_int64(st, 1, (int64_t)through_seq);

    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    return (rc == SQLITE_DONE) ? 0 : -2;
}

int kv_delta(kvstore_t *store, uint64_t after_seq, kv_range_result_t *out) {
    out->entries = NULL;
    out->count = 0;

    sqlite3_stmt *st = store->stmt_delta;
    sqlite3_reset(st);
    sqlite3_bind_int64(st, 1, (int64_t)after_seq);

    size_t cap = 64;
    kv_entry_t *entries = malloc(cap * sizeof(kv_entry_t));
    if (!entries) { sqlite3_reset(st); return -2; }

    size_t n = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == cap) {
            cap *= 2;
            kv_entry_t *tmp = realloc(entries, cap * sizeof(kv_entry_t));
            if (!tmp) goto oom;
            entries = tmp;
        }

        const char *k = (const char *)sqlite3_column_text(st, 0);
        const void *v = sqlite3_column_blob(st, 1);
        int vlen = sqlite3_column_bytes(st, 1);

        entries[n].key = strdup(k);
        entries[n].value = malloc((size_t)vlen);
        if (!entries[n].key || !entries[n].value) goto oom;
        memcpy(entries[n].value, v, (size_t)vlen);
        entries[n].value_len = (size_t)vlen;
        n++;
    }

    sqlite3_reset(st);
    out->entries = entries;
    out->count = n;
    return 0;

oom:
    for (size_t i = 0; i < n; i++) {
        free(entries[i].key);
        free(entries[i].value);
    }
    free(entries);
    sqlite3_reset(st);
    return -2;
}

void kv_disable_auto_checkpoint(kvstore_t *store) {
    sqlite3_wal_autocheckpoint(store->db, 0);
}

int kv_checkpoint(kvstore_t *store) {
    int rc = sqlite3_wal_checkpoint_v2(store->db, NULL,
                                        SQLITE_CHECKPOINT_PASSIVE,
                                        NULL, NULL);
    return (rc == SQLITE_OK) ? 0 : -1;
}
