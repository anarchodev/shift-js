#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct code_db code_db_t;

typedef struct {
    char *path;
    char *sha1;     /* 40-char hex string */
} code_tree_entry_t;

typedef struct {
    code_tree_entry_t *entries;
    size_t             count;
} code_tree_t;

/* Open/close the code database (separate SQLite file). */
int  code_db_open(const char *path, code_db_t **out);
void code_db_close(code_db_t *db);

/* Store a file (content-addressed by SHA-1).  Idempotent.
 * Also updates the working tree for database_id with path→sha1. */
int code_db_put_file(code_db_t *db, const char *database_id,
                     const char *path, const void *content, size_t len);

/* Get a file blob by SHA-1.  Caller frees *out. */
int code_db_get_file(code_db_t *db, const char *sha1,
                     void **out, size_t *out_len);

/* List all entries in a tree.  Use tree_hash="" for the working tree. */
int  code_db_tree_list(code_db_t *db, const char *database_id,
                       const char *tree_hash, code_tree_t *out);
void code_db_tree_free(code_tree_t *tree);

/* Get a file's content from a specific tree (JOIN through files table).
 * Caller frees *out. */
int code_db_tree_get(code_db_t *db, const char *database_id,
                     const char *tree_hash, const char *path,
                     void **out, size_t *out_len);

/* Snapshot the working tree: compute Merkle tree hash, freeze entries,
 * record deployment.  Returns malloc'd 40-char hex SHA-1 tree hash. */
char *code_db_snapshot(code_db_t *db, const char *database_id);

/* Delete a path from the working tree. */
int code_db_tree_delete(code_db_t *db, const char *database_id,
                        const char *path);

/* Clear the entire working tree for a database_id. */
int code_db_tree_clear(code_db_t *db, const char *database_id);
