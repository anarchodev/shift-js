#define _GNU_SOURCE

#include "log_store.h"
#include "http_api.h"

#include <shift.h>
#include <shift_h2.h>

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile bool g_running = true;

static void handle_signal(int sig) {
    (void)sig;
    g_running = false;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -d <path>          Log database path (default: logs.db)\n"
        "  -p <port>          Listen port (default: 9002)\n"
        "  --auth-secret <s>  HMAC secret for auth cookies (required)\n"
        "  -h                 Show this help\n",
        prog);
}

#define MAX_CONNECTIONS 256
#define RING_ENTRIES    1024
#define BUF_COUNT       1024
#define BUF_SIZE        (64 * 1024)
#define BACKLOG         128

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

int main(int argc, char **argv) {
    const char *db_path     = "logs.db";
    uint16_t    port        = 9002;
    const char *auth_secret = NULL;

    static struct option long_opts[] = {
        { "auth-secret", required_argument, NULL, 'S' },
        { NULL, 0, NULL, 0 },
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:p:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd': db_path     = optarg; break;
        case 'p': port        = (uint16_t)atoi(optarg); break;
        case 'S': auth_secret = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (!auth_secret) {
        fprintf(stderr, "error: --auth-secret is required\n");
        usage(argv[0]);
        return 1;
    }

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    /* Open log database */
    log_store_t ls;
    if (log_store_open(&ls, db_path) != 0) {
        fprintf(stderr, "error: failed to open database %s\n", db_path);
        return 1;
    }

    log_server_ctx_t sctx = {
        .ls              = &ls,
        .auth_secret     = auth_secret,
        .auth_secret_len = strlen(auth_secret),
    };

    /* Shift ECS context */
    shift_t *sh = NULL;
    shift_config_t sh_cfg = {
        .max_entities            = MAX_CONNECTIONS * 16 + 4096,
        .max_components          = 32,
        .max_collections         = 16,
        .deferred_queue_capacity = MAX_CONNECTIONS * 64,
    };
    if (shift_context_create(&sh_cfg, &sh) != shift_ok) {
        fprintf(stderr, "error: shift_context_create failed\n");
        log_store_close(&ls);
        return 1;
    }

    sh2_component_ids_t comp;
    if (sh2_register_components(sh, &comp) != sh2_ok) {
        fprintf(stderr, "error: sh2_register_components failed\n");
        shift_context_destroy(sh);
        log_store_close(&ls);
        return 1;
    }

    shift_component_id_t all_comps[] = {
        comp.stream_id, comp.session, comp.req_headers, comp.req_body,
        comp.resp_headers, comp.resp_body, comp.status, comp.io_result,
        comp.domain_tag,
    };
    size_t ncomps = sizeof(all_comps) / sizeof(all_comps[0]);

    shift_collection_id_t request_out, response_in, response_result_out;
    shift_collection_info_t ci_req  = { .name = "request_out",         .comp_ids = all_comps, .comp_count = ncomps };
    shift_collection_info_t ci_resp = { .name = "response_in",         .comp_ids = all_comps, .comp_count = ncomps };
    shift_collection_info_t ci_res  = { .name = "response_result_out", .comp_ids = all_comps, .comp_count = ncomps };
    if (shift_collection_register(sh, &ci_req,  &request_out) != shift_ok ||
        shift_collection_register(sh, &ci_resp, &response_in) != shift_ok ||
        shift_collection_register(sh, &ci_res,  &response_result_out) != shift_ok) {
        fprintf(stderr, "error: collection register failed\n");
        shift_context_destroy(sh);
        log_store_close(&ls);
        return 1;
    }

    sh2_context_t *h2 = NULL;
    sh2_config_t h2cfg = {
        .shift               = sh,
        .comp_ids            = comp,
        .max_connections     = MAX_CONNECTIONS,
        .ring_entries        = RING_ENTRIES,
        .buf_count           = BUF_COUNT,
        .buf_size            = BUF_SIZE,
        .request_out         = request_out,
        .response_in         = response_in,
        .response_result_out = response_result_out,
    };
    if (sh2_context_create(&h2cfg, &h2) != sh2_ok) {
        fprintf(stderr, "error: sh2_context_create failed\n");
        shift_context_destroy(sh);
        log_store_close(&ls);
        return 1;
    }

    if (sh2_listen(h2, port, BACKLOG) != sh2_ok) {
        fprintf(stderr, "error: failed to listen on port %d\n", port);
        sh2_context_destroy(h2);
        shift_context_destroy(sh);
        log_store_close(&ls);
        return 1;
    }

    printf("shift-log: listening on port %d, db %s\n", port, db_path);

    uint64_t last_checkpoint = now_ms();

    /* Event loop */
    while (g_running) {
        if (sh2_poll(h2, 0) != sh2_ok)
            break;

        shift_entity_t *entities = NULL;
        size_t count = 0;
        shift_collection_get_entities(sh, request_out, &entities, &count);

        for (size_t i = 0; i < count; i++) {
            log_server_handle_request(&sctx, sh, entities[i],
                                       &comp, response_in);
        }

        /* Periodic WAL checkpoint */
        uint64_t now = now_ms();
        if (now - last_checkpoint > 500) {
            log_store_checkpoint(&ls);
            last_checkpoint = now;
        }
    }

    printf("shift-log: shutting down\n");

    sh2_context_destroy(h2);
    shift_context_destroy(sh);
    log_store_close(&ls);

    return 0;
}
