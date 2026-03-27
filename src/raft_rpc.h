#pragma once

#include <stddef.h>  /* needed before raft.h for size_t */
#include <raft.h>
#include <stdbool.h>
#include <stdint.h>

/* ---- Message types on the wire ---- */
typedef enum {
    RAFT_MSG_VOTE_REQ    = 1,
    RAFT_MSG_VOTE_RESP   = 2,
    RAFT_MSG_APPEND_REQ  = 3,
    RAFT_MSG_APPEND_RESP = 4,
    RAFT_MSG_IDENT       = 5,
    RAFT_MSG_SNAP_OFFER = 6,  /* leader tells follower: you need a snapshot */
    RAFT_MSG_SNAP_REQ   = 7,  /* follower requests delta with its max KV seq */
    RAFT_MSG_SNAP_DATA  = 8,  /* leader sends KV delta entries */
} raft_msg_type_t;

/* ---- Encode (returns malloc'd framed buffer, caller frees) ---- */
/* Frame format: [4 bytes: payload length (network order)] [payload]
 * Payload: [1 byte: type] [type-specific fields] */

void *raft_rpc_encode_vote_req(const msg_requestvote_t *req, uint32_t *out_len);
void *raft_rpc_encode_vote_resp(const msg_requestvote_response_t *resp, uint32_t *out_len);
void *raft_rpc_encode_append_req(const msg_appendentries_t *req, uint32_t *out_len);
void *raft_rpc_encode_append_resp(const msg_appendentries_response_t *resp, uint32_t *out_len);
void *raft_rpc_encode_ident(uint32_t node_id, uint32_t *out_len);

/* ---- Decode ---- */

/* Tagged union for decoded messages. */
typedef struct {
    raft_msg_type_t type;
    union {
        msg_requestvote_t          vote_req;
        msg_requestvote_response_t vote_resp;
        msg_appendentries_t        append_req;
        msg_appendentries_response_t append_resp;
        uint32_t                   ident_node_id;
    };
} raft_wire_msg_t;

/* Decode payload bytes (after 4-byte length header is stripped).
 * For APPEND_REQ, entries[] is malloc'd — caller frees entries and
 * each entry's data.buf. */
int raft_rpc_decode(const void *data, uint32_t len, raft_wire_msg_t *out);

/* ---- Frame helpers ---- */

#define RAFT_RPC_HEADER_SIZE 4

/* Read payload length from a 4-byte network-order header. */
uint32_t raft_rpc_frame_len(const void *header);
