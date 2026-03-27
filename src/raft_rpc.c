#include "raft_rpc.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

/* ---- wire helpers ---- */

static void w64(uint8_t **p, uint64_t v) {
    uint32_t hi = htonl((uint32_t)(v >> 32));
    uint32_t lo = htonl((uint32_t)(v & 0xFFFFFFFF));
    memcpy(*p, &hi, 4); *p += 4;
    memcpy(*p, &lo, 4); *p += 4;
}

static void w32(uint8_t **p, uint32_t v) {
    uint32_t nv = htonl(v);
    memcpy(*p, &nv, 4); *p += 4;
}

static void w8(uint8_t **p, uint8_t v) {
    **p = v; (*p)++;
}

static uint64_t r64(const uint8_t **p) {
    uint32_t hi, lo;
    memcpy(&hi, *p, 4); *p += 4;
    memcpy(&lo, *p, 4); *p += 4;
    return ((uint64_t)ntohl(hi) << 32) | (uint64_t)ntohl(lo);
}

static uint32_t r32(const uint8_t **p) {
    uint32_t v;
    memcpy(&v, *p, 4); *p += 4;
    return ntohl(v);
}

static uint8_t r8(const uint8_t **p) {
    uint8_t v = **p; (*p)++;
    return v;
}

static void *frame_alloc(uint8_t type, uint32_t payload_size, uint32_t *out_total) {
    uint32_t frame_payload = 1 + payload_size;
    uint32_t total = 4 + frame_payload;
    uint8_t *buf = malloc(total);
    if (!buf) return NULL;

    uint32_t nlen = htonl(frame_payload);
    memcpy(buf, &nlen, 4);
    buf[4] = type;

    *out_total = total;
    return buf;
}

/* ---- encode ---- */

void *raft_rpc_encode_vote_req(const msg_requestvote_t *req, uint32_t *out_len) {
    /* term(8) + candidate_id(4) + last_log_idx(8) + last_log_term(8) = 28 */
    uint32_t total;
    uint8_t *buf = frame_alloc(RAFT_MSG_VOTE_REQ, 28, &total);
    if (!buf) return NULL;

    uint8_t *p = buf + 5;
    w64(&p, (uint64_t)req->term);
    w32(&p, (uint32_t)req->candidate_id);
    w64(&p, (uint64_t)req->last_log_idx);
    w64(&p, (uint64_t)req->last_log_term);

    *out_len = total;
    return buf;
}

void *raft_rpc_encode_vote_resp(const msg_requestvote_response_t *resp, uint32_t *out_len) {
    /* term(8) + vote_granted(4) = 12 */
    uint32_t total;
    uint8_t *buf = frame_alloc(RAFT_MSG_VOTE_RESP, 12, &total);
    if (!buf) return NULL;

    uint8_t *p = buf + 5;
    w64(&p, (uint64_t)resp->term);
    w32(&p, (uint32_t)resp->vote_granted);

    *out_len = total;
    return buf;
}

void *raft_rpc_encode_append_req(const msg_appendentries_t *req, uint32_t *out_len) {
    /* Fixed: term(8) + prev_log_idx(8) + prev_log_term(8)
     *        + leader_commit(8) + n_entries(4) = 36
     * Per entry: term(8) + id(4) + type(4) + data_len(4) + data */
    uint32_t entries_size = 0;
    for (int i = 0; i < req->n_entries; i++)
        entries_size += 8 + 4 + 4 + 4 + req->entries[i].data.len;

    uint32_t total;
    uint8_t *buf = frame_alloc(RAFT_MSG_APPEND_REQ, 36 + entries_size, &total);
    if (!buf) return NULL;

    uint8_t *p = buf + 5;
    w64(&p, (uint64_t)req->term);
    w64(&p, (uint64_t)req->prev_log_idx);
    w64(&p, (uint64_t)req->prev_log_term);
    w64(&p, (uint64_t)req->leader_commit);
    w32(&p, (uint32_t)req->n_entries);

    for (int i = 0; i < req->n_entries; i++) {
        w64(&p, (uint64_t)req->entries[i].term);
        w32(&p, (uint32_t)req->entries[i].id);
        w32(&p, (uint32_t)req->entries[i].type);
        w32(&p, (uint32_t)req->entries[i].data.len);
        if (req->entries[i].data.len > 0) {
            memcpy(p, req->entries[i].data.buf, req->entries[i].data.len);
            p += req->entries[i].data.len;
        }
    }

    *out_len = total;
    return buf;
}

void *raft_rpc_encode_append_resp(const msg_appendentries_response_t *resp, uint32_t *out_len) {
    /* term(8) + success(4) + current_idx(8) + first_idx(8) = 28 */
    uint32_t total;
    uint8_t *buf = frame_alloc(RAFT_MSG_APPEND_RESP, 28, &total);
    if (!buf) return NULL;

    uint8_t *p = buf + 5;
    w64(&p, (uint64_t)resp->term);
    w32(&p, (uint32_t)resp->success);
    w64(&p, (uint64_t)resp->current_idx);
    w64(&p, (uint64_t)resp->first_idx);

    *out_len = total;
    return buf;
}

void *raft_rpc_encode_ident(uint32_t node_id, uint32_t *out_len) {
    uint32_t total;
    uint8_t *buf = frame_alloc(RAFT_MSG_IDENT, 4, &total);
    if (!buf) return NULL;

    uint8_t *p = buf + 5;
    w32(&p, node_id);

    *out_len = total;
    return buf;
}

/* ---- decode ---- */

int raft_rpc_decode(const void *data, uint32_t len, raft_wire_msg_t *out) {
    if (len < 1) return -1;

    const uint8_t *p = data;
    uint8_t type = r8(&p);
    uint32_t rem = len - 1;

    out->type = (raft_msg_type_t)type;

    switch (type) {
    case RAFT_MSG_VOTE_REQ:
        if (rem < 28) return -1;
        out->vote_req.term           = (raft_term_t)r64(&p);
        out->vote_req.candidate_id   = (raft_node_id_t)r32(&p);
        out->vote_req.last_log_idx   = (raft_index_t)r64(&p);
        out->vote_req.last_log_term  = (raft_term_t)r64(&p);
        return 0;

    case RAFT_MSG_VOTE_RESP:
        if (rem < 12) return -1;
        out->vote_resp.term         = (raft_term_t)r64(&p);
        out->vote_resp.vote_granted = (int)r32(&p);
        return 0;

    case RAFT_MSG_APPEND_REQ: {
        if (rem < 36) return -1;
        out->append_req.term           = (raft_term_t)r64(&p);
        out->append_req.prev_log_idx   = (raft_index_t)r64(&p);
        out->append_req.prev_log_term  = (raft_term_t)r64(&p);
        out->append_req.leader_commit  = (raft_index_t)r64(&p);
        out->append_req.n_entries      = (int)r32(&p);
        rem -= 36;

        if (out->append_req.n_entries > 0) {
            msg_entry_t *entries = calloc((size_t)out->append_req.n_entries,
                                          sizeof(msg_entry_t));
            if (!entries) return -1;

            for (int i = 0; i < out->append_req.n_entries; i++) {
                if (rem < 20) goto ae_fail;
                entries[i].term     = (raft_term_t)r64(&p); rem -= 8;
                entries[i].id       = (raft_entry_id_t)r32(&p); rem -= 4;
                entries[i].type     = (int)r32(&p); rem -= 4;
                uint32_t dlen       = r32(&p); rem -= 4;
                entries[i].data.len = dlen;
                if (dlen > 0) {
                    if (rem < dlen) goto ae_fail;
                    entries[i].data.buf = malloc(dlen);
                    if (!entries[i].data.buf) goto ae_fail;
                    memcpy(entries[i].data.buf, p, dlen);
                    p += dlen;
                    rem -= dlen;
                } else {
                    entries[i].data.buf = NULL;
                }
            }

            out->append_req.entries = entries;
            return 0;

        ae_fail:
            for (int j = 0; j < out->append_req.n_entries; j++)
                free(entries[j].data.buf);
            free(entries);
            return -1;
        } else {
            out->append_req.entries = NULL;
        }
        return 0;
    }

    case RAFT_MSG_APPEND_RESP:
        if (rem < 28) return -1;
        out->append_resp.term        = (raft_term_t)r64(&p);
        out->append_resp.success     = (int)r32(&p);
        out->append_resp.current_idx = (raft_index_t)r64(&p);
        out->append_resp.first_idx   = (raft_index_t)r64(&p);
        return 0;

    case RAFT_MSG_IDENT:
        if (rem < 4) return -1;
        out->ident_node_id = r32(&p);
        return 0;

    default:
        return -1;
    }
}

uint32_t raft_rpc_frame_len(const void *header) {
    uint32_t nlen;
    memcpy(&nlen, header, 4);
    return ntohl(nlen);
}
