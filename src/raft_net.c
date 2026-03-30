#include "raft_net.h"
#include "raft_rpc.h"

#include <liburing.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <netdb.h>

/* ---- constants ---- */

#define RING_DEPTH       64
#define RECV_BUF_SIZE    (256 * 1024)  /* 256 KB per peer recv buffer */
#define MAX_SEND_QUEUE   256           /* max queued send buffers per peer */
#define RECONNECT_MS     1000          /* reconnect interval */

/* ---- CQE user_data encoding ---- */

/* We encode the operation type and peer index in the user_data field:
 * bits [63:56] = op type, bits [31:0] = peer index or fd */
typedef enum {
    OP_ACCEPT   = 1,
    OP_CONNECT  = 2,
    OP_RECV     = 3,
    OP_SEND     = 4,
    OP_TIMEOUT  = 5,
} op_type_t;

static uint64_t encode_ud(op_type_t op, uint32_t idx) {
    return ((uint64_t)op << 56) | (uint64_t)idx;
}

static op_type_t decode_op(uint64_t ud) {
    return (op_type_t)(ud >> 56);
}

static uint32_t decode_idx(uint64_t ud) {
    return (uint32_t)(ud & 0xFFFFFFFF);
}

/* ---- peer connection state ---- */

typedef enum {
    PEER_DISCONNECTED,
    PEER_CONNECTING,
    PEER_CONNECTED,
    PEER_SELF,           /* this node — never connect */
    PEER_ACCEPTED,       /* inbound connection */
} peer_state_t;

typedef struct {
    int          fd;
    peer_state_t state;
    char         host[256];
    uint16_t     port;

    /* Receive buffer with frame reassembly */
    uint8_t     *recv_buf;
    uint32_t     recv_len;      /* bytes currently buffered */
    bool         recv_pending;  /* recv SQE submitted */

    /* Send queue */
    struct send_item {
        void    *data;
        uint32_t len;
        uint32_t sent;
    } send_queue[MAX_SEND_QUEUE];
    uint32_t     send_head;
    uint32_t     send_tail;
    bool         send_pending;  /* send SQE submitted */

    /* Reconnect timing */
    uint64_t     reconnect_at_ms;

    /* For accepted connections: the peer_id we've identified this as.
     * Set to UINT32_MAX until a message arrives revealing the sender. */
    uint32_t     identified_as;
} peer_conn_t;

struct raft_net {
    uint32_t          node_id;
    uint32_t          peer_count;
    peer_conn_t      *peers;          /* [peer_count] for outbound */

    /* Accepted (inbound) connections */
    peer_conn_t      *accepted;       /* dynamic array */
    uint32_t          accepted_count;
    uint32_t          accepted_cap;

    int               listen_fd;
    struct io_uring   ring;

    raft_net_recv_fn  on_recv;
    void             *user_ctx;

    bool              accept_pending;
};

/* ---- socket helpers ---- */

static int make_listen_socket(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
        .sin_addr   = { .s_addr = INADDR_ANY },
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int resolve_and_connect(const char *host, uint16_t port) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return -1;

    int fd = socket(res->ai_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    return fd;
}

/* ---- io_uring submission helpers ---- */

static void submit_accept(raft_net_t *net) {
    if (net->accept_pending) return;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&net->ring);
    if (!sqe) return;

    io_uring_prep_accept(sqe, net->listen_fd, NULL, NULL, SOCK_NONBLOCK);
    io_uring_sqe_set_data64(sqe, encode_ud(OP_ACCEPT, 0));
    net->accept_pending = true;
}

static void submit_recv(raft_net_t *net, peer_conn_t *peer, uint32_t idx) {
    if (peer->recv_pending || peer->fd < 0) return;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&net->ring);
    if (!sqe) return;

    uint32_t space = RECV_BUF_SIZE - peer->recv_len;
    io_uring_prep_recv(sqe, peer->fd,
                       peer->recv_buf + peer->recv_len, space, 0);
    io_uring_sqe_set_data64(sqe, encode_ud(OP_RECV, idx));
    peer->recv_pending = true;
}

static void submit_send(raft_net_t *net, peer_conn_t *peer, uint32_t idx) {
    if (peer->send_pending || peer->fd < 0) return;
    if (peer->send_head == peer->send_tail) return;

    struct send_item *item = &peer->send_queue[peer->send_head % MAX_SEND_QUEUE];
    uint32_t remaining = item->len - item->sent;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&net->ring);
    if (!sqe) return;

    io_uring_prep_send(sqe, peer->fd,
                       (uint8_t *)item->data + item->sent, remaining, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(sqe, encode_ud(OP_SEND, idx));
    peer->send_pending = true;
}

static void submit_connect(raft_net_t *net, uint32_t idx) {
    peer_conn_t *peer = &net->peers[idx];
    int fd = resolve_and_connect(peer->host, peer->port);
    if (fd < 0) {
        peer->state = PEER_DISCONNECTED;
        return;
    }

    peer->fd = fd;
    peer->state = PEER_CONNECTING;

    /* Use a poll SQE to detect when connect completes (fd becomes writable) */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&net->ring);
    if (!sqe) { close(fd); peer->fd = -1; peer->state = PEER_DISCONNECTED; return; }

    io_uring_prep_poll_add(sqe, fd, POLLOUT);
    io_uring_sqe_set_data64(sqe, encode_ud(OP_CONNECT, idx));
}

static void peer_disconnect(peer_conn_t *peer) {
    if (peer->fd >= 0) { close(peer->fd); peer->fd = -1; }
    peer->state = PEER_DISCONNECTED;
    peer->recv_len = 0;
    peer->recv_pending = false;
    peer->send_pending = false;

    /* Drop unsent data */
    while (peer->send_head != peer->send_tail) {
        struct send_item *item = &peer->send_queue[peer->send_head % MAX_SEND_QUEUE];
        free(item->data);
        peer->send_head++;
    }
}

/* Process fully framed messages in the recv buffer. */
static void process_recv_buf(raft_net_t *net, peer_conn_t *peer,
                             uint32_t peer_id) {
    while (peer->recv_len >= RAFT_RPC_HEADER_SIZE) {
        uint32_t payload_len = raft_rpc_frame_len(peer->recv_buf);
        uint32_t frame_total = RAFT_RPC_HEADER_SIZE + payload_len;

        if (peer->recv_len < frame_total) break;  /* incomplete frame */

        /* Deliver payload (type + body, after length header) */
        net->on_recv(peer_id,
                     peer->recv_buf + RAFT_RPC_HEADER_SIZE,
                     payload_len,
                     net->user_ctx);

        /* Shift remaining data */
        uint32_t remaining = peer->recv_len - frame_total;
        if (remaining > 0)
            memmove(peer->recv_buf, peer->recv_buf + frame_total, remaining);
        peer->recv_len = remaining;
    }
}

/* ---- public API ---- */

int raft_net_create(uint32_t node_id, const raft_peer_addr_t *peers,
                    uint32_t peer_count, uint16_t listen_port,
                    raft_net_recv_fn on_recv, void *user_ctx,
                    raft_net_t **out) {
    raft_net_t *net = calloc(1, sizeof(*net));
    if (!net) return -1;

    net->node_id = node_id;
    net->peer_count = peer_count;
    net->on_recv = on_recv;
    net->user_ctx = user_ctx;

    net->peers = calloc(peer_count, sizeof(peer_conn_t));
    if (!net->peers) { free(net); return -1; }

    for (uint32_t i = 0; i < peer_count; i++) {
        net->peers[i].fd = -1;
        if (i == node_id) {
            net->peers[i].state = PEER_SELF;
        } else {
            net->peers[i].state = PEER_DISCONNECTED;
            snprintf(net->peers[i].host, sizeof(net->peers[i].host),
                     "%s", peers[i].host);
            net->peers[i].port = peers[i].port;

            net->peers[i].recv_buf = malloc(RECV_BUF_SIZE);
            if (!net->peers[i].recv_buf) goto fail;
        }
    }

    /* Accepted connections */
    net->accepted_cap = 16;
    net->accepted = calloc(net->accepted_cap, sizeof(peer_conn_t));
    if (!net->accepted) goto fail;

    /* Listen socket */
    net->listen_fd = make_listen_socket(listen_port);
    if (net->listen_fd < 0) goto fail;

    /* io_uring */
    if (io_uring_queue_init(RING_DEPTH, &net->ring, 0) < 0) goto fail;

    /* Start accepting */
    submit_accept(net);

    /* Start connecting to all peers */
    for (uint32_t i = 0; i < peer_count; i++) {
        if (i == node_id) continue;
        submit_connect(net, i);
    }

    io_uring_submit(&net->ring);

    *out = net;
    return 0;

fail:
    if (net->listen_fd >= 0) close(net->listen_fd);
    for (uint32_t i = 0; i < peer_count; i++)
        free(net->peers[i].recv_buf);
    free(net->accepted);
    free(net->peers);
    free(net);
    return -1;
}

void raft_net_destroy(raft_net_t *net) {
    if (!net) return;

    for (uint32_t i = 0; i < net->peer_count; i++) {
        peer_disconnect(&net->peers[i]);
        free(net->peers[i].recv_buf);
    }
    for (uint32_t i = 0; i < net->accepted_count; i++) {
        peer_disconnect(&net->accepted[i]);
        free(net->accepted[i].recv_buf);
    }

    if (net->listen_fd >= 0) close(net->listen_fd);
    io_uring_queue_exit(&net->ring);
    free(net->accepted);
    free(net->peers);
    free(net);
}

int raft_net_send(raft_net_t *net, uint32_t peer_id,
                  const void *data, uint32_t len) {
    if (peer_id >= net->peer_count || peer_id == net->node_id)
        return -1;

    peer_conn_t *peer = &net->peers[peer_id];
    if (peer->state != PEER_CONNECTED) return -1;

    uint32_t queued = peer->send_tail - peer->send_head;
    if (queued >= MAX_SEND_QUEUE) return -1;

    /* Copy the data (caller retains ownership of original) */
    void *copy = malloc(len);
    if (!copy) return -1;
    memcpy(copy, data, len);

    struct send_item *item = &peer->send_queue[peer->send_tail % MAX_SEND_QUEUE];
    item->data = copy;
    item->len  = len;
    item->sent = 0;
    peer->send_tail++;

    submit_send(net, peer, peer_id);
    return 0;
}

int raft_net_poll(raft_net_t *net, uint32_t timeout_ms) {
    io_uring_submit(&net->ring);

    struct io_uring_cqe *cqe;
    int rc;
    if (timeout_ms == 0) {
        /* Non-blocking: peek without syscall if nothing ready */
        rc = io_uring_peek_cqe(&net->ring, &cqe);
        if (rc == -EAGAIN) return 0;
        if (rc < 0) return -1;
    } else {
        struct __kernel_timespec ts = {
            .tv_sec  = timeout_ms / 1000,
            .tv_nsec = (long)(timeout_ms % 1000) * 1000000L,
        };
        rc = io_uring_wait_cqe_timeout(&net->ring, &cqe, &ts);
        if (rc == -ETIME) return 0;
        if (rc < 0) return -1;
    }

    /* Process all available CQEs */
    unsigned head;
    unsigned count = 0;
    io_uring_for_each_cqe(&net->ring, head, cqe) {
        count++;
        uint64_t ud = io_uring_cqe_get_data64(cqe);
        op_type_t op = decode_op(ud);
        uint32_t idx = decode_idx(ud);
        int32_t res = cqe->res;

        switch (op) {
        case OP_ACCEPT: {
            net->accept_pending = false;
            if (res >= 0) {
                int afd = res;
                int opt = 1;
                setsockopt(afd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

                /* Grow accepted array if needed */
                if (net->accepted_count >= net->accepted_cap) {
                    uint32_t new_cap = net->accepted_cap * 2;
                    peer_conn_t *tmp = realloc(net->accepted,
                                                new_cap * sizeof(peer_conn_t));
                    if (tmp) {
                        net->accepted = tmp;
                        net->accepted_cap = new_cap;
                    } else {
                        close(afd);
                        break;
                    }
                }

                uint32_t ai = net->accepted_count++;
                memset(&net->accepted[ai], 0, sizeof(peer_conn_t));
                net->accepted[ai].fd = afd;
                net->accepted[ai].state = PEER_ACCEPTED;
                net->accepted[ai].identified_as = UINT32_MAX;
                net->accepted[ai].recv_buf = malloc(RECV_BUF_SIZE);
                if (net->accepted[ai].recv_buf) {
                    /* Use peer_count + ai as the idx for accepted conns */
                    submit_recv(net, &net->accepted[ai],
                                net->peer_count + ai);
                }
            }
            submit_accept(net);
            break;
        }

        case OP_CONNECT: {
            peer_conn_t *peer = &net->peers[idx];
            /* Check if connect succeeded */
            int err = 0;
            socklen_t errlen = sizeof(err);
            getsockopt(peer->fd, SOL_SOCKET, SO_ERROR, &err, &errlen);

            if (err == 0 && res >= 0) {
                peer->state = PEER_CONNECTED;
                peer->reconnect_at_ms = 0;
                submit_recv(net, peer, idx);

                /* Send identification message so the remote side knows
                 * which peer this outbound connection belongs to. */
                uint32_t ident_len;
                void *ident = raft_rpc_encode_ident(net->node_id, &ident_len);
                if (ident) {
                    /* Queue directly (we own the data) */
                    struct send_item *item =
                        &peer->send_queue[peer->send_tail % MAX_SEND_QUEUE];
                    item->data = ident;
                    item->len  = ident_len;
                    item->sent = 0;
                    peer->send_tail++;
                    submit_send(net, peer, idx);
                }
            } else {
                peer_disconnect(peer);
            }
            break;
        }

        case OP_RECV: {
            peer_conn_t *peer;
            uint32_t peer_id;

            if (idx < net->peer_count) {
                peer = &net->peers[idx];
                peer_id = idx;
            } else {
                uint32_t ai = idx - net->peer_count;
                if (ai >= net->accepted_count) break;
                peer = &net->accepted[ai];
                peer_id = peer->identified_as;
            }

            peer->recv_pending = false;

            if (res <= 0) {
                /* Connection closed or error */
                peer_disconnect(peer);
                break;
            }

            peer->recv_len += (uint32_t)res;

            /* For accepted connections, try to identify the peer.
             * The first message should be RAFT_MSG_IDENT (sent on connect). */
            if (peer_id == UINT32_MAX && peer->recv_len >= RAFT_RPC_HEADER_SIZE) {
                uint32_t payload_len = raft_rpc_frame_len(peer->recv_buf);
                uint32_t frame_total = RAFT_RPC_HEADER_SIZE + payload_len;
                if (peer->recv_len >= frame_total && payload_len >= 5) {
                    const uint8_t *payload = peer->recv_buf + RAFT_RPC_HEADER_SIZE;
                    uint8_t msg_type = payload[0];
                    if (msg_type == RAFT_MSG_IDENT) {
                        uint32_t sender_id;
                        memcpy(&sender_id, payload + 1, 4);
                        sender_id = ntohl(sender_id);
                        if (sender_id < net->peer_count) {
                            peer->identified_as = sender_id;
                            peer_id = sender_id;
                        }
                        /* Consume the ident frame */
                        uint32_t remaining = peer->recv_len - frame_total;
                        if (remaining > 0)
                            memmove(peer->recv_buf,
                                    peer->recv_buf + frame_total, remaining);
                        peer->recv_len = remaining;
                    }
                }
            }

            if (peer_id != UINT32_MAX)
                process_recv_buf(net, peer, peer_id);

            submit_recv(net, peer, idx);
            break;
        }

        case OP_SEND: {
            peer_conn_t *peer;
            if (idx < net->peer_count) {
                peer = &net->peers[idx];
            } else {
                uint32_t ai = idx - net->peer_count;
                if (ai >= net->accepted_count) break;
                peer = &net->accepted[ai];
            }

            peer->send_pending = false;

            if (res <= 0) {
                peer_disconnect(peer);
                break;
            }

            struct send_item *item =
                &peer->send_queue[peer->send_head % MAX_SEND_QUEUE];
            item->sent += (uint32_t)res;

            if (item->sent >= item->len) {
                free(item->data);
                peer->send_head++;
            }

            submit_send(net, peer, idx);
            break;
        }

        case OP_TIMEOUT:
            break;
        }
    }

    io_uring_cq_advance(&net->ring, count);

    /* Attempt reconnection for disconnected outbound peers with backoff */
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t t = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
        for (uint32_t i = 0; i < net->peer_count; i++) {
            if (i == net->node_id) continue;
            if (net->peers[i].state == PEER_DISCONNECTED && t >= net->peers[i].reconnect_at_ms) {
                submit_connect(net, i);
                /* Backoff: 100ms, 200ms, 400ms, ... capped at 5s */
                uint64_t delay = net->peers[i].reconnect_at_ms ?
                    (t - net->peers[i].reconnect_at_ms + 100) * 2 : 100;
                if (delay > 5000) delay = 5000;
                net->peers[i].reconnect_at_ms = t + delay;
            }
        }
    }

    return 0;
}
