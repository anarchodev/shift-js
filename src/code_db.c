#include "code_db.h"

#include <sqlite3.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

struct code_db {
    sqlite3 *db;
    sqlite3_stmt *put_file;
    sqlite3_stmt *get_file;
    sqlite3_stmt *tree_put;
    sqlite3_stmt *tree_delete;
    sqlite3_stmt *tree_clear;
    sqlite3_stmt *tree_list;
    sqlite3_stmt *tree_get;       /* JOIN: tree entry → file content */
    sqlite3_stmt *tree_copy;      /* copy working tree → snapshot */
    sqlite3_stmt *deploy_insert;
};

static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS files ("
    "  sha1    TEXT PRIMARY KEY NOT NULL,"
    "  content BLOB NOT NULL"
    ") WITHOUT ROWID;"
    "CREATE TABLE IF NOT EXISTS trees ("
    "  database_id TEXT NOT NULL,"
    "  tree_hash   TEXT NOT NULL DEFAULT '',"
    "  path        TEXT NOT NULL,"
    "  sha1        TEXT NOT NULL,"
    "  PRIMARY KEY (database_id, tree_hash, path)"
    ") WITHOUT ROWID;"
    "CREATE TABLE IF NOT EXISTS deployments ("
    "  database_id TEXT NOT NULL,"
    "  tree_hash   TEXT NOT NULL,"
    "  deployed_at INTEGER NOT NULL,"
    "  PRIMARY KEY (database_id, tree_hash)"
    ") WITHOUT ROWID;";

static void sha1_hex(const void *data, size_t len, char out[41]) {
    unsigned char hash[20];
    EVP_Digest(data, len, hash, NULL, EVP_sha1(), NULL);
    for (int i = 0; i < 20; i++)
        snprintf(out + i * 2, 3, "%02x", hash[i]);
    out[40] = '\0';
}

int code_db_open(const char *path, code_db_t **out) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                             SQLITE_OPEN_NOMUTEX, NULL);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }

    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);

    char *err = NULL;
    rc = sqlite3_exec(db, SCHEMA, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "code_db schema: %s\n", err ? err : "unknown");
        sqlite3_free(err);
        sqlite3_close(db);
        return -1;
    }

    code_db_t *cdb = calloc(1, sizeof(*cdb));
    if (!cdb) { sqlite3_close(db); return -1; }
    cdb->db = db;

#define PREP(field, sql) do { \
    if (sqlite3_prepare_v2(db, sql, -1, &cdb->field, NULL) != SQLITE_OK) { \
        fprintf(stderr, "code_db prepare(%s): %s\n", #field, sqlite3_errmsg(db)); \
        code_db_close(cdb); return -1; \
    } \
} while (0)

    PREP(put_file,
         "INSERT OR IGNORE INTO files (sha1, content) VALUES (?, ?)");
    PREP(get_file,
         "SELECT content FROM files WHERE sha1 = ?");
    PREP(tree_put,
         "INSERT OR REPLACE INTO trees (database_id, tree_hash, path, sha1)"
         " VALUES (?, '', ?, ?)");
    PREP(tree_delete,
         "DELETE FROM trees WHERE database_id = ? AND tree_hash = '' AND path = ?");
    PREP(tree_clear,
         "DELETE FROM trees WHERE database_id = ? AND tree_hash = ''");
    PREP(tree_list,
         "SELECT path, sha1 FROM trees"
         " WHERE database_id = ? AND tree_hash = ? ORDER BY path");
    PREP(tree_get,
         "SELECT f.content FROM trees t JOIN files f ON t.sha1 = f.sha1"
         " WHERE t.database_id = ? AND t.tree_hash = ? AND t.path = ?");
    PREP(tree_copy,
         "INSERT INTO trees (database_id, tree_hash, path, sha1)"
         " SELECT database_id, ?, path, sha1 FROM trees"
         " WHERE database_id = ? AND tree_hash = ''");
    PREP(deploy_insert,
         "INSERT OR REPLACE INTO deployments (database_id, tree_hash, deployed_at)"
         " VALUES (?, ?, ?)");

#undef PREP

    *out = cdb;
    return 0;
}

void code_db_close(code_db_t *db) {
    if (!db) return;
    sqlite3_finalize(db->put_file);
    sqlite3_finalize(db->get_file);
    sqlite3_finalize(db->tree_put);
    sqlite3_finalize(db->tree_delete);
    sqlite3_finalize(db->tree_clear);
    sqlite3_finalize(db->tree_list);
    sqlite3_finalize(db->tree_get);
    sqlite3_finalize(db->tree_copy);
    sqlite3_finalize(db->deploy_insert);
    if (db->db) sqlite3_close(db->db);
    free(db);
}

int code_db_put_file(code_db_t *db, const char *database_id,
                     const char *path, const void *content, size_t len) {
    char hash[41];
    sha1_hex(content, len, hash);

    /* Insert content blob (idempotent). */
    sqlite3_bind_text(db->put_file, 1, hash, 40, SQLITE_STATIC);
    sqlite3_bind_blob(db->put_file, 2, content, (int)len, SQLITE_STATIC);
    int rc = sqlite3_step(db->put_file);
    sqlite3_reset(db->put_file);
    if (rc != SQLITE_DONE) return -1;

    /* Update working tree entry. */
    sqlite3_bind_text(db->tree_put, 1, database_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(db->tree_put, 2, path, -1, SQLITE_STATIC);
    sqlite3_bind_text(db->tree_put, 3, hash, 40, SQLITE_STATIC);
    rc = sqlite3_step(db->tree_put);
    sqlite3_reset(db->tree_put);
    return rc == SQLITE_DONE ? 0 : -1;
}

int code_db_get_file(code_db_t *db, const char *sha1,
                     void **out, size_t *out_len) {
    sqlite3_bind_text(db->get_file, 1, sha1, -1, SQLITE_STATIC);
    int rc = sqlite3_step(db->get_file);
    if (rc != SQLITE_ROW) {
        sqlite3_reset(db->get_file);
        return -1;
    }
    size_t len = (size_t)sqlite3_column_bytes(db->get_file, 0);
    const void *blob = sqlite3_column_blob(db->get_file, 0);
    void *copy = malloc(len);
    if (!copy) { sqlite3_reset(db->get_file); return -1; }
    memcpy(copy, blob, len);
    *out = copy;
    *out_len = len;
    sqlite3_reset(db->get_file);
    return 0;
}

int code_db_tree_list(code_db_t *db, const char *database_id,
                      const char *tree_hash, code_tree_t *out) {
    out->entries = NULL;
    out->count = 0;

    sqlite3_bind_text(db->tree_list, 1, database_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(db->tree_list, 2, tree_hash, -1, SQLITE_STATIC);

    size_t cap = 0;
    while (sqlite3_step(db->tree_list) == SQLITE_ROW) {
        if (out->count >= cap) {
            cap = cap ? cap * 2 : 64;
            code_tree_entry_t *ne = realloc(out->entries, cap * sizeof(*ne));
            if (!ne) { sqlite3_reset(db->tree_list); return -1; }
            out->entries = ne;
        }
        code_tree_entry_t *e = &out->entries[out->count++];
        e->path = strdup((const char *)sqlite3_column_text(db->tree_list, 0));
        e->sha1 = strdup((const char *)sqlite3_column_text(db->tree_list, 1));
    }
    sqlite3_reset(db->tree_list);
    return 0;
}

void code_db_tree_free(code_tree_t *tree) {
    for (size_t i = 0; i < tree->count; i++) {
        free(tree->entries[i].path);
        free(tree->entries[i].sha1);
    }
    free(tree->entries);
    tree->entries = NULL;
    tree->count = 0;
}

int code_db_tree_get(code_db_t *db, const char *database_id,
                     const char *tree_hash, const char *path,
                     void **out, size_t *out_len) {
    sqlite3_bind_text(db->tree_get, 1, database_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(db->tree_get, 2, tree_hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(db->tree_get, 3, path, -1, SQLITE_STATIC);
    int rc = sqlite3_step(db->tree_get);
    if (rc != SQLITE_ROW) {
        sqlite3_reset(db->tree_get);
        return -1;
    }
    size_t len = (size_t)sqlite3_column_bytes(db->tree_get, 0);
    const void *blob = sqlite3_column_blob(db->tree_get, 0);
    void *copy = malloc(len);
    if (!copy) { sqlite3_reset(db->tree_get); return -1; }
    memcpy(copy, blob, len);
    *out = copy;
    *out_len = len;
    sqlite3_reset(db->tree_get);
    return 0;
}

char *code_db_snapshot(code_db_t *db, const char *database_id) {
    /* List working tree entries (sorted by path). */
    code_tree_t tree;
    if (code_db_tree_list(db, database_id, "", &tree) != 0) return NULL;
    if (tree.count == 0) { code_db_tree_free(&tree); return NULL; }

    /* Build Merkle hash input: "path\0sha1\n" for each entry. */
    size_t buf_len = 0;
    for (size_t i = 0; i < tree.count; i++)
        buf_len += strlen(tree.entries[i].path) + 1 + 40 + 1;

    char *buf = malloc(buf_len);
    if (!buf) { code_db_tree_free(&tree); return NULL; }

    size_t pos = 0;
    for (size_t i = 0; i < tree.count; i++) {
        size_t plen = strlen(tree.entries[i].path);
        memcpy(buf + pos, tree.entries[i].path, plen);
        pos += plen;
        buf[pos++] = '\0';
        memcpy(buf + pos, tree.entries[i].sha1, 40);
        pos += 40;
        buf[pos++] = '\n';
    }

    char hash[41];
    sha1_hex(buf, buf_len, hash);
    free(buf);
    code_db_tree_free(&tree);

    /* Copy working tree entries to the new tree hash within a transaction. */
    sqlite3_exec(db->db, "BEGIN", NULL, NULL, NULL);

    sqlite3_bind_text(db->tree_copy, 1, hash, 40, SQLITE_STATIC);
    sqlite3_bind_text(db->tree_copy, 2, database_id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(db->tree_copy);
    sqlite3_reset(db->tree_copy);
    if (rc != SQLITE_DONE) {
        sqlite3_exec(db->db, "ROLLBACK", NULL, NULL, NULL);
        return NULL;
    }

    /* Record deployment. */
    sqlite3_bind_text(db->deploy_insert, 1, database_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(db->deploy_insert, 2, hash, 40, SQLITE_STATIC);
    sqlite3_bind_int64(db->deploy_insert, 3, (int64_t)time(NULL));
    rc = sqlite3_step(db->deploy_insert);
    sqlite3_reset(db->deploy_insert);
    if (rc != SQLITE_DONE) {
        sqlite3_exec(db->db, "ROLLBACK", NULL, NULL, NULL);
        return NULL;
    }

    sqlite3_exec(db->db, "COMMIT", NULL, NULL, NULL);

    char *result = malloc(41);
    memcpy(result, hash, 41);
    return result;
}

int code_db_tree_delete(code_db_t *db, const char *database_id,
                        const char *path) {
    sqlite3_bind_text(db->tree_delete, 1, database_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(db->tree_delete, 2, path, -1, SQLITE_STATIC);
    int rc = sqlite3_step(db->tree_delete);
    sqlite3_reset(db->tree_delete);
    return rc == SQLITE_DONE ? 0 : -1;
}

int code_db_tree_clear(code_db_t *db, const char *database_id) {
    sqlite3_bind_text(db->tree_clear, 1, database_id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(db->tree_clear);
    sqlite3_reset(db->tree_clear);
    return rc == SQLITE_DONE ? 0 : -1;
}
