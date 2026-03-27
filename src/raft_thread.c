#define _GNU_SOURCE
#include "raft_thread.h"
#include "raft_log.h"
#include "raft_net.h"
#include "raft_rpc.h"
#include "kvstore.h"

#include <stddef.h>  /* needed before raft.h for size_t */
#include <raft.h>

#include <arpa/inet.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ---- MPSC queue (lock-free, bounded) ---- */

#define PROPOSAL_QUEUE_CAP 4096  /* must be power of 2 */

typedef struct {
    raft_write_set_t  slots[PROPOSAL_QUEUE_CAP];
    _Atomic uint64_t  head;
    _Atomic uint64_t  tail;
} proposal_queue_t;

static void pq_init(proposal_queue_t *q) {
    memset(q->slots, 0, sizeof(q->slots));
    atomic_store(&q->head, 0);
    atomic_store(&q->tail, 0);
}

static bool pq_push(proposal_queue_t *q, raft_write_set_t *ws) {
    uint64_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    if (tail - head >= PROPOSAL_QUEUE_CAP)
        return false;
    q->slots[tail & (PROPOSAL_QUEUE_CAP - 1)] = *ws;
    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return true;
}

static bool pq_pop(proposal_queue_t *q, raft_write_set_t *out) {
    uint64_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (head >= tail)
        return false;
    *out = q->slots[head & (PROPOSAL_QUEUE_CAP - 1)];
    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return true;
}

/* ---- Handle ---- */

#define MAX_WORKERS 128

struct raft_handle {
    /* Shared atomics (read by workers, written by raft thread) */
    _Atomic uint64_t committed_seq;
    _Atomic uint64_t faulted_seq;
    _Atomic bool     is_leader;
    _Atomic int32_t  leader_id;

    /* Proposal queue (written by workers, read by raft thread) */
    proposal_queue_t *queue;

    /* Per-worker watermarks. Each worker sets its own entry.
     * UINT64_MAX = idle (don't consider this worker).
     * Other value = the highest seq this worker has pushed, or the
     * global high watermark at the time it started a transaction. */
    _Atomic uint64_t worker_watermark[MAX_WORKERS];
    uint32_t         worker_count;

    /* High watermark: highest seq seen from any worker push.
     * Workers read this atomically when starting a transaction. */
    _Atomic uint64_t high_watermark;

    /* Thread */
    pthread_t        thread;
    volatile bool   *running;

    /* willemt/raft server (owned by raft thread, accessed in callbacks) */
    raft_server_t   *_raft;

    /* Config (owned copies) */
    raft_config_t    config;
};

/* ---- Thread-local context for callbacks ---- */

typedef struct {
    raft_handle_t *handle;
    raft_net_t    *net;
    sjs_raft_log_t *log;
    kvstore_t     *kv;
} thread_ctx_t;

/* ---- Monotonic time ---- */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ---- Serialize/deserialize batch blobs ---- */

static void *serialize_batch(raft_write_set_t *sets, uint32_t count,
                             uint32_t *out_len) {
    /* Count non-deduped ops per write-set */
    uint32_t size = 4;
    for (uint32_t i = 0; i < count; i++) {
        size += 8 + 4;
        for (uint32_t j = 0; j < sets[i].op_count; j++) {
            raft_kv_op_t *op = &sets[i].ops[j];
            if (!op->key) continue; /* deduped out */
            uint32_t klen = (uint32_t)strlen(op->key);
            size += 1 + 4 + klen + 4 + op->value_len;
        }
    }

    uint8_t *buf = malloc(size);
    if (!buf) return NULL;

    uint8_t *p = buf;
    uint32_t nc = htonl(count);
    memcpy(p, &nc, 4); p += 4;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t shi = htonl((uint32_t)(sets[i].seq >> 32));
        uint32_t slo = htonl((uint32_t)(sets[i].seq & 0xFFFFFFFF));
        memcpy(p, &shi, 4); p += 4;
        memcpy(p, &slo, 4); p += 4;

        /* Count live ops for this write-set */
        uint32_t live_ops = 0;
        for (uint32_t j = 0; j < sets[i].op_count; j++)
            if (sets[i].ops[j].key) live_ops++;

        uint32_t noc = htonl(live_ops);
        memcpy(p, &noc, 4); p += 4;

        for (uint32_t j = 0; j < sets[i].op_count; j++) {
            raft_kv_op_t *op = &sets[i].ops[j];
            if (!op->key) continue; /* deduped out */
            *p++ = (uint8_t)op->op;
            uint32_t klen = (uint32_t)strlen(op->key);
            uint32_t nkl = htonl(klen);
            memcpy(p, &nkl, 4); p += 4;
            memcpy(p, op->key, klen); p += klen;
            uint32_t nvl = htonl(op->value_len);
            memcpy(p, &nvl, 4); p += 4;
            if (op->value_len > 0) {
                memcpy(p, op->value, op->value_len);
                p += op->value_len;
            }
        }
    }

    *out_len = size;
    return buf;
}

static int apply_batch(kvstore_t *kv, const void *data, uint32_t data_len,
                       uint64_t *max_seq_out) {
    if (data_len < 4) return -1;
    const uint8_t *p = data;

    uint32_t nc;
    memcpy(&nc, p, 4); p += 4;
    uint32_t count = ntohl(nc);

    uint64_t max_seq = 0;

    for (uint32_t i = 0; i < count; i++) {
        if ((uint32_t)(p - (const uint8_t *)data) + 12 > data_len) return -1;

        uint32_t shi, slo;
        memcpy(&shi, p, 4); p += 4;
        memcpy(&slo, p, 4); p += 4;
        uint64_t seq = ((uint64_t)ntohl(shi) << 32) | (uint64_t)ntohl(slo);
        if (seq > max_seq) max_seq = seq;

        uint32_t noc;
        memcpy(&noc, p, 4); p += 4;
        uint32_t op_count = ntohl(noc);

        for (uint32_t j = 0; j < op_count; j++) {
            uint8_t op_type = *p++;
            uint32_t nkl;
            memcpy(&nkl, p, 4); p += 4;
            uint32_t klen = ntohl(nkl);
            char *key = malloc(klen + 1);
            if (!key) return -1;
            memcpy(key, p, klen);
            key[klen] = '\0';
            p += klen;
            uint32_t nvl;
            memcpy(&nvl, p, 4); p += 4;
            uint32_t vlen = ntohl(nvl);
            if (op_type == RAFT_KV_PUT) {
                kv_put_seq(kv, key, p, vlen, seq);
                p += vlen;
            } else if (op_type == RAFT_KV_DELETE) {
                kv_delete(kv, key);
            }
            free(key);
        }
    }

    if (max_seq_out) *max_seq_out = max_seq;
    return 0;
}

static void dedupe_batch(raft_write_set_t *sets, uint32_t count) {
    for (uint32_t i = count; i > 0; i--) {
        raft_write_set_t *ws = &sets[i - 1];
        for (uint32_t j = ws->op_count; j > 0; j--) {
            raft_kv_op_t *op = &ws->ops[j - 1];
            if (!op->key) continue;
            bool dominated = false;
            for (uint32_t ii = count; ii > i && !dominated; ii--) {
                raft_write_set_t *ws2 = &sets[ii - 1];
                for (uint32_t jj = 0; jj < ws2->op_count; jj++) {
                    if (ws2->ops[jj].key && strcmp(op->key, ws2->ops[jj].key) == 0) {
                        dominated = true;
                        break;
                    }
                }
            }
            if (!dominated) {
                for (uint32_t jj = ws->op_count; jj > j; jj--) {
                    if (ws->ops[jj - 1].key &&
                        strcmp(op->key, ws->ops[jj - 1].key) == 0) {
                        dominated = true;
                        break;
                    }
                }
            }
            if (dominated) {
                free(op->key); free(op->value);
                op->key = NULL; op->value = NULL; op->value_len = 0;
            }
        }
    }
}

/* ---- willemt/raft callbacks ---- */

static int cb_send_requestvote(raft_server_t *raft, void *udata,
                               raft_node_t *node, msg_requestvote_t *msg) {
    thread_ctx_t *ctx = udata;
    raft_node_id_t peer_id = raft_node_get_id(node);

    uint32_t len;
    void *buf = raft_rpc_encode_vote_req(msg, &len);
    if (buf) {
        raft_net_send(ctx->net, (uint32_t)peer_id, buf, len);
        free(buf);
    }
    return 0;
}

static int cb_send_appendentries(raft_server_t *raft, void *udata,
                                 raft_node_t *node, msg_appendentries_t *msg) {
    thread_ctx_t *ctx = udata;
    raft_node_id_t peer_id = raft_node_get_id(node);

    uint32_t len;
    void *buf = raft_rpc_encode_append_req(msg, &len);
    if (buf) {
        raft_net_send(ctx->net, (uint32_t)peer_id, buf, len);
        free(buf);
    }
    return 0;
}

static int cb_send_snapshot(raft_server_t *raft, void *udata,
                            raft_node_t *node) {
    thread_ctx_t *ctx = udata;
    raft_node_id_t peer_id = raft_node_get_id(node);

    /* Read the KV database file into memory.
     * The checkpointed DB is the snapshot — it contains all committed
     * state up to the snapshot index. */
    const char *db_path = ctx->handle->config.db_path;
    FILE *f = fopen(db_path, "rb");
    if (!f) {
        fprintf(stderr, "raft: snapshot send failed: can't open %s\n", db_path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 256 * 1024 * 1024) {
        fprintf(stderr, "raft: snapshot send failed: db size %ld\n", fsize);
        fclose(f);
        return -1;
    }

    /* Frame: [4 len][1 type=SNAPSHOT][8 last_term][8 last_idx][db bytes] */
    raft_index_t snap_idx = raft_get_snapshot_last_idx(raft);
    raft_term_t  snap_term = raft_get_snapshot_last_term(raft);

    uint32_t payload_size = 1 + 8 + 8 + (uint32_t)fsize;
    uint32_t total = 4 + payload_size;
    uint8_t *buf = malloc(total);
    if (!buf) { fclose(f); return -1; }

    /* Length header */
    uint32_t nlen = htonl(payload_size);
    memcpy(buf, &nlen, 4);

    /* Type */
    buf[4] = RAFT_MSG_SNAPSHOT;

    /* Snapshot metadata */
    uint8_t *p = buf + 5;
    uint32_t hi, lo;
    hi = htonl((uint32_t)((uint64_t)snap_term >> 32));
    lo = htonl((uint32_t)((uint64_t)snap_term & 0xFFFFFFFF));
    memcpy(p, &hi, 4); p += 4;
    memcpy(p, &lo, 4); p += 4;
    hi = htonl((uint32_t)((uint64_t)snap_idx >> 32));
    lo = htonl((uint32_t)((uint64_t)snap_idx & 0xFFFFFFFF));
    memcpy(p, &hi, 4); p += 4;
    memcpy(p, &lo, 4); p += 4;

    /* DB contents */
    if (fread(p, 1, (size_t)fsize, f) != (size_t)fsize) {
        fprintf(stderr, "raft: snapshot send failed: read error\n");
        fclose(f);
        free(buf);
        return -1;
    }
    fclose(f);

    raft_net_send(ctx->net, (uint32_t)peer_id, buf, total);
    free(buf);

    fprintf(stderr, "raft: sent snapshot to node %d (idx=%ld, %ld bytes)\n",
            (int)peer_id, (long)snap_idx, fsize);
    return 0;
}

static int cb_applylog(raft_server_t *raft, void *udata,
                       raft_entry_t *entry, raft_index_t entry_idx) {
    thread_ctx_t *ctx = udata;

    if (entry->type != RAFT_LOGTYPE_NORMAL)
        return 0;

    uint64_t max_seq = 0;

    if (raft_is_leader(raft)) {
        /* Leader: workers already applied writes to local SQLite.
         * Just parse max_seq to advance the committed watermark. */
        if (entry->data.len >= 4) {
            const uint8_t *p = entry->data.buf;
            uint32_t nc;
            memcpy(&nc, p, 4); p += 4;
            uint32_t count = ntohl(nc);
            for (uint32_t i = 0; i < count; i++) {
                uint32_t shi, slo;
                memcpy(&shi, p, 4); p += 4;
                memcpy(&slo, p, 4); p += 4;
                uint64_t seq = ((uint64_t)ntohl(shi) << 32) | (uint64_t)ntohl(slo);
                if (seq > max_seq) max_seq = seq;
                uint32_t noc;
                memcpy(&noc, p, 4); p += 4;
                uint32_t op_count = ntohl(noc);
                for (uint32_t j = 0; j < op_count; j++) {
                    p++;
                    uint32_t nkl; memcpy(&nkl, p, 4); p += 4;
                    p += ntohl(nkl);
                    uint32_t nvl; memcpy(&nvl, p, 4); p += 4;
                    p += ntohl(nvl);
                }
            }
        }
    } else {
        /* Follower: apply KV writes from the committed entry. */
        kv_begin(ctx->kv);
        apply_batch(ctx->kv, entry->data.buf, entry->data.len, &max_seq);
        kv_commit(ctx->kv);
    }

    if (max_seq > 0) {
        atomic_store_explicit(&ctx->handle->committed_seq, max_seq,
                              memory_order_release);
    }

    return 0;
}

static int cb_persist_vote(raft_server_t *raft, void *udata,
                           raft_node_id_t vote) {
    thread_ctx_t *ctx = udata;
    raft_term_t term = raft_get_current_term(raft);
    raft_log_save_state(ctx->log, (uint64_t)term, (int32_t)vote);
    return 0;
}

static int cb_persist_term(raft_server_t *raft, void *udata,
                           raft_term_t term, raft_node_id_t vote) {
    thread_ctx_t *ctx = udata;
    raft_log_save_state(ctx->log, (uint64_t)term, (int32_t)vote);
    return 0;
}

static int cb_log_offer(raft_server_t *raft, void *udata,
                        raft_entry_t *entry, raft_index_t entry_idx) {
    thread_ctx_t *ctx = udata;

    /* Make a permanent copy of entry data — the library may free the
     * original buffer after this callback returns. */
    void *data_copy = NULL;
    if (entry->data.len > 0 && entry->data.buf) {
        data_copy = malloc(entry->data.len);
        if (!data_copy) return -1;
        memcpy(data_copy, entry->data.buf, entry->data.len);
        entry->data.buf = data_copy;
    }

    raft_log_append(ctx->log, (uint64_t)entry_idx, (uint64_t)entry->term,
                    entry->data.buf, (uint32_t)entry->data.len);
    return 0;
}

static int cb_log_poll(raft_server_t *raft, void *udata,
                       raft_entry_t *entry, raft_index_t entry_idx) {
    thread_ctx_t *ctx = udata;
    /* Remove oldest entry (compaction) */
    raft_log_truncate_before(ctx->log, (uint64_t)entry_idx);
    if (entry->data.buf) {
        free(entry->data.buf);
        entry->data.buf = NULL;
    }
    return 0;
}

static int cb_log_pop(raft_server_t *raft, void *udata,
                      raft_entry_t *entry, raft_index_t entry_idx) {
    thread_ctx_t *ctx = udata;
    /* Remove newest entry (truncation on conflict) */
    raft_log_truncate_after(ctx->log, (uint64_t)entry_idx - 1);
    if (entry->data.buf) {
        free(entry->data.buf);
        entry->data.buf = NULL;
    }
    return 0;
}

static void cb_log_fn(raft_server_t *raft, raft_node_t *node,
                      void *udata, const char *buf) {
    thread_ctx_t *ctx = udata;
    int nid = node ? (int)raft_node_get_id(node) : -1;
    fprintf(stderr, "raft[%u]: node=%d %s\n",
            ctx->handle->config.node_id, nid, buf);
}

/* ---- Network receive callback ---- */

static void on_peer_message(uint32_t from_id, const void *data,
                            uint32_t len, void *user_ctx) {
    thread_ctx_t *ctx = user_ctx;
    raft_server_t *r = ctx->handle->_raft;

    raft_wire_msg_t msg;
    if (raft_rpc_decode(data, len, &msg) != 0)
        return;

    raft_node_t *node = raft_get_node(r, (raft_node_id_t)from_id);
    if (!node) return;

    switch (msg.type) {
    case RAFT_MSG_VOTE_REQ: {
        msg_requestvote_response_t resp;
        raft_recv_requestvote(r, node, &msg.vote_req, &resp);
        uint32_t rlen;
        void *rbuf = raft_rpc_encode_vote_resp(&resp, &rlen);
        if (rbuf) {
            raft_net_send(ctx->net, from_id, rbuf, rlen);
            free(rbuf);
        }
        break;
    }
    case RAFT_MSG_VOTE_RESP:
        raft_recv_requestvote_response(r, node, &msg.vote_resp);
        break;
    case RAFT_MSG_APPEND_REQ: {
        msg_appendentries_response_t resp;
        raft_recv_appendentries(r, node, &msg.append_req, &resp);
        /* Free decoded entries */
        for (int i = 0; i < msg.append_req.n_entries; i++)
            free(msg.append_req.entries[i].data.buf);
        free(msg.append_req.entries);
        uint32_t rlen;
        void *rbuf = raft_rpc_encode_append_resp(&resp, &rlen);
        if (rbuf) {
            raft_net_send(ctx->net, from_id, rbuf, rlen);
            free(rbuf);
        }
        break;
    }
    case RAFT_MSG_APPEND_RESP:
        raft_recv_appendentries_response(r, node, &msg.append_resp);
        break;
    case RAFT_MSG_SNAPSHOT: {
        /* Snapshot received from leader: replace KV DB and load into raft. */
        if (len < 1 + 8 + 8) break;
        const uint8_t *sp = (const uint8_t *)data + 1; /* skip type byte */
        uint32_t shi, slo;
        memcpy(&shi, sp, 4); sp += 4;
        memcpy(&slo, sp, 4); sp += 4;
        raft_term_t snap_term = (raft_term_t)(((uint64_t)ntohl(shi) << 32) |
                                               (uint64_t)ntohl(slo));
        memcpy(&shi, sp, 4); sp += 4;
        memcpy(&slo, sp, 4); sp += 4;
        raft_index_t snap_idx = (raft_index_t)(((uint64_t)ntohl(shi) << 32) |
                                                (uint64_t)ntohl(slo));
        uint32_t db_size = len - 1 - 16;
        const void *db_data = sp;

        fprintf(stderr, "raft: received snapshot from node %u "
                "(term=%ld, idx=%ld, %u bytes)\n",
                from_id, (long)snap_term, (long)snap_idx, db_size);

        /* Close existing KV connection, write new DB, reopen */
        if (ctx->kv) { kv_close(ctx->kv); ctx->kv = NULL; }

        FILE *f = fopen(ctx->handle->config.db_path, "wb");
        if (f) {
            fwrite(db_data, 1, db_size, f);
            fclose(f);
        }

        if (kv_open(ctx->handle->config.db_path, &ctx->kv) != 0) {
            fprintf(stderr, "raft: failed to reopen kv after snapshot\n");
            break;
        }
        kv_disable_auto_checkpoint(ctx->kv);

        /* Tell the raft library about the loaded snapshot */
        if (raft_begin_load_snapshot(r, snap_term, snap_idx) == 0) {
            /* Re-add all nodes (required by the library) */
            for (uint32_t i = 0; i < ctx->handle->config.node_count; i++) {
                raft_node_t *n = raft_get_node(r, (raft_node_id_t)i);
                if (n) {
                    raft_node_set_active(n, 1);
                    raft_node_set_voting_committed(n, 1);
                    raft_node_set_addition_committed(n, 1);
                }
            }
            raft_end_load_snapshot(r);
        }

        fprintf(stderr, "raft: snapshot loaded (idx=%ld)\n", (long)snap_idx);
        break;
    }
    default:
        break;
    }

    /* Update shared state */
    raft_node_id_t lid = raft_get_current_leader(r);
    atomic_store(&ctx->handle->leader_id, (int32_t)lid);

    bool was_leader = atomic_load(&ctx->handle->is_leader);
    bool now_leader = raft_is_leader(r);

    if (!was_leader && now_leader) {
        atomic_store(&ctx->handle->is_leader, true);
        atomic_store(&ctx->handle->faulted_seq, 0);
        fprintf(stderr, "raft: became leader (node %u, term %ld)\n",
                ctx->handle->config.node_id,
                (long)raft_get_current_term(r));
    } else if (was_leader && !now_leader) {
        atomic_store(&ctx->handle->is_leader, false);
        uint64_t cur_seq = atomic_load(&ctx->handle->high_watermark);
        atomic_store_explicit(&ctx->handle->faulted_seq, cur_seq,
                              memory_order_release);
        fprintf(stderr, "raft: lost leadership (node %u, term %ld)\n",
                ctx->handle->config.node_id,
                (long)raft_get_current_term(r));
    }
}

/* ---- Raft thread main loop ---- */

static void pin_to_core(int core) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

static void *raft_thread_fn(void *arg) {
    raft_handle_t *handle = arg;
    const raft_config_t *cfg = &handle->config;

    pin_to_core((int)cfg->raft_core);

    /* Open our own SQLite connections */
    sjs_raft_log_t *log = NULL;
    kvstore_t  *kv  = NULL;

    /* Raft log uses a separate database to avoid SQLITE_BUSY contention
     * between the raft thread and worker threads writing to the KV table. */
    char raft_db_path[512];
    snprintf(raft_db_path, sizeof(raft_db_path), "%s-raft", cfg->db_path);

    if (raft_log_open(raft_db_path, &log) != 0) {
        fprintf(stderr, "raft: failed to open log at %s\n", raft_db_path);
        return NULL;
    }
    /* KV connection: used by followers to apply committed entries,
     * and by the leader to checkpoint the WAL after raft commit. */
    if (kv_open(cfg->db_path, &kv) != 0) {
        fprintf(stderr, "raft: failed to open kv store\n");
        raft_log_close(log);
        return NULL;
    }
    kv_disable_auto_checkpoint(kv);

    /* Create thread context */
    thread_ctx_t ctx = {
        .handle = handle,
        .net    = NULL,
        .log    = log,
        .kv     = kv,
    };

    /* Create willemt/raft server */
    raft_server_t *r = raft_new();
    if (!r) {
        fprintf(stderr, "raft: raft_new failed\n");
        kv_close(kv); raft_log_close(log);
        return NULL;
    }
    handle->_raft = r;

    raft_cbs_t cbs = {
        .send_requestvote    = cb_send_requestvote,
        .send_appendentries  = cb_send_appendentries,
        .send_snapshot       = cb_send_snapshot,
        .applylog            = cb_applylog,
        .persist_vote        = cb_persist_vote,
        .persist_term        = cb_persist_term,
        .log_offer           = cb_log_offer,
        .log_poll            = cb_log_poll,
        .log_pop             = cb_log_pop,
        .log                 = cb_log_fn,
    };
    raft_set_callbacks(r, &cbs, &ctx);

    /* Add nodes (same order on all servers, 1-based IDs) */
    for (uint32_t i = 0; i < cfg->node_count; i++) {
        int is_self = (i == cfg->node_id) ? 1 : 0;
        raft_add_node(r, NULL, (raft_node_id_t)i, is_self);
    }

    /* Set timeouts */
    raft_set_election_timeout(r, (int)cfg->election_base_ms);
    raft_set_request_timeout(r, (int)cfg->heartbeat_interval_ms);

    /* Restore persistent state */
    uint64_t saved_term;
    int32_t  saved_vote;
    raft_log_load_state(log, &saved_term, &saved_vote);
    if (saved_term > 0)
        raft_set_current_term(r, (raft_term_t)saved_term);
    if (saved_vote >= 0)
        raft_vote_for_nodeid(r, (raft_node_id_t)saved_vote);

    /* Restore log entries */
    uint64_t last_idx, last_term;
    raft_log_last(log, &last_idx, &last_term);
    for (uint64_t i = 1; i <= last_idx; i++) {
        sjs_raft_entry_t ent;
        if (raft_log_get(log, i, &ent) == 0) {
            msg_entry_t entry = {
                .term = (raft_term_t)ent.term,
                .id   = (raft_entry_id_t)i,
                .type = RAFT_LOGTYPE_NORMAL,
                .data = { .buf = ent.data, .len = ent.data_len },
            };
            raft_append_entry(r, &entry);
            /* entry.data.buf ownership transferred to raft library */
        }
    }

    /* Initialize networking */
    raft_peer_addr_t *addrs = calloc(cfg->node_count, sizeof(raft_peer_addr_t));
    for (uint32_t i = 0; i < cfg->node_count; i++) {
        addrs[i].host = cfg->peer_hosts[i];
        addrs[i].port = cfg->peer_ports[i];
    }

    raft_net_t *net = NULL;
    if (raft_net_create(cfg->node_id, addrs, cfg->node_count, cfg->raft_port,
                        on_peer_message, &ctx, &net) != 0) {
        fprintf(stderr, "raft: failed to create networking\n");
        free(addrs); raft_free(r);
        kv_close(kv); raft_log_close(log);
        return NULL;
    }
    ctx.net = net;
    free(addrs);

    fprintf(stderr, "raft: node %u started (port %u, %u peers, term %ld)\n",
            cfg->node_id, cfg->raft_port, cfg->node_count,
            (long)raft_get_current_term(r));

    /* Drain buffer: proposals popped from MPSC, waiting to be batched.
     * With per-worker watermarks we don't need OOO — all proposals in
     * the queue with seq <= safe_seq are safe to batch in any order. */
    uint32_t drain_cap = PROPOSAL_QUEUE_CAP;
    raft_write_set_t *drain_buf = calloc(drain_cap, sizeof(raft_write_set_t));
    uint32_t drain_count = 0;
    uint64_t last_batch_time = now_ms();

    uint64_t last_tick = now_ms();
    uint64_t last_checkpoint = now_ms();

    /* ---- Main event loop ---- */
    while (*cfg->running) {
        uint64_t t = now_ms();
        uint64_t elapsed = t - last_tick;
        last_tick = t;

        /* 1. Tick the raft library (elections, heartbeats, apply) */
        int tick_rc = raft_periodic(r, (int)elapsed);
        if (tick_rc == RAFT_ERR_SHUTDOWN) break;

        /* Update leadership state after tick */
        bool was_leader = atomic_load(&handle->is_leader);
        bool now_leader = raft_is_leader(r);
        raft_node_id_t lid = raft_get_current_leader(r);
        atomic_store(&handle->leader_id, (int32_t)lid);

        if (!was_leader && now_leader) {
            atomic_store(&handle->is_leader, true);
            atomic_store(&handle->faulted_seq, 0);
            fprintf(stderr, "raft: became leader (node %u, term %ld)\n",
                    cfg->node_id, (long)raft_get_current_term(r));
        } else if (was_leader && !now_leader) {
            atomic_store(&handle->is_leader, false);
            uint64_t hw = atomic_load(&handle->high_watermark);
            atomic_store_explicit(&handle->faulted_seq, hw,
                                  memory_order_release);
            fprintf(stderr, "raft: lost leadership (node %u, term %ld)\n",
                    cfg->node_id, (long)raft_get_current_term(r));
            for (uint32_t s = 0; s < drain_count; s++)
                raft_write_set_free(&drain_buf[s]);
            drain_count = 0;
        }

        /* 2. Poll network. */
        raft_net_poll(net, 1);

        /* 2b. Tick again after poll to immediately apply entries
         * committed by acks received during the poll. */
        raft_periodic(r, 0);

        /* 3. Drain proposal queue (only if leader) */
        if (atomic_load(&handle->is_leader)) {
            raft_write_set_t ws;
            while (pq_pop(handle->queue, &ws)) {
                if (drain_count >= drain_cap) {
                    fprintf(stderr, "raft: FATAL: drain buffer overflow "
                            "(seq=%lu, count=%u, cap=%u)\n",
                            (unsigned long)ws.seq, drain_count, drain_cap);
                    abort();
                }
                drain_buf[drain_count++] = ws;
            }

            /* 4. Compute safe_seq from per-worker watermarks */
            uint64_t safe_seq = UINT64_MAX;
            for (uint32_t w = 0; w < handle->worker_count; w++) {
                uint64_t wm = atomic_load_explicit(
                    &handle->worker_watermark[w], memory_order_acquire);
                if (wm < safe_seq) safe_seq = wm;
            }

            /* 5. Partition drain_buf: entries with seq <= safe_seq go
             * into the batch, capped at batch_max_entries. */
            t = now_ms();
            uint32_t batch_count = 0;
            for (uint32_t i = 0; i < drain_count; i++) {
                if (drain_buf[i].seq <= safe_seq) {
                    batch_count++;
                    if (batch_count >= cfg->batch_max_entries) break;
                }
            }

            bool should_propose = batch_count > 0 &&
                (t >= last_batch_time + cfg->batch_interval_ms ||
                 batch_count >= cfg->batch_max_entries);

            if (should_propose) {
                /* Separate safe entries from not-yet-safe ones,
                 * capped at batch_count. */
                raft_write_set_t *batch = calloc(batch_count, sizeof(raft_write_set_t));
                uint32_t bi = 0;
                uint32_t ri = 0; /* remaining */
                for (uint32_t i = 0; i < drain_count; i++) {
                    if (drain_buf[i].seq <= safe_seq && bi < batch_count)
                        batch[bi++] = drain_buf[i];
                    else
                        drain_buf[ri++] = drain_buf[i];
                }
                drain_count = ri;

                /* Sort by seq so followers apply in WAL order */
                for (uint32_t i = 1; i < batch_count; i++) {
                    raft_write_set_t tmp = batch[i];
                    uint32_t j = i;
                    while (j > 0 && batch[j-1].seq > tmp.seq) {
                        batch[j] = batch[j-1];
                        j--;
                    }
                    batch[j] = tmp;
                }

                dedupe_batch(batch, batch_count);

                uint32_t blob_len;
                void *blob = serialize_batch(batch, batch_count, &blob_len);

                if (blob) {
                    msg_entry_t entry = {
                        .type = RAFT_LOGTYPE_NORMAL,
                        .data = { .buf = blob, .len = blob_len },
                    };
                    msg_entry_response_t resp;
                    int rc = raft_recv_entry(r, &entry, &resp);

                    if (rc != 0) {
                        free(blob);
                        fprintf(stderr, "raft: propose failed rc=%d\n", rc);
                    }
                }

                for (uint32_t i = 0; i < batch_count; i++)
                    raft_write_set_free(&batch[i]);
                free(batch);
                last_batch_time = t;
            }
        }

        /* Periodic raft snapshot (all nodes).
         * Snapshot = WAL checkpoint + raft log compaction.
         * The checkpointed KV DB IS the snapshot — it reflects all
         * committed entries up to the commit index.  After checkpoint,
         * raft log entries up to commit index can be deleted.
         * Each node snapshots independently to bound its own log. */
        if (t - last_checkpoint >= 500 &&
            !raft_snapshot_is_in_progress(r)) {
            raft_index_t commit_idx = raft_get_commit_idx(r);
            if (commit_idx > raft_get_snapshot_last_idx(r)) {
                if (raft_begin_snapshot(r, RAFT_SNAPSHOT_NONBLOCKING_APPLY) == 0) {
                    /* Compact kv_seq rows up to committed seq */
                    uint64_t cs = atomic_load(&handle->committed_seq);
                    if (cs > 0)
                        kv_seq_truncate(ctx.kv, cs);
                    kv_checkpoint(ctx.kv);
                    raft_end_snapshot(r);
                    last_checkpoint = t;
                }
            }
        }
    }

    /* Cleanup */
    for (uint32_t i = 0; i < drain_count; i++)
        raft_write_set_free(&drain_buf[i]);
    free(drain_buf);

    raft_net_destroy(net);
    raft_free(r);
    kv_close(kv);
    raft_log_close(log);

    fprintf(stderr, "raft: node %u stopped\n", cfg->node_id);
    return NULL;
}

/* ---- Public API ---- */

int raft_handle_create(const raft_config_t *config, raft_handle_t **out) {
    raft_handle_t *h = calloc(1, sizeof(*h));
    if (!h) return -1;

    h->config = *config;
    h->running = config->running;

    h->config.peer_hosts = calloc(config->node_count, sizeof(char *));
    h->config.peer_ports = calloc(config->node_count, sizeof(uint16_t));
    for (uint32_t i = 0; i < config->node_count; i++) {
        h->config.peer_hosts[i] = strdup(config->peer_hosts[i]);
        h->config.peer_ports[i] = config->peer_ports[i];
    }
    h->config.db_path = strdup(config->db_path);

    if (h->config.election_base_ms == 0)     h->config.election_base_ms = 1000;
    if (h->config.heartbeat_interval_ms == 0) h->config.heartbeat_interval_ms = 200;
    if (h->config.batch_interval_ms == 0)    h->config.batch_interval_ms = 2;
    if (h->config.batch_max_entries == 0)    h->config.batch_max_entries = 256;

    atomic_store(&h->committed_seq, 0);
    atomic_store(&h->faulted_seq, 0);
    atomic_store(&h->is_leader, false);
    atomic_store(&h->leader_id, -1);
    atomic_store(&h->high_watermark, 0);
    uint32_t nw = config->worker_count;
    h->worker_count = nw > MAX_WORKERS ? MAX_WORKERS : nw;
    for (uint32_t i = 0; i < MAX_WORKERS; i++)
        atomic_store(&h->worker_watermark[i], UINT64_MAX);

    h->queue = calloc(1, sizeof(proposal_queue_t));
    if (!h->queue) { free(h); return -1; }
    pq_init(h->queue);

    if (pthread_create(&h->thread, NULL, raft_thread_fn, h) != 0) {
        free(h->queue); free(h);
        return -1;
    }

    *out = h;
    return 0;
}

void raft_handle_destroy(raft_handle_t *handle) {
    if (!handle) return;
    pthread_join(handle->thread, NULL);
    for (uint32_t i = 0; i < handle->config.node_count; i++)
        free((char *)handle->config.peer_hosts[i]);
    free((char **)handle->config.peer_hosts);
    free(handle->config.peer_ports);
    free((char *)handle->config.db_path);
    free(handle->queue);
    free(handle);
}

bool raft_handle_is_leader(const raft_handle_t *handle) {
    return atomic_load_explicit(&handle->is_leader, memory_order_acquire);
}

int32_t raft_handle_leader_id(const raft_handle_t *handle) {
    return atomic_load_explicit(&handle->leader_id, memory_order_acquire);
}

void raft_propose_writeset(raft_handle_t *handle, raft_write_set_t *ws) {
    /* Update high watermark (monotonic max) */
    uint64_t old_hw = atomic_load_explicit(&handle->high_watermark,
                                            memory_order_relaxed);
    while (ws->seq > old_hw) {
        if (atomic_compare_exchange_weak_explicit(&handle->high_watermark,
                                                   &old_hw, ws->seq,
                                                   memory_order_release,
                                                   memory_order_relaxed))
            break;
    }

    while (!pq_push(handle->queue, ws))
        sched_yield();
}

uint64_t raft_committed_seq(const raft_handle_t *handle) {
    return atomic_load_explicit(&handle->committed_seq, memory_order_acquire);
}

uint64_t raft_faulted_seq(const raft_handle_t *handle) {
    return atomic_load_explicit(&handle->faulted_seq, memory_order_acquire);
}

#define RAFT_MAX_INFLIGHT 2048

bool raft_has_capacity(const raft_handle_t *handle) {
    uint64_t hw = atomic_load_explicit(&handle->high_watermark, memory_order_acquire);
    uint64_t committed = atomic_load_explicit(&handle->committed_seq, memory_order_acquire);
    return (hw - committed) < RAFT_MAX_INFLIGHT;
}

void raft_worker_begin(raft_handle_t *handle, int worker_id) {
    if (worker_id < 0 || (uint32_t)worker_id >= MAX_WORKERS) return;
    uint64_t hw = atomic_load_explicit(&handle->high_watermark,
                                        memory_order_acquire);
    atomic_store_explicit(&handle->worker_watermark[worker_id], hw,
                          memory_order_release);
}

void raft_worker_committed(raft_handle_t *handle, int worker_id, uint64_t seq) {
    if (worker_id < 0 || (uint32_t)worker_id >= MAX_WORKERS) return;
    atomic_store_explicit(&handle->worker_watermark[worker_id], seq,
                          memory_order_release);
}

void raft_worker_idle(raft_handle_t *handle, int worker_id) {
    if (worker_id < 0 || (uint32_t)worker_id >= MAX_WORKERS) return;
    atomic_store_explicit(&handle->worker_watermark[worker_id], UINT64_MAX,
                          memory_order_release);
}

/* ---- Write-set helpers ---- */

void raft_write_set_init(raft_write_set_t *ws) {
    memset(ws, 0, sizeof(*ws));
}

void raft_write_set_free(raft_write_set_t *ws) {
    for (uint32_t i = 0; i < ws->op_count; i++) {
        free(ws->ops[i].key);
        free(ws->ops[i].value);
    }
    free(ws->ops);
    memset(ws, 0, sizeof(*ws));
}

int raft_write_set_add_put(raft_write_set_t *ws, const char *key,
                           const void *value, uint32_t value_len) {
    if (ws->op_count >= ws->op_cap) {
        uint32_t new_cap = ws->op_cap ? ws->op_cap * 2 : 8;
        raft_kv_op_t *tmp = realloc(ws->ops, new_cap * sizeof(raft_kv_op_t));
        if (!tmp) return -1;
        ws->ops = tmp;
        ws->op_cap = new_cap;
    }
    raft_kv_op_t *op = &ws->ops[ws->op_count++];
    op->op = RAFT_KV_PUT;
    op->key = strdup(key);
    op->value = malloc(value_len);
    if (!op->key || !op->value) return -1;
    memcpy(op->value, value, value_len);
    op->value_len = value_len;
    return 0;
}

int raft_write_set_add_delete(raft_write_set_t *ws, const char *key) {
    if (ws->op_count >= ws->op_cap) {
        uint32_t new_cap = ws->op_cap ? ws->op_cap * 2 : 8;
        raft_kv_op_t *tmp = realloc(ws->ops, new_cap * sizeof(raft_kv_op_t));
        if (!tmp) return -1;
        ws->ops = tmp;
        ws->op_cap = new_cap;
    }
    raft_kv_op_t *op = &ws->ops[ws->op_count++];
    op->op = RAFT_KV_DELETE;
    op->key = strdup(key);
    op->value = NULL;
    op->value_len = 0;
    if (!op->key) return -1;
    return 0;
}
