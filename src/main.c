#define _GNU_SOURCE

#include "worker.h"

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
            "  -h           Show this help\n",
            prog);
}

int main(int argc, char **argv) {
    const char *db_path = "shift-js.db";
    uint16_t    port    = 9000;
    int         nworkers = 0;

    int opt;
    while ((opt = getopt(argc, argv, "d:p:w:h")) != -1) {
        switch (opt) {
        case 'd': db_path  = optarg; break;
        case 'p': port     = (uint16_t)atoi(optarg); break;
        case 'w': nworkers = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (nworkers <= 0) nworkers = (int)ncpus;
    if (nworkers > (int)ncpus) nworkers = (int)ncpus;

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    printf("shift-js: %d workers, port %d, db %s\n", nworkers, port, db_path);

    sjs_worker_config_t *configs = calloc((size_t)nworkers, sizeof(*configs));
    pthread_t           *threads = calloc((size_t)nworkers, sizeof(*threads));

    for (int i = 0; i < nworkers; i++) {
        configs[i] = (sjs_worker_config_t){
            .worker_id   = i,
            .worker_core = i,
            .db_path     = db_path,
            .port        = port,
            .running     = &g_running,
        };
        pthread_create(&threads[i], NULL, sjs_worker_fn, &configs[i]);
    }

    for (int i = 0; i < nworkers; i++)
        pthread_join(threads[i], NULL);

    printf("shift-js: shutdown complete\n");

    free(threads);
    free(configs);
    return 0;
}
