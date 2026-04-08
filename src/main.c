#define _GNU_SOURCE

#include "worker.h"
#include "ctl.h"
#include "code_db.h"
#include "code_store.h"
#include "code_server.h"
#include "code_client.h"
#include "preprocessor.h"
#include "ejs.h"
#include "typescript.h"
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

/* ======================================================================
 * sjs serve  (all-in-one server, current behavior)
 * ====================================================================== */

static void serve_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s serve [options]\n"
            "  -d <path>    SQLite database path (default: sjs.db)\n"
            "  -p <port>    Listen port (default: 9000)\n"
            "  -w <count>   Worker thread count (default: number of CPUs)\n"
            "  -t           Enable TLS (certs loaded from KV)\n"
            "  -h           Show this help\n"
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
    if (!hosts || !ports || !dup) {
        free(hosts); free(ports); free(dup);
        return 0;
    }

    char *tok = strtok(dup, ",");
    for (uint32_t i = 0; i < count && tok; i++) {
        char *colon = strrchr(tok, ':');
        if (!colon) { free(dup); free(hosts); free(ports); return 0; }
        *colon = '\0';
        hosts[i] = strdup(tok);
        if (!hosts[i]) { free(dup); free(hosts); free(ports); return 0; }
        ports[i] = (uint16_t)atoi(colon + 1);
        tok = strtok(NULL, ",");
    }
    free(dup);

    *out_hosts = hosts;
    *out_ports = ports;
    return count;
}

static int cmd_serve(int argc, char **argv) {
    const char *db_path = "sjs.db";
    uint16_t    port    = 9000;
    int         nworkers = 0;
    bool        tls      = false;
    bool        no_log   = false;

    /* Raft options */
    int         raft_id    = -1;
    const char *raft_peers = NULL;
    uint16_t    raft_port  = 9100;
    int         batch_ms   = 2;
    int         batch_max  = 256;

    static struct option long_opts[] = {
        { "raft-id",    required_argument, NULL, 'R' },
        { "raft-peers", required_argument, NULL, 'P' },
        { "raft-port",  required_argument, NULL, 'Q' },
        { "batch-ms",   required_argument, NULL, 'B' },
        { "batch-max",  required_argument, NULL, 'M' },
        { "no-log",     no_argument,       NULL, 'N' },
        { NULL, 0, NULL, 0 },
    };

    optind = 1;  /* reset getopt for subcommand parsing */
    int opt;
    while ((opt = getopt_long(argc, argv, "d:p:w:th", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd': db_path    = optarg; break;
        case 'p': port       = (uint16_t)atoi(optarg); break;
        case 'w': nworkers   = atoi(optarg); break;
        case 't': tls        = true; break;
        case 'h': serve_usage(argv[0]); return 0;
        case 'R': raft_id    = atoi(optarg); break;
        case 'P': raft_peers = optarg; break;
        case 'Q': raft_port  = (uint16_t)atoi(optarg); break;
        case 'B': batch_ms   = atoi(optarg); break;
        case 'M': batch_max  = atoi(optarg); break;
        case 'N': no_log    = true; break;
        default:  serve_usage(argv[0]); return 1;
        }
    }

    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    int max_workers = (raft_id >= 0) ? (int)ncpus - 1 : (int)ncpus;
    if (max_workers < 1) max_workers = 1;
    if (nworkers <= 0) nworkers = max_workers;
    if (nworkers > max_workers) nworkers = max_workers;

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    /* Preprocessor registry */
    sjs_preprocessor_registry_t preprocessors;
    sjs_preprocessor_init(&preprocessors);
    sjs_preprocessor_register(&preprocessors, ".ejs", sjs_ejs_transform, NULL);
    sjs_preprocessor_register(&preprocessors, ".ts",  sjs_typescript_transform, NULL);
    sjs_preprocessor_register(&preprocessors, ".tsx", sjs_typescript_transform, NULL);

    /* Raft setup */
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

        printf("sjs: raft enabled (node %d, %u peers, raft port %d)\n",
               raft_id, node_count, raft_port);
    }

    printf("sjs: %d workers, port %d, db %s%s\n",
           nworkers, port, db_path, tls ? ", TLS" : "");

    /* Code database + in-memory bytecode store */
    code_db_t *cdb = NULL;
    char code_path[512];
    snprintf(code_path, sizeof(code_path), "%s.code", db_path);
    if (code_db_open(code_path, &cdb) != 0) {
        fprintf(stderr, "sjs: warning: code_db_open failed (%s)\n", code_path);
        /* Non-fatal — code_db features disabled */
    }

    code_store_t *code_store = code_store_create();

    sjs_worker_config_t *configs = calloc((size_t)nworkers, sizeof(*configs));
    pthread_t           *threads = calloc((size_t)nworkers, sizeof(*threads));

    for (int i = 0; i < nworkers; i++) {
        configs[i] = (sjs_worker_config_t){
            .worker_id   = i,
            .num_workers = nworkers,
            .worker_core = i,
            .db_path     = db_path,
            .port        = port,
            .running     = &g_running,
            .tls         = tls,
            .preprocessors = &preprocessors,
            .raft        = raft,
            .no_log      = no_log,
            .code_db     = cdb,
            .code_store  = code_store,
        };
        pthread_create(&threads[i], NULL, sjs_worker_fn, &configs[i]);
    }

    for (int i = 0; i < nworkers; i++)
        pthread_join(threads[i], NULL);

    if (raft)
        raft_handle_destroy(raft);
    code_store_destroy(code_store);
    code_db_close(cdb);

    printf("sjs: shutdown complete\n");

    free(threads);
    free(configs);
    return 0;
}

/* ======================================================================
 * Top-level dispatch
 * ====================================================================== */

static void usage(void) {
    fprintf(stderr,
            "Usage: sjs <command> [options]\n\n"
            "Commands:\n"
            "  serve      Start all-in-one server (default)\n"
            "  ctl        Admin CLI (manage KV, deploy code, etc.)\n"
            "  login      Authenticate to a remote server\n"
            "\n"
            "Run 'sjs <command> -h' for command-specific help.\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        /* No subcommand — default to serve */
        return cmd_serve(argc, argv);
    }

    const char *cmd = argv[1];

    /* Shift argv so subcommand sees itself as argv[0] */
    if (strcmp(cmd, "serve") == 0) {
        argv[1] = argv[0];  /* keep program name for usage messages */
        return cmd_serve(argc - 1, argv + 1);
    } else if (strcmp(cmd, "ctl") == 0) {
        return cmd_ctl(argc - 2, argv + 2);
    } else if (strcmp(cmd, "login") == 0) {
        fprintf(stderr, "sjs login: not yet implemented\n");
        return 1;
    } else if (strcmp(cmd, "code") == 0) {
        /* sjs code -d <db_path> -p <port> */
        const char *db = "code.db";
        uint16_t cport = 9001;
        for (int a = 2; a < argc; a++) {
            if (strcmp(argv[a], "-d") == 0 && a + 1 < argc) db = argv[++a];
            else if (strcmp(argv[a], "-p") == 0 && a + 1 < argc) cport = (uint16_t)atoi(argv[++a]);
        }
        code_server_config_t ccfg = {
            .db_path     = db,
            .database_id = "default",
            .port        = cport,
            .store       = NULL,
        };
        return code_server_run(&ccfg);
    } else if (strcmp(cmd, "replay") == 0) {
        fprintf(stderr, "sjs replay: not yet implemented\n");
        return 1;
    } else if (strcmp(cmd, "worker") == 0) {
        /* sjs worker -d <db> -p <port> --code-server=host:port */
        const char *db = "sjs.db";
        uint16_t wport = 9000;
        int nw = 0;
        const char *cs_host = NULL;
        uint16_t cs_port = 9001;

        for (int a = 2; a < argc; a++) {
            if (strcmp(argv[a], "-d") == 0 && a + 1 < argc) db = argv[++a];
            else if (strcmp(argv[a], "-p") == 0 && a + 1 < argc) wport = (uint16_t)atoi(argv[++a]);
            else if (strcmp(argv[a], "-w") == 0 && a + 1 < argc) nw = atoi(argv[++a]);
            else if (strncmp(argv[a], "--code-server=", 14) == 0) {
                const char *val = argv[a] + 14;
                char *colon = strrchr(val, ':');
                if (colon) {
                    cs_host = strndup(val, (size_t)(colon - val));
                    cs_port = (uint16_t)atoi(colon + 1);
                } else {
                    cs_host = val;
                }
            }
        }

        if (!cs_host) {
            fprintf(stderr, "sjs worker: --code-server=host:port is required\n");
            return 1;
        }

        signal(SIGINT,  handle_signal);
        signal(SIGTERM, handle_signal);

        long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
        if (nw <= 0) nw = (int)ncpus;

        code_store_t *wstore = code_store_create();

        /* Pull initial snapshot from code server */
        printf("sjs worker: connecting to code server %s:%d...\n", cs_host, cs_port);
        code_client_t *cclient = NULL;
        code_client_config_t cc_cfg = {
            .host    = cs_host,
            .port    = cs_port,
            .store   = wstore,
            .running = &g_running,
        };
        if (code_client_start(&cc_cfg, &cclient) != 0) {
            fprintf(stderr, "sjs worker: failed to pull from code server\n");
            code_store_destroy(wstore);
            return 1;
        }

        printf("sjs worker: code loaded, starting %d workers on port %d\n", nw, wport);

        sjs_preprocessor_registry_t wpp;
        sjs_preprocessor_init(&wpp);

        sjs_worker_config_t *wconfigs = calloc((size_t)nw, sizeof(*wconfigs));
        pthread_t *wthreads = calloc((size_t)nw, sizeof(*wthreads));
        for (int wi = 0; wi < nw; wi++) {
            wconfigs[wi] = (sjs_worker_config_t){
                .worker_id     = wi,
                .num_workers   = nw,
                .worker_core   = wi,
                .db_path       = db,
                .port          = wport,
                .running       = &g_running,
                .preprocessors = &wpp,
                .code_store    = wstore,
                .code_server_host = cs_host,
                .code_server_port = cs_port,
                .no_log        = true, /* TODO: connect to replay server */
            };
            pthread_create(&wthreads[wi], NULL, sjs_worker_fn, &wconfigs[wi]);
        }

        for (int wi = 0; wi < nw; wi++)
            pthread_join(wthreads[wi], NULL);

        code_client_stop(cclient);
        code_store_destroy(wstore);
        free(wconfigs);
        free(wthreads);
        return 0;
    } else if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        usage();
        return 0;
    } else if (cmd[0] == '-') {
        /* Flags without subcommand — treat as serve */
        return cmd_serve(argc, argv);
    } else {
        fprintf(stderr, "sjs: unknown command '%s'\n\n", cmd);
        usage();
        return 1;
    }
}
