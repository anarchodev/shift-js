#include "code_db.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ======================================================================
 * Schema
 * ====================================================================== */

static const char *SCHEMA_SQL =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"

    "CREATE TABLE IF NOT EXISTS modules ("
    "  id INTEGER PRIMARY KEY,"
    "  tenant_id INTEGER NOT NULL DEFAULT 0,"
    "  path TEXT NOT NULL,"
    "  source BLOB NOT NULL,"
    "  extension TEXT NOT NULL,"
    "  content_hash TEXT NOT NULL,"
    "  updated_at INTEGER NOT NULL,"
    "  UNIQUE(tenant_id, path)"
    ");"

    "CREATE TABLE IF NOT EXISTS source_blobs ("
    "  content_hash TEXT PRIMARY KEY,"
    "  source BLOB NOT NULL,"
    "  extension TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL"
    ");"

    "CREATE TABLE IF NOT EXISTS trees ("
    "  hash TEXT PRIMARY KEY,"
    "  tenant_id INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL,"
    "  module_count INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_trees_tenant"
    "  ON trees(tenant_id, created_at DESC);"

    "CREATE TABLE IF NOT EXISTS tree_modules ("
    "  tree_hash TEXT NOT NULL REFERENCES trees(hash),"
    "  module_path TEXT NOT NULL,"
    "  bytecode BLOB NOT NULL,"
    "  source_map TEXT,"
    "  content_hash TEXT NOT NULL,"
    "  PRIMARY KEY (tree_hash, module_path)"
    ");";

/* ======================================================================
 * Prepared statement SQL
 * ====================================================================== */

#define SQL_PUT_MODULE \
    "INSERT INTO modules (tenant_id, path, source, extension, content_hash, updated_at) " \
    "VALUES (?, ?, ?, ?, ?, ?) " \
    "ON CONFLICT(tenant_id, path) DO UPDATE SET " \
    "source=excluded.source, extension=excluded.extension, " \
    "content_hash=excluded.content_hash, updated_at=excluded.updated_at"

#define SQL_GET_MODULE \
    "SELECT path, extension, source, content_hash, updated_at " \
    "FROM modules WHERE tenant_id=? AND path=?"

#define SQL_DELETE_MODULE \
    "DELETE FROM modules WHERE tenant_id=? AND path=?"

#define SQL_LIST_MODULES \
    "SELECT path, extension, length(source), content_hash, updated_at " \
    "FROM modules WHERE tenant_id=? ORDER BY path"

#define SQL_PUT_BLOB \
    "INSERT OR IGNORE INTO source_blobs (content_hash, source, extension, created_at) " \
    "VALUES (?, ?, ?, ?)"

#define SQL_GET_BLOB \
    "SELECT source, extension FROM source_blobs WHERE content_hash=?"

#define SQL_PUT_TREE \
    "INSERT INTO trees (hash, tenant_id, created_at, module_count) VALUES (?, ?, ?, ?)"

#define SQL_PUT_TREE_MODULE \
    "INSERT INTO tree_modules (tree_hash, module_path, bytecode, source_map, content_hash) " \
    "VALUES (?, ?, ?, ?, ?)"

#define SQL_LIST_TREES \
    "SELECT hash, created_at, module_count FROM trees " \
    "WHERE tenant_id=? ORDER BY created_at DESC LIMIT 100"

#define SQL_GET_TREE_MODULES \
    "SELECT module_path, bytecode, source_map, content_hash " \
    "FROM tree_modules WHERE tree_hash=? ORDER BY module_path"

#define SQL_GET_SOURCEMAP \
    "SELECT source_map FROM tree_modules WHERE tree_hash=? AND module_path=?"

/* ======================================================================
 * Open / close
 * ====================================================================== */

static int prepare(sqlite3 *db, const char *sql, sqlite3_stmt **out) {
    int rc = sqlite3_prepare_v2(db, sql, -1, out, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "code_db: prepare failed: %s\n  SQL: %s\n",
                sqlite3_errmsg(db), sql);
        return -1;
    }
    return 0;
}

int code_db_open(code_db_t *db, const char *path) {
    memset(db, 0, sizeof(*db));

    int rc = sqlite3_open(path, &db->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "code_db: open failed: %s\n", sqlite3_errmsg(db->db));
        sqlite3_close(db->db);
        db->db = NULL;
        return -1;
    }

    char *err = NULL;
    rc = sqlite3_exec(db->db, SCHEMA_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "code_db: schema failed: %s\n", err);
        sqlite3_free(err);
        sqlite3_close(db->db);
        db->db = NULL;
        return -1;
    }

    if (prepare(db->db, SQL_PUT_MODULE, &db->put_module) ||
        prepare(db->db, SQL_GET_MODULE, &db->get_module) ||
        prepare(db->db, SQL_DELETE_MODULE, &db->delete_module) ||
        prepare(db->db, SQL_LIST_MODULES, &db->list_modules) ||
        prepare(db->db, SQL_PUT_BLOB, &db->put_blob) ||
        prepare(db->db, SQL_GET_BLOB, &db->get_blob) ||
        prepare(db->db, SQL_PUT_TREE, &db->put_tree) ||
        prepare(db->db, SQL_PUT_TREE_MODULE, &db->put_tree_module) ||
        prepare(db->db, SQL_LIST_TREES, &db->list_trees) ||
        prepare(db->db, SQL_GET_TREE_MODULES, &db->get_tree_modules) ||
        prepare(db->db, SQL_GET_SOURCEMAP, &db->get_sourcemap)) {
        code_db_close(db);
        return -1;
    }

    return 0;
}

static void finalize(sqlite3_stmt **s) {
    if (*s) { sqlite3_finalize(*s); *s = NULL; }
}

void code_db_close(code_db_t *db) {
    if (!db) return;
    finalize(&db->put_module);
    finalize(&db->get_module);
    finalize(&db->delete_module);
    finalize(&db->list_modules);
    finalize(&db->put_blob);
    finalize(&db->get_blob);
    finalize(&db->put_tree);
    finalize(&db->put_tree_module);
    finalize(&db->list_trees);
    finalize(&db->get_tree_modules);
    finalize(&db->get_sourcemap);
    if (db->db) { sqlite3_close(db->db); db->db = NULL; }
}

/* ======================================================================
 * Module CRUD
 * ====================================================================== */

int code_db_put_module(code_db_t *db, int64_t tenant_id,
                       const char *path, const char *extension,
                       const void *source, size_t source_len,
                       const char *content_hash) {
    if (!db->db) return -1;
    int64_t now = (int64_t)time(NULL);

    sqlite3_exec(db->db, "BEGIN", NULL, NULL, NULL);

    /* Upsert module */
    sqlite3_stmt *s = db->put_module;
    sqlite3_bind_int64(s, 1, tenant_id);
    sqlite3_bind_text(s, 2, path, -1, SQLITE_STATIC);
    sqlite3_bind_blob(s, 3, source, (int)source_len, SQLITE_STATIC);
    sqlite3_bind_text(s, 4, extension, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 5, content_hash, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 6, now);

    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    if (rc != SQLITE_DONE) {
        sqlite3_exec(db->db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }

    /* Insert source blob (deduplicated by hash, INSERT OR IGNORE) */
    s = db->put_blob;
    sqlite3_bind_text(s, 1, content_hash, -1, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, source, (int)source_len, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, extension, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 4, now);
    sqlite3_step(s);
    sqlite3_reset(s);

    sqlite3_exec(db->db, "COMMIT", NULL, NULL, NULL);
    return 0;
}

int code_db_get_module(code_db_t *db, int64_t tenant_id,
                       const char *path, code_module_t *out) {
    memset(out, 0, sizeof(*out));
    if (!db->db) return -1;

    sqlite3_stmt *s = db->get_module;
    sqlite3_bind_int64(s, 1, tenant_id);
    sqlite3_bind_text(s, 2, path, -1, SQLITE_STATIC);

    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_reset(s);
        return -1;
    }

    out->path = strdup((const char *)sqlite3_column_text(s, 0));
    out->extension = strdup((const char *)sqlite3_column_text(s, 1));
    int blob_len = sqlite3_column_bytes(s, 2);
    out->source = malloc((size_t)blob_len);
    memcpy(out->source, sqlite3_column_blob(s, 2), (size_t)blob_len);
    out->source_len = (size_t)blob_len;
    out->content_hash = strdup((const char *)sqlite3_column_text(s, 3));
    out->updated_at = sqlite3_column_int64(s, 4);

    sqlite3_reset(s);
    return 0;
}

int code_db_delete_module(code_db_t *db, int64_t tenant_id, const char *path) {
    if (!db->db) return -1;

    sqlite3_stmt *s = db->delete_module;
    sqlite3_bind_int64(s, 1, tenant_id);
    sqlite3_bind_text(s, 2, path, -1, SQLITE_STATIC);

    int rc = sqlite3_step(s);
    sqlite3_reset(s);

    if (rc != SQLITE_DONE) return -1;
    return (sqlite3_changes(db->db) > 0) ? 0 : -1;
}

code_module_t *code_db_list_modules(code_db_t *db, int64_t tenant_id,
                                     size_t *count) {
    *count = 0;
    if (!db->db) return NULL;

    sqlite3_stmt *s = db->list_modules;
    sqlite3_bind_int64(s, 1, tenant_id);

    size_t cap = 64;
    code_module_t *list = malloc(cap * sizeof(*list));
    size_t n = 0;

    while (sqlite3_step(s) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= 2;
            list = realloc(list, cap * sizeof(*list));
        }
        code_module_t *m = &list[n++];
        m->path = strdup((const char *)sqlite3_column_text(s, 0));
        m->extension = strdup((const char *)sqlite3_column_text(s, 1));
        m->source = NULL;
        m->source_len = (size_t)sqlite3_column_int64(s, 2);
        m->content_hash = strdup((const char *)sqlite3_column_text(s, 3));
        m->updated_at = sqlite3_column_int64(s, 4);
    }
    sqlite3_reset(s);

    *count = n;
    return list;
}

void code_module_free(code_module_t *m) {
    if (!m) return;
    free(m->path);
    free(m->extension);
    free(m->source);
    free(m->content_hash);
    memset(m, 0, sizeof(*m));
}

/* ======================================================================
 * Source blobs
 * ====================================================================== */

void *code_db_get_blob(code_db_t *db, const char *content_hash,
                       size_t *len, char **extension) {
    if (!db->db) return NULL;

    sqlite3_stmt *s = db->get_blob;
    sqlite3_bind_text(s, 1, content_hash, -1, SQLITE_STATIC);

    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_reset(s);
        return NULL;
    }

    int blob_len = sqlite3_column_bytes(s, 0);
    void *source = malloc((size_t)blob_len);
    memcpy(source, sqlite3_column_blob(s, 0), (size_t)blob_len);
    *len = (size_t)blob_len;

    if (extension) {
        const char *ext = (const char *)sqlite3_column_text(s, 1);
        *extension = ext ? strdup(ext) : NULL;
    }

    sqlite3_reset(s);
    return source;
}

/* ======================================================================
 * Trees
 * ====================================================================== */

int code_db_put_tree(code_db_t *db, const char *hash, int64_t tenant_id,
                     int module_count) {
    if (!db->db) return -1;
    int64_t now = (int64_t)time(NULL);

    sqlite3_stmt *s = db->put_tree;
    sqlite3_bind_text(s, 1, hash, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, tenant_id);
    sqlite3_bind_int64(s, 3, now);
    sqlite3_bind_int(s, 4, module_count);

    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int code_db_put_tree_module(code_db_t *db, const char *tree_hash,
                            const char *module_path,
                            const void *bytecode, size_t bytecode_len,
                            const char *source_map,
                            const char *content_hash) {
    if (!db->db) return -1;

    sqlite3_stmt *s = db->put_tree_module;
    sqlite3_bind_text(s, 1, tree_hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, module_path, -1, SQLITE_STATIC);
    sqlite3_bind_blob(s, 3, bytecode, (int)bytecode_len, SQLITE_STATIC);
    if (source_map)
        sqlite3_bind_text(s, 4, source_map, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 4);
    sqlite3_bind_text(s, 5, content_hash, -1, SQLITE_STATIC);

    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

code_tree_info_t *code_db_list_trees(code_db_t *db, int64_t tenant_id,
                                      size_t *count) {
    *count = 0;
    if (!db->db) return NULL;

    sqlite3_stmt *s = db->list_trees;
    sqlite3_bind_int64(s, 1, tenant_id);

    size_t cap = 32;
    code_tree_info_t *list = malloc(cap * sizeof(*list));
    size_t n = 0;

    while (sqlite3_step(s) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= 2;
            list = realloc(list, cap * sizeof(*list));
        }
        list[n].hash = strdup((const char *)sqlite3_column_text(s, 0));
        list[n].created_at = sqlite3_column_int64(s, 1);
        list[n].module_count = sqlite3_column_int(s, 2);
        n++;
    }
    sqlite3_reset(s);

    *count = n;
    return list;
}

code_tree_module_t *code_db_get_tree_modules(code_db_t *db,
                                              const char *tree_hash,
                                              size_t *count) {
    *count = 0;
    if (!db->db) return NULL;

    sqlite3_stmt *s = db->get_tree_modules;
    sqlite3_bind_text(s, 1, tree_hash, -1, SQLITE_STATIC);

    size_t cap = 64;
    code_tree_module_t *list = malloc(cap * sizeof(*list));
    size_t n = 0;

    while (sqlite3_step(s) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= 2;
            list = realloc(list, cap * sizeof(*list));
        }
        code_tree_module_t *tm = &list[n++];
        tm->module_path = strdup((const char *)sqlite3_column_text(s, 0));

        int bc_len = sqlite3_column_bytes(s, 1);
        tm->bytecode = malloc((size_t)bc_len);
        memcpy(tm->bytecode, sqlite3_column_blob(s, 1), (size_t)bc_len);
        tm->bytecode_len = (size_t)bc_len;

        const char *sm = (const char *)sqlite3_column_text(s, 2);
        tm->source_map = sm ? strdup(sm) : NULL;

        tm->content_hash = strdup((const char *)sqlite3_column_text(s, 3));
    }
    sqlite3_reset(s);

    *count = n;
    return list;
}

char *code_db_get_sourcemap(code_db_t *db, const char *tree_hash,
                            const char *module_path) {
    if (!db->db) return NULL;

    sqlite3_stmt *s = db->get_sourcemap;
    sqlite3_bind_text(s, 1, tree_hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, module_path, -1, SQLITE_STATIC);

    char *result = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *sm = (const char *)sqlite3_column_text(s, 0);
        if (sm) result = strdup(sm);
    }
    sqlite3_reset(s);
    return result;
}

void code_tree_module_free(code_tree_module_t *tm) {
    if (!tm) return;
    free(tm->module_path);
    free(tm->bytecode);
    free(tm->source_map);
    free(tm->content_hash);
    memset(tm, 0, sizeof(*tm));
}
