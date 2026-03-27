#pragma once

#include <stddef.h>
#include <stdint.h>

/* SQLite-backed Raft log storage.
 * Named sjs_raft_log to avoid conflicts with the willemt/raft library. */
typedef struct sjs_raft_log sjs_raft_log_t;

typedef struct {
    uint64_t index;
    uint64_t term;
    void    *data;
    uint32_t data_len;
} sjs_raft_entry_t;

/* Open the raft log (creates tables if needed).
 * Each thread that needs access should open its own instance. */
int  raft_log_open(const char *db_path, sjs_raft_log_t **out);
void raft_log_close(sjs_raft_log_t *log);

/* Append an entry at the given index.  Overwrites if index already exists. */
int raft_log_append(sjs_raft_log_t *log, uint64_t index, uint64_t term,
                    const void *data, uint32_t data_len);

/* Get a single entry.  Caller must free out->data. */
int raft_log_get(sjs_raft_log_t *log, uint64_t index, sjs_raft_entry_t *out);

/* Delete all entries with index > after_index. */
int raft_log_truncate_after(sjs_raft_log_t *log, uint64_t after_index);

/* Delete all entries with index <= through_index (compaction). */
int raft_log_truncate_before(sjs_raft_log_t *log, uint64_t through_index);

/* Get the last entry's index and term.  Returns 0/0 if log is empty. */
int raft_log_last(sjs_raft_log_t *log, uint64_t *out_index, uint64_t *out_term);

/* Persistent Raft state (currentTerm, votedFor) — survives restarts. */
int raft_log_save_state(sjs_raft_log_t *log, uint64_t term, int32_t voted_for);
int raft_log_load_state(sjs_raft_log_t *log, uint64_t *term, int32_t *voted_for);
