#define _GNU_SOURCE

#include "worker.h"
#include "raft_thread.h"

#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile bool g_running = true;

static void handle_signal(int sig) {
    (void)sig;
    g_running = false;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  -d <path>    SQLite database path (default: shift-js.db)\n"
            "  -p <port>    Listen port (default: 9000)\n"
            "  -w <count>   Worker thread count (default: number of CPUs)\n"
            "  -t           Enable TLS (certs loaded from KV)\n"
            "  -h           Show this help\n"
            "\n"
            "Auth options:\n"
            "  --auth-secret <key>    HMAC secret for auth cookies\n"
            "  --mgmt-secret <key>    Secret for /_mgmt/ endpoints (defaults to auth-secret)\n"
            "\n"
            "Raft options (enables clustering when --raft-id is set):\n"
            "  --raft-id <N>          This node's index (0-based)\n"
            "  --raft-peers <addrs>   Comma-separated host:port for all nodes\n"
            "  --raft-port <port>     Port for Raft peer TCP (default: 9100)\n",
            prog);
}

/* Parse "host1:port1,host2:port2,..." into arrays.
 * Returns node count, or 0 on error. */
static uint32_t parse_peers(const char *str,
                            const char ***out_hosts, uint16_t **out_ports) {
    uint32_t count = 1;
    for (const char *p = str; *p; p++)
        if (*p == ',') count++;

    const char **hosts = calloc(count, sizeof(char *));
    uint16_t *ports = calloc(count, sizeof(uint16_t));

    char *dup = strdup(str);
    char *tok = strtok(dup, ",");
    for (uint32_t i = 0; i < count && tok; i++) {
        char *colon = strrchr(tok, ':');
        if (!colon) { free(dup); free(hosts); free(ports); return 0; }
        *colon = '\0';
        hosts[i] = strdup(tok);
        ports[i] = (uint16_t)atoi(colon + 1);
        tok = strtok(NULL, ",");
    }
    free(dup);

    *out_hosts = hosts;
    *out_ports = ports;
    return count;
}

int main(int argc, char **argv) {
    const char *db_path     = "shift-js.db";
    uint16_t    port        = 9000;
    int         nworkers    = 0;
    bool        tls         = false;
    const char *auth_secret = NULL;
    const char *mgmt_secret = NULL;
    const char *log_server  = NULL;

    /* Raft options */
    int         raft_id    = -1;
    const char *raft_peers = NULL;
    uint16_t    raft_port  = 9100;
    int         batch_ms   = 2;
    int         batch_max  = 256;

    static struct option long_opts[] = {
        { "auth-secret", required_argument, NULL, 'S' },
        { "mgmt-secret", required_argument, NULL, 'G' },
        { "log-server",  required_argument, NULL, 'L' },
        { "raft-id",     required_argument, NULL, 'R' },
        { "raft-peers",  required_argument, NULL, 'P' },
        { "raft-port",   required_argument, NULL, 'Q' },
        { "batch-ms",    required_argument, NULL, 'B' },
        { "batch-max",   required_argument, NULL, 'M' },
        { NULL, 0, NULL, 0 },
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:p:w:th", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd': db_path     = optarg; break;
        case 'p': port        = (uint16_t)atoi(optarg); break;
        case 'w': nworkers    = atoi(optarg); break;
        case 't': tls         = true; break;
        case 'h': usage(argv[0]); return 0;
        case 'S': auth_secret = optarg; break;
        case 'G': mgmt_secret = optarg; break;
        case 'L': log_server  = optarg; break;
        case 'R': raft_id     = atoi(optarg); break;
        case 'P': raft_peers  = optarg; break;
        case 'Q': raft_port   = (uint16_t)atoi(optarg); break;
        case 'B': batch_ms    = atoi(optarg); break;
        case 'M': batch_max   = atoi(optarg); break;
        default:  usage(argv[0]); return 1;
        }
    }

    if (!mgmt_secret) mgmt_secret = auth_secret;

    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    int max_workers = (raft_id >= 0) ? (int)ncpus - 1 : (int)ncpus;
    if (max_workers < 1) max_workers = 1;
    if (nworkers <= 0) nworkers = max_workers;
    if (nworkers > max_workers) nworkers = max_workers;

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    /* ---- Raft setup ---- */
    raft_handle_t *raft = NULL;

    if (raft_id >= 0) {
        if (!raft_peers) {
            fprintf(stderr, "error: --raft-peers is required when --raft-id is set\n");
            return 1;
        }

        const char **peer_hosts = NULL;
        uint16_t    *peer_ports = NULL;
        uint32_t     node_count = parse_peers(raft_peers, &peer_hosts, &peer_ports);

        if (node_count == 0 || (uint32_t)raft_id >= node_count) {
            fprintf(stderr, "error: invalid --raft-peers or --raft-id out of range\n");
            return 1;
        }

        raft_config_t rcfg = {
            .node_id              = (uint32_t)raft_id,
            .node_count           = node_count,
            .db_path              = db_path,
            .raft_port            = raft_port,
            .peer_hosts           = peer_hosts,
            .peer_ports           = peer_ports,
            .election_base_ms     = 1000,
            .heartbeat_interval_ms = 200,
            .batch_interval_ms    = (uint64_t)batch_ms,
            .batch_max_entries    = (uint32_t)batch_max,
            .worker_count         = (uint32_t)nworkers,
            .raft_core            = (uint32_t)ncpus - 1,
            .running              = &g_running,
        };

        if (raft_handle_create(&rcfg, &raft) != 0) {
            fprintf(stderr, "error: failed to start Raft thread\n");
            for (uint32_t i = 0; i < node_count; i++) free((char *)peer_hosts[i]);
            free(peer_hosts);
            free(peer_ports);
            return 1;
        }

        for (uint32_t i = 0; i < node_count; i++) free((char *)peer_hosts[i]);
        free(peer_hosts);
        free(peer_ports);

        printf("shift-js: raft enabled (node %d, %u peers, raft port %d)\n",
               raft_id, node_count, raft_port);
    }

    printf("shift-js: %d workers, port %d, db %s%s\n",
           nworkers, port, db_path, tls ? ", TLS" : "");

    sjs_worker_config_t *configs = calloc((size_t)nworkers, sizeof(*configs));
    pthread_t           *threads = calloc((size_t)nworkers, sizeof(*threads));

    for (int i = 0; i < nworkers; i++) {
        configs[i] = (sjs_worker_config_t){
            .worker_id   = i,
            .worker_core = i,
            .db_path     = db_path,
            .port        = port,
            .running     = &g_running,
            .tls         = tls,
            .auth_secret = auth_secret,
            .mgmt_secret = mgmt_secret,
            .log_server  = log_server,
            .raft        = raft,
        };
        pthread_create(&threads[i], NULL, sjs_worker_fn, &configs[i]);
    }

    for (int i = 0; i < nworkers; i++)
        pthread_join(threads[i], NULL);

    if (raft)
        raft_handle_destroy(raft);

    printf("shift-js: shutdown complete\n");

    free(threads);
    free(configs);
    return 0;
}
