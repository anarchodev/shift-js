#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct kvstore kvstore_t;

typedef struct {
    char   *key;
    void   *value;
    size_t  value_len;
} kv_entry_t;

typedef struct {
    kv_entry_t *entries;
    size_t      count;
} kv_range_result_t;

/* Open or create a KV store backed by the given SQLite file.
 * Each thread should call kv_open() to get its own connection. */
int kv_open(const char *path, kvstore_t **out);
void kv_close(kvstore_t *store);

/* Core operations.  Values are binary blobs.
 * kv_get: caller must free(*out_value) when done.
 * Returns 0 on success, -1 on not-found, -2 on error. */
int kv_get(kvstore_t *store, const char *key, void **out_value, size_t *out_len);
int kv_put(kvstore_t *store, const char *key, const void *value, size_t len);
int kv_delete(kvstore_t *store, const char *key);

/* Transactions.  Returns 0 on success, -2 on error, -3 on conflict (BUSY). */
int kv_begin(kvstore_t *store);
int kv_commit(kvstore_t *store);
int kv_rollback(kvstore_t *store);

#define KV_CONFLICT (-3)

/* Build a tenant-prefixed key.  If prefix is NULL or empty, returns key
 * unchanged.  Otherwise writes "<prefix><key>" into buf and returns buf.
 * Returns NULL if the result would exceed bufsize. */
const char *kv_prefixed_key(const char *prefix, const char *key,
                            char *buf, size_t bufsize);

/* Range scan: keys where start <= key < end, up to count results.
 * Caller must free the result with kv_range_free(). */
int kv_range(kvstore_t *store, const char *start, const char *end,
             size_t count, kv_range_result_t *out);
void kv_range_free(kv_range_result_t *result);

/* Allocate a monotonic sequence number inside the current transaction.
 * Must be called between kv_begin() and kv_commit().
 * Returns the sequence number, or 0 on error. */
uint64_t kv_next_seq(kvstore_t *store);

/* Delete sequence entries up to and including through_seq (compaction). */
int kv_seq_truncate(kvstore_t *store, uint64_t through_seq);

/* Disable automatic WAL checkpointing on this connection.
 * Call once after kv_open for connections managed by the raft leader. */
void kv_disable_auto_checkpoint(kvstore_t *store);

/* Manually checkpoint the WAL (passive — doesn't block readers).
 * Called by the raft thread after a batch is committed. */
int kv_checkpoint(kvstore_t *store);
