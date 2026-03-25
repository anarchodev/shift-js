#define _GNU_SOURCE

#include "worker.h"
#include "kvstore.h"
#include "js_runtime.h"
#include "router.h"

#include <shift_h2.h>
#include <shift.h>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONNECTIONS 4096
#define MAX_STREAMS     (MAX_CONNECTIONS * 128)
#define BUF_COUNT       8192
#define BUF_SIZE        (64 * 1024)
#define RING_ENTRIES    8192
#define BACKLOG         4096

static void pin_to_core(int core) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

/* Extract a pseudo-header value from sh2 request headers */
static const char *find_header(const sh2_header_field_t *fields,
                               uint32_t count,
                               const char *name, uint32_t *out_len) {
    size_t nlen = strlen(name);
    for (uint32_t i = 0; i < count; i++) {
        if (fields[i].name_len == nlen &&
            !memcmp(fields[i].name, name, nlen)) {
            if (out_len) *out_len = fields[i].value_len;
            return fields[i].value;
        }
    }
    return NULL;
}

/* Copy a header value to a NUL-terminated string */
static char *header_to_str(const char *val, uint32_t len) {
    char *s = malloc(len + 1);
    if (s) { memcpy(s, val, len); s[len] = '\0'; }
    return s;
}

void *sjs_worker_fn(void *arg) {
    sjs_worker_config_t *wcfg = arg;

    pin_to_core(wcfg->worker_core);
    printf("shift-js worker %d: pinned to core %d\n",
           wcfg->worker_id, wcfg->worker_core);

    /* ---- KV store (per-worker connection) ---- */
    kvstore_t *kv = NULL;
    if (kv_open(wcfg->db_path, &kv) != 0) {
        fprintf(stderr, "Worker %d: kv_open failed\n", wcfg->worker_id);
        return NULL;
    }

    /* ---- QuickJS runtime (per-worker) ---- */
    sjs_runtime_t sjs = {0};
    if (sjs_runtime_init(&sjs, kv) != 0) {
        fprintf(stderr, "Worker %d: sjs_runtime_init failed\n", wcfg->worker_id);
        kv_close(kv);
        return NULL;
    }

    /* ---- Shift context ---- */
    shift_t *sh = NULL;
    shift_config_t sh_cfg = {
        .max_entities            = MAX_CONNECTIONS * 16 + MAX_STREAMS + 1024,
        .max_components          = 32,
        .max_collections         = 32,
        .deferred_queue_capacity = MAX_CONNECTIONS * 256,
    };
    if (shift_context_create(&sh_cfg, &sh) != shift_ok) {
        fprintf(stderr, "Worker %d: shift_context_create failed\n", wcfg->worker_id);
        sjs_runtime_free(&sjs);
        kv_close(kv);
        return NULL;
    }

    /* ---- Register sh2 components ---- */
    sh2_component_ids_t comp;
    if (sh2_register_components(sh, &comp) != sh2_ok) {
        fprintf(stderr, "Worker %d: sh2_register_components failed\n", wcfg->worker_id);
        shift_context_destroy(sh);
        sjs_runtime_free(&sjs);
        kv_close(kv);
        return NULL;
    }

    /* ---- Create user collections (all sh2 components) ---- */
    shift_component_id_t all_comps[] = {
        comp.stream_id, comp.session, comp.req_headers, comp.req_body,
        comp.resp_headers, comp.resp_body, comp.status, comp.io_result,
        comp.domain_tag,
    };
    size_t ncomps = sizeof(all_comps) / sizeof(all_comps[0]);

    shift_collection_id_t request_out, response_in, response_result_out;
    shift_collection_info_t ci_req  = { .name = "request_out",        .comp_ids = all_comps, .comp_count = ncomps };
    shift_collection_info_t ci_resp = { .name = "response_in",        .comp_ids = all_comps, .comp_count = ncomps };
    shift_collection_info_t ci_res  = { .name = "response_result_out", .comp_ids = all_comps, .comp_count = ncomps };
    if (shift_collection_register(sh, &ci_req,  &request_out) != shift_ok ||
        shift_collection_register(sh, &ci_resp, &response_in) != shift_ok ||
        shift_collection_register(sh, &ci_res,  &response_result_out) != shift_ok) {
        fprintf(stderr, "Worker %d: collection register failed\n", wcfg->worker_id);
        shift_context_destroy(sh);
        sjs_runtime_free(&sjs);
        kv_close(kv);
        return NULL;
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
        fprintf(stderr, "Worker %d: sh2_context_create failed: errno=%d (%s)\n",
                wcfg->worker_id, errno, strerror(errno));
        shift_context_destroy(sh);
        sjs_runtime_free(&sjs);
        kv_close(kv);
        return NULL;
    }

    if (sh2_listen(h2, wcfg->port, BACKLOG) != sh2_ok) {
        fprintf(stderr, "Worker %d: sh2_listen failed on port %d\n",
                wcfg->worker_id, wcfg->port);
        sh2_context_destroy(h2);
        shift_context_destroy(sh);
        sjs_runtime_free(&sjs);
        kv_close(kv);
        return NULL;
    }

    printf("shift-js worker %d: listening on port %d\n",
           wcfg->worker_id, wcfg->port);

    /* ---- Event loop ---- */
    while (*wcfg->running) {
        if (sh2_poll(h2, 0) != sh2_ok)
            break;

        /* ---- Process incoming requests ---- */
        {
            shift_entity_t *entities = NULL;
            size_t          count    = 0;
            shift_collection_get_entities(sh, request_out, &entities, &count);

            for (size_t i = 0; i < count; i++) {
                shift_entity_t e = entities[i];

                sh2_req_headers_t *rqh = NULL;
                sh2_req_body_t    *rqb = NULL;
                shift_entity_get_component(sh, e, comp.req_headers, (void **)&rqh);
                shift_entity_get_component(sh, e, comp.req_body, (void **)&rqb);

                /* Extract :method and :path pseudo-headers */
                uint32_t method_len = 0, path_len = 0;
                const char *method_val = find_header(rqh->fields, rqh->count,
                                                     ":method", &method_len);
                const char *path_val = find_header(rqh->fields, rqh->count,
                                                   ":path", &path_len);

                if (!method_val || !path_val) {
                    /* Malformed request — respond 400 */
                    sh2_status_t *st = NULL;
                    shift_entity_get_component(sh, e, comp.status, (void **)&st);
                    st->code = 400;

                    sh2_resp_body_t *rb = NULL;
                    shift_entity_get_component(sh, e, comp.resp_body, (void **)&rb);
                    rb->data = strdup("Bad Request");
                    rb->len  = 11;

                    sh2_resp_headers_t *rh = NULL;
                    shift_entity_get_component(sh, e, comp.resp_headers, (void **)&rh);
                    rh->fields = NULL;
                    rh->count  = 0;

                    shift_entity_move_one(sh, e, response_in);
                    continue;
                }

                char *method_str = header_to_str(method_val, method_len);
                char *path_str   = header_to_str(path_val, path_len);

                /* Build request context for JS dispatch */
                sjs_request_ctx_t req = {
                    .method       = method_str,
                    .path         = path_str,
                    .headers      = rqh->fields,
                    .header_count = rqh->count,
                    .body         = rqb->data,
                    .body_len     = rqb->len,
                };

                uint32_t body_len = 0;
                char *body = sjs_dispatch(&sjs, &req, &body_len);

                /* Build sh2 response */
                sh2_status_t *st = NULL;
                shift_entity_get_component(sh, e, comp.status, (void **)&st);
                st->code = req.resp_status;

                /* Response headers */
                uint32_t nhdr = req.resp_header_count;
                sh2_header_field_t *resp_fields = NULL;
                if (nhdr > 0) {
                    resp_fields = malloc(nhdr * sizeof(sh2_header_field_t));
                    for (uint32_t h = 0; h < nhdr; h++) {
                        resp_fields[h] = (sh2_header_field_t){
                            .name      = req.resp_header_names[h],
                            .name_len  = (uint32_t)strlen(req.resp_header_names[h]),
                            .value     = req.resp_header_values[h],
                            .value_len = (uint32_t)strlen(req.resp_header_values[h]),
                        };
                    }
                    /* Transfer ownership of name/value strings to sh2 */
                }

                sh2_resp_headers_t *rh = NULL;
                shift_entity_get_component(sh, e, comp.resp_headers, (void **)&rh);
                rh->fields = resp_fields;
                rh->count  = nhdr;

                sh2_resp_body_t *rb = NULL;
                shift_entity_get_component(sh, e, comp.resp_body, (void **)&rb);
                rb->data = body;
                rb->len  = body_len;

                shift_entity_move_one(sh, e, response_in);

                free(method_str);
                free(path_str);
                /* Don't free req.resp_header_names/values arrays — strings
                 * are now owned by resp_fields. Free the pointer arrays only. */
                free(req.resp_header_names);
                free(req.resp_header_values);
            }
        }

        /* ---- Drain completed responses ---- */
        {
            shift_entity_t *entities = NULL;
            size_t          count    = 0;
            shift_collection_get_entities(sh, response_result_out,
                                          &entities, &count);

            for (size_t i = 0; i < count; i++) {
                shift_entity_t e = entities[i];

                sh2_io_result_t *io = NULL;
                shift_entity_get_component(sh, e, comp.io_result, (void **)&io);
                if (io && io->error != 0) {
                    fprintf(stderr, "Worker %d: response send failed: %d\n",
                            wcfg->worker_id, io->error);
                }

                shift_entity_destroy_one(sh, e);
            }
        }

        shift_flush(sh);
    }

    /* ---- Shutdown ---- */
    sh2_context_destroy(h2);
    shift_flush(sh);

    /* Drain remaining entities */
    shift_collection_id_t drain[] = { request_out, response_in, response_result_out };
    for (int c = 0; c < 3; c++) {
        shift_entity_t *ents = NULL;
        size_t cnt = 0;
        shift_collection_get_entities(sh, drain[c], &ents, &cnt);
        for (size_t i = 0; i < cnt; i++)
            shift_entity_destroy_one(sh, ents[i]);
    }
    shift_flush(sh);

    shift_context_destroy(sh);
    sjs_runtime_free(&sjs);
    kv_close(kv);

    printf("shift-js worker %d: shutdown complete\n", wcfg->worker_id);
    return NULL;
}
