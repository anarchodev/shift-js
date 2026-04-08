#pragma once

#include <stddef.h>
#include <stdint.h>

/* In-memory bytecode store.
 * Thread-safe: reads are lock-free (atomic pointer swap on update),
 * writes take a lock and rebuild the table. */

typedef struct code_store code_store_t;

typedef struct {
    const void *bytecode;
    size_t      bytecode_len;
    char        source_hash[41]; /* 40-char hex SHA + NUL */
} code_entry_t;

/* Create/destroy the store. */
code_store_t *code_store_create(void);
void          code_store_destroy(code_store_t *store);

/* Look up bytecode by module path (e.g. "api/users/index.mjs").
 * Returns 0 on success, -1 on not found.
 * The returned entry points into store-owned memory — valid until
 * the next code_store_update_begin/end cycle. */
int code_store_get(code_store_t *store, const char *path,
                   code_entry_t *out);

/* Look up a static file by path.
 * Returns 0 on success, -1 on not found. */
int code_store_get_static(code_store_t *store, const char *path,
                          const void **out_data, size_t *out_len);

/* Get the current tree hash (empty string if none). */
const char *code_store_tree_hash(code_store_t *store);

/* --- Bulk update API ---
 * Call begin, then put entries, then end.
 * end atomically swaps the new table in. */
void code_store_update_begin(code_store_t *store);

void code_store_update_put(code_store_t *store, const char *path,
                           const void *bytecode, size_t bytecode_len,
                           const char *source_hash);

void code_store_update_put_static(code_store_t *store, const char *path,
                                  const void *data, size_t len);

void code_store_update_set_hash(code_store_t *store, const char *tree_hash);

void code_store_update_end(code_store_t *store);
