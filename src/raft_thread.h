#pragma once

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

/* Opaque handle shared between workers and the raft thread. */
typedef struct raft_handle raft_handle_t;

/* A single KV operation in a write-set. */
typedef enum {
    RAFT_KV_PUT    = 1,
    RAFT_KV_DELETE = 2,
} raft_kv_op_type_t;

typedef struct {
    raft_kv_op_type_t op;
    char             *key;      /* malloc'd, owned by the proposal */
    void             *value;    /* malloc'd, NULL for DELETE */
    uint32_t          value_len;
} raft_kv_op_t;

/* A proposal from a worker: a set of KV ops from one request. */
typedef struct {
    raft_kv_op_t *ops;
    uint32_t      op_count;
    uint32_t      op_cap;
    uint64_t      seq;          /* assigned by raft_propose_writeset */
} raft_write_set_t;

/* Configuration for starting the raft thread. */
typedef struct {
    uint32_t        node_id;
    uint32_t        node_count;
    const char     *db_path;
    uint16_t        raft_port;

    /* Peer addresses (array of node_count).  Index node_id is self. */
    const char    **peer_hosts;
    uint16_t       *peer_ports;

    /* Timing (ms) */
    uint64_t        election_base_ms;     /* default: 300 */
    uint64_t        heartbeat_interval_ms; /* default: 100 */
    uint64_t        batch_interval_ms;     /* default: 2 */
    uint32_t        batch_max_entries;     /* default: 256 */

    /* Number of worker threads (for per-worker watermark array) */
    uint32_t        worker_count;

    /* CPU core to pin the raft thread to */
    uint32_t        raft_core;

    /* Shared shutdown flag */
    volatile bool  *running;
} raft_config_t;

/* ---- Lifecycle ---- */

/* Create the raft handle and start the raft thread.
 * Returns 0 on success. */
int  raft_handle_create(const raft_config_t *config, raft_handle_t **out);

/* Stop the raft thread and free the handle.
 * Workers must have stopped before calling this. */
void raft_handle_destroy(raft_handle_t *handle);

/* ---- Worker-facing API (called from worker threads) ---- */

/* Check if this node is currently the leader. */
bool raft_handle_is_leader(const raft_handle_t *handle);

/* Get the current leader's node ID, or -1 if unknown. */
int32_t raft_handle_leader_id(const raft_handle_t *handle);

/* Submit a write-set to be proposed to Raft.
 * seq must have been obtained from kv_next_seq() inside the committed txn.
 * The write_set is consumed (caller must not free ops after this). */
void raft_propose_writeset(raft_handle_t *handle, raft_write_set_t *ws);

/* Get the current committed sequence watermark.
 * Responses with seq <= this value can be sent to clients. */
uint64_t raft_committed_seq(const raft_handle_t *handle);

/* Get the current faulted sequence.  If non-zero, all pending responses
 * with seq > last committed should be 503'd (leader loss). */
uint64_t raft_faulted_seq(const raft_handle_t *handle);

/* Check if the raft pipeline has capacity for more proposals.
 * Returns false if the gap between pending and committed is too large. */
bool raft_has_capacity(const raft_handle_t *handle);

/* Per-worker watermark management.
 * Workers call these to signal their progress to the raft thread. */
void raft_worker_begin(raft_handle_t *handle, int worker_id);
void raft_worker_committed(raft_handle_t *handle, int worker_id, uint64_t seq);
void raft_worker_idle(raft_handle_t *handle, int worker_id);

/* ---- Write-set helpers ---- */

void raft_write_set_init(raft_write_set_t *ws);
void raft_write_set_free(raft_write_set_t *ws);
int  raft_write_set_add_put(raft_write_set_t *ws, const char *key,
                            const void *value, uint32_t value_len);
int  raft_write_set_add_delete(raft_write_set_t *ws, const char *key);
