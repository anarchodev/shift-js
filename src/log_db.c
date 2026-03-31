#include "log_db.h"
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
    "CREATE TABLE IF NOT EXISTS replay_index ("
    "  request_id INTEGER PRIMARY KEY,"
    "  offset     INTEGER NOT NULL"
    ");";

static const char *INSERT_SQL =
    "INSERT INTO logs (timestamp, worker_id, request_id, session_id, level, message) "
    "VALUES (?, ?, ?, ?, ?, ?)";

static const char *REPLAY_INDEX_SQL =
    "INSERT OR REPLACE INTO replay_index (request_id, offset) VALUES (?, ?)";

/* ---- Binary replay record helpers ---- */

static void write_u32(uint8_t *buf, uint32_t v) {
    memcpy(buf, &v, 4);  /* native endian (same machine reads) */
}

static void write_u64(uint8_t *buf, uint64_t v) {
    memcpy(buf, &v, 8);
}

static uint32_t read_u32(const uint8_t *buf) {
    uint32_t v;
    memcpy(&v, buf, 4);
    return v;
}

static uint64_t read_u64(const uint8_t *buf) {
    uint64_t v;
    memcpy(&v, buf, 8);
    return v;
}

/* Write a length-prefixed field to buffer, return bytes written */
static size_t write_field_text(uint8_t *buf, const char *s) {
    uint32_t len = s ? (uint32_t)strlen(s) : 0;
    write_u32(buf, len);
    if (len > 0) memcpy(buf + 4, s, len);
    return 4 + len;
}

static size_t write_field_blob(uint8_t *buf, const void *data, size_t len) {
    write_u32(buf, (uint32_t)len);
    if (len > 0) memcpy(buf + 4, data, len);
    return 4 + len;
}

/* Read a length-prefixed text field, advance *pos. Returns strdup'd string or NULL. */
static char *read_field_text(const uint8_t *data, size_t data_len, size_t *pos) {
    if (*pos + 4 > data_len) return NULL;
    uint32_t len = read_u32(data + *pos);
    *pos += 4;
    if (len == 0) return NULL;
    if (*pos + len > data_len) return NULL;
    char *s = malloc(len + 1);
    memcpy(s, data + *pos, len);
    s[len] = '\0';
    *pos += len;
    return s;
}

/* Read a length-prefixed blob field, advance *pos. Returns malloc'd buffer or NULL. */
static uint8_t *read_field_blob(const uint8_t *data, size_t data_len, size_t *pos, size_t *out_len) {
    *out_len = 0;
    if (*pos + 4 > data_len) return NULL;
    uint32_t len = read_u32(data + *pos);
    *pos += 4;
    if (len == 0) return NULL;
    if (*pos + len > data_len) return NULL;
    uint8_t *buf = malloc(len);
    memcpy(buf, data + *pos, len);
    *out_len = len;
    *pos += len;
    return buf;
}

/* ---- Public API ---- */

int log_db_open(log_db_t *ldb, const char *db_path, const char *replay_path) {
    int rc = sqlite3_open(db_path, &ldb->db);
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

    rc = sqlite3_prepare_v2(ldb->db, REPLAY_INDEX_SQL, -1,
                            &ldb->replay_index_stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "log_db: replay index prepare failed: %s\n",
                sqlite3_errmsg(ldb->db));
        sqlite3_finalize(ldb->insert_stmt);
        ldb->insert_stmt = NULL;
        sqlite3_close(ldb->db);
        ldb->db = NULL;
        return -1;
    }

    /* Open append-only replay log */
    ldb->replay_file = fopen(replay_path, "ab");
    if (!ldb->replay_file) {
        fprintf(stderr, "log_db: replay file open failed: %s\n", replay_path);
        sqlite3_finalize(ldb->insert_stmt);
        sqlite3_finalize(ldb->replay_index_stmt);
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
    if (ldb->replay_index_stmt) {
        sqlite3_finalize(ldb->replay_index_stmt);
        ldb->replay_index_stmt = NULL;
    }
    if (ldb->db) {
        sqlite3_close(ldb->db);
        ldb->db = NULL;
    }
    if (ldb->replay_file) {
        fclose(ldb->replay_file);
        ldb->replay_file = NULL;
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

    /* console.log entries → SQLite */
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

    /* replay capture → binary log + SQLite index */
    if (ldb->replay_file && ldb->replay_index_stmt && rec->req_json) {
        /* Calculate total record size */
        size_t req_json_len = rec->req_json ? strlen(rec->req_json) : 0;
        size_t kv_tape_len = rec->kv_tape ? strlen(rec->kv_tape) : 0;
        size_t date_tape_len = rec->date_tape ? strlen(rec->date_tape) : 0;
        size_t math_random_tape_len = rec->math_random_tape ? strlen(rec->math_random_tape) : 0;
        size_t module_tree_len = rec->module_tree ? strlen(rec->module_tree) : 0;

        /* 8B request_id + 6 fields * (4B len + data) */
        size_t payload_len = 8
            + 4 + req_json_len
            + 4 + kv_tape_len
            + 4 + rec->random_tape_len
            + 4 + date_tape_len
            + 4 + math_random_tape_len
            + 4 + module_tree_len;
        size_t total = 4 + payload_len;  /* 4B total_len header */

        uint8_t *buf = malloc(total);
        if (!buf) return;

        size_t pos = 0;
        write_u32(buf + pos, (uint32_t)payload_len); pos += 4;
        write_u64(buf + pos, rec->request_id); pos += 8;
        pos += write_field_text(buf + pos, rec->req_json);
        pos += write_field_text(buf + pos, rec->kv_tape);
        pos += write_field_blob(buf + pos, rec->random_tape, rec->random_tape_len);
        pos += write_field_text(buf + pos, rec->date_tape);
        pos += write_field_text(buf + pos, rec->math_random_tape);
        pos += write_field_text(buf + pos, rec->module_tree);

        /* Write to file and index */
        long offset = ftell(ldb->replay_file);
        if (fwrite(buf, 1, total, ldb->replay_file) == total) {
            sqlite3_stmt *s = ldb->replay_index_stmt;
            sqlite3_bind_int64(s, 1, (sqlite3_int64)rec->request_id);
            sqlite3_bind_int64(s, 2, (sqlite3_int64)offset);
            sqlite3_step(s);
            sqlite3_reset(s);
        }
        free(buf);
    }
}

void log_db_flush_commit(log_db_t *ldb) {
    if (ldb->db)
        sqlite3_exec(ldb->db, "COMMIT", NULL, NULL, NULL);
    if (ldb->replay_file)
        fflush(ldb->replay_file);
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

/* ---- Read path ---- */

int log_db_get_replay(sqlite3 *db, FILE *replay_file, uint64_t request_id,
                      char **request_data,
                      char **kv_tape,
                      uint8_t **random_tape, size_t *random_tape_len,
                      char **date_tape,
                      char **math_random_tape,
                      char **module_tree) {
    if (!db || !replay_file) return -1;

    /* Look up offset in index */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT offset FROM replay_index WHERE request_id = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)request_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;
    }
    long offset = (long)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    /* Read record from binary log */
    fseek(replay_file, offset, SEEK_SET);

    uint8_t hdr[4];
    if (fread(hdr, 1, 4, replay_file) != 4) return -1;
    uint32_t payload_len = read_u32(hdr);

    uint8_t *data = malloc(payload_len);
    if (!data) return -1;
    if (fread(data, 1, payload_len, replay_file) != payload_len) {
        free(data);
        return -1;
    }

    /* Parse fields */
    size_t pos = 0;
    uint64_t rid = read_u64(data + pos); pos += 8;
    (void)rid;  /* already known */

    *request_data = read_field_text(data, payload_len, &pos);
    *kv_tape = read_field_text(data, payload_len, &pos);
    *random_tape = read_field_blob(data, payload_len, &pos, random_tape_len);
    *date_tape = read_field_text(data, payload_len, &pos);
    *math_random_tape = read_field_text(data, payload_len, &pos);
    *module_tree = read_field_text(data, payload_len, &pos);

    free(data);
    return 0;
}

int log_db_list_requests(sqlite3 *db, FILE *replay_file, int limit,
                         log_db_request_entry_t **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;
    if (!db || !replay_file) return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT request_id, offset FROM replay_index ORDER BY request_id DESC LIMIT ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_int(stmt, 1, limit);

    size_t cap = 64;
    log_db_request_entry_t *entries = malloc(cap * sizeof(*entries));
    size_t count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            entries = realloc(entries, cap * sizeof(*entries));
        }
        uint64_t rid = (uint64_t)sqlite3_column_int64(stmt, 0);
        long offset = (long)sqlite3_column_int64(stmt, 1);

        entries[count].request_id = rid;
        entries[count].request_data = NULL;

        /* Read just the req_json field from the binary log */
        long saved = ftell(replay_file);
        fseek(replay_file, offset, SEEK_SET);

        uint8_t hdr[4];
        if (fread(hdr, 1, 4, replay_file) == 4) {
            uint32_t payload_len = read_u32(hdr);
            /* Skip past: request_id (8B) */
            fseek(replay_file, 8, SEEK_CUR);
            /* Read req_json length + data */
            uint8_t len_buf[4];
            if (fread(len_buf, 1, 4, replay_file) == 4) {
                uint32_t json_len = read_u32(len_buf);
                if (json_len > 0 && json_len <= payload_len) {
                    char *json = malloc(json_len + 1);
                    if (json && fread(json, 1, json_len, replay_file) == json_len) {
                        json[json_len] = '\0';
                        entries[count].request_data = json;
                    } else {
                        free(json);
                    }
                }
            }
        }
        fseek(replay_file, saved, SEEK_SET);
        count++;
    }

    sqlite3_finalize(stmt);
    *out = entries;
    *out_count = count;
    return 0;
}

void log_db_free_request_entries(log_db_request_entry_t *entries, size_t count) {
    for (size_t i = 0; i < count; i++)
        free(entries[i].request_data);
    free(entries);
}

int log_db_reader_open(log_db_reader_t *r, int num_workers) {
    r->dbs = calloc((size_t)num_workers, sizeof(sqlite3 *));
    r->replay_files = calloc((size_t)num_workers, sizeof(FILE *));
    if (!r->dbs || !r->replay_files) {
        free(r->dbs); free(r->replay_files);
        r->dbs = NULL; r->replay_files = NULL;
        return -1;
    }
    r->count = num_workers;

    for (int i = 0; i < num_workers; i++) {
        char path[64];
        snprintf(path, sizeof(path), "logs_%d.db", i);
        int rc = sqlite3_open_v2(path, &r->dbs[i],
                                 SQLITE_OPEN_READONLY | SQLITE_OPEN_WAL, NULL);
        if (rc != SQLITE_OK) {
            if (r->dbs[i]) sqlite3_close(r->dbs[i]);
            r->dbs[i] = NULL;
            continue;
        }
        sqlite3_busy_timeout(r->dbs[i], 1000);

        snprintf(path, sizeof(path), "replay_%d.log", i);
        r->replay_files[i] = fopen(path, "rb");
        /* May not exist yet — that's OK */
    }
    return 0;
}

void log_db_reader_close(log_db_reader_t *r) {
    if (!r->dbs) return;
    for (int i = 0; i < r->count; i++) {
        if (r->dbs[i]) sqlite3_close(r->dbs[i]);
        if (r->replay_files[i]) fclose(r->replay_files[i]);
    }
    free(r->dbs);
    free(r->replay_files);
    r->dbs = NULL;
    r->replay_files = NULL;
    r->count = 0;
}
