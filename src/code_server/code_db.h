#pragma once

#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    sqlite3 *db;
    /* Prepared statements */
    sqlite3_stmt *put_module;
    sqlite3_stmt *get_module;
    sqlite3_stmt *delete_module;
    sqlite3_stmt *list_modules;
    sqlite3_stmt *put_blob;
    sqlite3_stmt *get_blob;
    sqlite3_stmt *put_tree;
    sqlite3_stmt *put_tree_module;
    sqlite3_stmt *list_trees;
    sqlite3_stmt *get_tree_modules;
    sqlite3_stmt *get_sourcemap;
} code_db_t;

/* Module record returned from queries. Caller frees strings/blobs. */
typedef struct {
    char    *path;
    char    *extension;
    void    *source;
    size_t   source_len;
    char    *content_hash;     /* SHA-256 hex, 64 chars */
    int64_t  updated_at;
} code_module_t;

/* Tree module record. Caller frees blobs/strings. */
typedef struct {
    char    *module_path;      /* base path without extension */
    void    *bytecode;
    size_t   bytecode_len;
    char    *source_map;       /* JSON or NULL */
    char    *content_hash;
} code_tree_module_t;

/* Open or create the code database. Returns 0 on success. */
int code_db_open(code_db_t *db, const char *path);

/* Close the database and finalize all statements. */
void code_db_close(code_db_t *db);

/* ---- Module CRUD ---- */

/* Insert or update a module. source is copied. content_hash is SHA-256 hex.
 * Also inserts into source_blobs (deduplicated). Returns 0 on success. */
int code_db_put_module(code_db_t *db, int64_t tenant_id,
                       const char *path, const char *extension,
                       const void *source, size_t source_len,
                       const char *content_hash);

/* Get a module by path. Populates out (caller frees fields). Returns 0 or -1. */
int code_db_get_module(code_db_t *db, int64_t tenant_id,
                       const char *path, code_module_t *out);

/* Delete a module by path. Returns 0 on success, -1 if not found. */
int code_db_delete_module(code_db_t *db, int64_t tenant_id, const char *path);

/* List all modules for a tenant. Returns malloc'd array, sets *count.
 * Caller frees each entry's strings and the array itself. */
code_module_t *code_db_list_modules(code_db_t *db, int64_t tenant_id,
                                     size_t *count);

/* Free a single module record's strings. */
void code_module_free(code_module_t *m);

/* ---- Source blobs ---- */

/* Get a source blob by content hash. Returns malloc'd source, sets *len.
 * Also sets *extension if non-NULL. Returns NULL if not found. */
void *code_db_get_blob(code_db_t *db, const char *content_hash,
                       size_t *len, char **extension);

/* ---- Trees ---- */

/* Create a tree record. Returns 0 on success. */
int code_db_put_tree(code_db_t *db, const char *hash, int64_t tenant_id,
                     int module_count);

/* Add a module to a tree. Returns 0 on success. */
int code_db_put_tree_module(code_db_t *db, const char *tree_hash,
                            const char *module_path,
                            const void *bytecode, size_t bytecode_len,
                            const char *source_map,
                            const char *content_hash);

/* List trees for a tenant. Returns malloc'd array of hashes, sets *count.
 * Caller frees each string and the array. */
typedef struct {
    char    *hash;
    int64_t  created_at;
    int      module_count;
} code_tree_info_t;

code_tree_info_t *code_db_list_trees(code_db_t *db, int64_t tenant_id,
                                      size_t *count);

/* Get all modules in a tree. Returns malloc'd array, sets *count.
 * Caller frees each entry and the array. */
code_tree_module_t *code_db_get_tree_modules(code_db_t *db,
                                              const char *tree_hash,
                                              size_t *count);

/* Get source map for a module in a specific tree. Returns malloc'd string or NULL. */
char *code_db_get_sourcemap(code_db_t *db, const char *tree_hash,
                            const char *module_path);

/* Free a tree module record's strings/blobs. */
void code_tree_module_free(code_tree_module_t *tm);
