#pragma once

#include <stdint.h>

typedef struct raft_net raft_net_t;

typedef struct {
    const char *host;
    uint16_t    port;
} raft_peer_addr_t;

/* Callback invoked when a complete message frame is received from a peer.
 * data points to the payload after the 4-byte length header.
 * len is the payload length (type byte + body). */
typedef void (*raft_net_recv_fn)(uint32_t from_id, const void *data,
                                 uint32_t len, void *user_ctx);

/* Create the networking layer.
 * node_id: this node's index in the peers array.
 * peers: array of all nodes (including self).
 * peer_count: number of nodes.
 * listen_port: port to listen on for incoming peer connections. */
int  raft_net_create(uint32_t node_id, const raft_peer_addr_t *peers,
                     uint32_t peer_count, uint16_t listen_port,
                     raft_net_recv_fn on_recv, void *user_ctx,
                     raft_net_t **out);

void raft_net_destroy(raft_net_t *net);

/* Send a framed message to a peer.  Caller retains ownership of data.
 * The 4-byte length header is prepended automatically — pass just the
 * raw message bytes (type + payload), i.e. the full encoded frame including
 * the header as produced by raft_rpc_encode_*. */
int  raft_net_send(raft_net_t *net, uint32_t peer_id,
                   const void *data, uint32_t len);

/* Poll io_uring for events, process completions, invoke callbacks.
 * timeout_ms: max time to wait (0 = non-blocking).
 * Returns 0 on success, -1 on error. */
int  raft_net_poll(raft_net_t *net, uint32_t timeout_ms);
