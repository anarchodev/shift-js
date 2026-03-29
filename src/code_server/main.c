#define _GNU_SOURCE

#include "code_db.h"
#include "compile.h"
#include "http_api.h"

#include <shift.h>
#include <shift_h2.h>

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile bool g_running = true;

static void handle_signal(int sig) {
    (void)sig;
    g_running = false;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -d <path>          Code database path (default: code.db)\n"
        "  -p <port>          Listen port (default: 9001)\n"
        "  --auth-secret <s>  HMAC secret for auth cookies (required)\n"
        "  -h                 Show this help\n",
        prog);
}

#define MAX_CONNECTIONS 256
#define RING_ENTRIES    1024
#define BUF_COUNT       1024
#define BUF_SIZE        (64 * 1024)
#define BACKLOG         128

int main(int argc, char **argv) {
    const char *db_path     = "code.db";
    uint16_t    port        = 9001;
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

    /* ---- Open code database ---- */
    code_db_t db;
    if (code_db_open(&db, db_path) != 0) {
        fprintf(stderr, "error: failed to open database %s\n", db_path);
        return 1;
    }

    /* ---- Initialize compilation context ---- */
    compile_ctx_t cc;
    if (compile_ctx_init(&cc) != 0) {
        fprintf(stderr, "error: failed to initialize compilation context\n");
        code_db_close(&db);
        return 1;
    }

    /* ---- Server context ---- */
    code_server_ctx_t sctx = {
        .db              = &db,
        .cc              = &cc,
        .auth_secret     = auth_secret,
        .auth_secret_len = strlen(auth_secret),
    };

    /* ---- Shift ECS context ---- */
    shift_t *sh = NULL;
    shift_config_t sh_cfg = {
        .max_entities            = MAX_CONNECTIONS * 16 + 4096,
        .max_components          = 32,
        .max_collections         = 16,
        .deferred_queue_capacity = MAX_CONNECTIONS * 64,
    };
    if (shift_context_create(&sh_cfg, &sh) != shift_ok) {
        fprintf(stderr, "error: shift_context_create failed\n");
        compile_ctx_free(&cc);
        code_db_close(&db);
        return 1;
    }

    /* ---- Register sh2 components ---- */
    sh2_component_ids_t comp;
    if (sh2_register_components(sh, &comp) != sh2_ok) {
        fprintf(stderr, "error: sh2_register_components failed\n");
        shift_context_destroy(sh);
        compile_ctx_free(&cc);
        code_db_close(&db);
        return 1;
    }

    /* ---- Collections ---- */
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
        compile_ctx_free(&cc);
        code_db_close(&db);
        return 1;
    }

    /* ---- Create sh2 context ---- */
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
        compile_ctx_free(&cc);
        code_db_close(&db);
        return 1;
    }

    if (sh2_listen(h2, port, BACKLOG) != sh2_ok) {
        fprintf(stderr, "error: failed to listen on port %d\n", port);
        sh2_context_destroy(h2);
        shift_context_destroy(sh);
        compile_ctx_free(&cc);
        code_db_close(&db);
        return 1;
    }

    printf("shift-code: listening on port %d, db %s\n", port, db_path);

    /* ---- Event loop ---- */
    while (g_running) {
        if (sh2_poll(h2, 0) != sh2_ok)
            break;

        shift_entity_t *entities = NULL;
        size_t count = 0;
        shift_collection_get_entities(sh, request_out, &entities, &count);

        for (size_t i = 0; i < count; i++) {
            code_server_handle_request(&sctx, sh, entities[i],
                                        &comp, response_in);
        }
    }

    printf("shift-code: shutting down\n");

    sh2_context_destroy(h2);
    shift_context_destroy(sh);
    compile_ctx_free(&cc);
    code_db_close(&db);

    return 0;
}
