#include "code_server.h"
#include "code_db.h"
#include "code_store.h"
#include "preprocessor.h"
#include "ejs.h"
#include "typescript.h"

#include <quickjs.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ======================================================================
 * Static file detection (mirrors sjsctl logic)
 * ====================================================================== */

static const char *STATIC_EXTS[] = {
    ".js", ".css", ".html", ".json", ".svg", ".png", ".jpg", ".jpeg",
    ".gif", ".ico", ".woff", ".woff2", ".ttf", ".map", ".xml", ".txt",
    ".wasm",
};
static const size_t STATIC_EXT_COUNT =
    sizeof(STATIC_EXTS) / sizeof(STATIC_EXTS[0]);

static int is_static_file(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot) return 0;
    for (size_t i = 0; i < STATIC_EXT_COUNT; i++) {
        if (strcasecmp(dot, STATIC_EXTS[i]) == 0)
            return 1;
    }
    return 0;
}

/* ======================================================================
 * Deploy-time compilation (mirrors sjsctl deploy logic)
 * ====================================================================== */

typedef struct {
    code_db_t                         *cdb;
    const char                        *database_id;
    const char                        *tree_hash;
    const sjs_preprocessor_registry_t *preprocessors;
    JSRuntime                         *compile_rt;
    JSContext                         *compile_ctx;
    sjs_ts_ctx_t                       ts_ctx;
} deploy_ctx_t;

static JSModuleDef *deploy_module_loader(JSContext *ctx,
                                         const char *module_name,
                                         void *opaque) {
    deploy_ctx_t *dc = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    if (!dc) return NULL;

    void *source = NULL;
    size_t source_len = 0;
    const char *resolved_name = module_name;
    char *resolved_alloc = NULL;

    const char *ext = sjs_path_extension(module_name);
    if (ext) {
        if (code_db_tree_get(dc->cdb, dc->database_id, dc->tree_hash,
                             module_name, &source, &source_len) != 0)
            return NULL;
    } else {
        static const char *try_exts[] = { ".mjs", ".ejs", ".ts", ".tsx" };
        for (int i = 0; i < 4; i++) {
            char try_name[512];
            snprintf(try_name, sizeof(try_name), "%s%s", module_name, try_exts[i]);
            if (code_db_tree_get(dc->cdb, dc->database_id, dc->tree_hash,
                                 try_name, &source, &source_len) == 0) {
                resolved_alloc = strdup(try_name);
                resolved_name = resolved_alloc;
                ext = sjs_path_extension(resolved_name);
                break;
            }
        }
        if (!source) return NULL;
    }

    /* Null-terminate for JS_Eval */
    source = realloc(source, source_len + 1);
    ((char *)source)[source_len] = '\0';

    char  *compile_source = source;
    size_t compile_len    = source_len;

    const sjs_preprocessor_entry_t *pp = ext
        ? sjs_preprocessor_find(dc->preprocessors, ext) : NULL;

    if (pp) {
        if (pp->transform == sjs_typescript_transform) {
            sjs_ts_binding_t *binding = pp->user_data;
            if (binding && binding->ts_ctx)
                binding->ts_ctx->current_module_path = resolved_name;
        }
        size_t js_len;
        char *js = pp->transform(source, source_len, &js_len, pp->user_data);
        free(source);
        if (!js) { free(resolved_alloc); return NULL; }
        compile_source = js;
        compile_len = js_len;
    }

    JSValue func = JS_Eval(ctx, compile_source, compile_len, resolved_name,
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(compile_source);
    free(resolved_alloc);

    if (JS_IsException(func)) return NULL;

    JSModuleDef *mod = JS_VALUE_GET_PTR(func);
    JS_FreeValue(ctx, func);
    return mod;
}

static int deploy_init(deploy_ctx_t *dc, code_db_t *cdb,
                       const char *database_id, const char *tree_hash,
                       const sjs_preprocessor_registry_t *pp) {
    memset(dc, 0, sizeof(*dc));
    dc->cdb = cdb;
    dc->database_id = database_id;
    dc->tree_hash = tree_hash;
    dc->preprocessors = pp;

    dc->compile_rt = JS_NewRuntime();
    if (!dc->compile_rt) return -1;

    JS_SetRuntimeOpaque(dc->compile_rt, dc);
    JS_SetModuleLoaderFunc(dc->compile_rt, NULL, deploy_module_loader, NULL);

    dc->compile_ctx = JS_NewContext(dc->compile_rt);
    if (!dc->compile_ctx) {
        JS_FreeRuntime(dc->compile_rt);
        return -1;
    }

    if (sjs_typescript_init(dc->compile_ctx, &dc->ts_ctx) != 0) {
        fprintf(stderr, "code_server: failed to initialize Sucrase\n");
        JS_FreeContext(dc->compile_ctx);
        JS_FreeRuntime(dc->compile_rt);
        return -1;
    }

    return 0;
}

static void deploy_free(deploy_ctx_t *dc) {
    sjs_typescript_free(&dc->ts_ctx);
    if (dc->compile_ctx) JS_FreeContext(dc->compile_ctx);
    if (dc->compile_rt) JS_FreeRuntime(dc->compile_rt);
}

static void *deploy_compile(deploy_ctx_t *dc, const char *path,
                            const void *source, size_t source_len,
                            size_t *out_bc_len) {
    const char *ext = sjs_path_extension(path);
    const sjs_preprocessor_entry_t *pp = ext
        ? sjs_preprocessor_find(dc->preprocessors, ext) : NULL;

    char  *compile_source = (char *)source;
    size_t compile_len    = source_len;
    char  *to_free        = NULL;

    if (pp) {
        if (pp->transform == sjs_typescript_transform) {
            sjs_ts_binding_t *binding = pp->user_data;
            if (binding && binding->ts_ctx)
                binding->ts_ctx->current_module_path = path;
        }
        size_t js_len;
        char *js = pp->transform(source, source_len, &js_len, pp->user_data);
        if (!js) {
            fprintf(stderr, "code_server: preprocess failed: %s\n", path);
            return NULL;
        }
        compile_source = js;
        compile_len = js_len;
        to_free = js;
    }

    JSContext *cc = dc->compile_ctx;
    JSValue module_val = JS_Eval(cc, compile_source, compile_len, path,
                                 JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(to_free);

    if (JS_IsException(module_val)) {
        JSValue exc = JS_GetException(cc);
        const char *err = JS_ToCString(cc, exc);
        fprintf(stderr, "code_server: compile error in %s: %s\n",
                path, err ? err : "(unknown)");
        JS_FreeCString(cc, err);
        JS_FreeValue(cc, exc);
        return NULL;
    }

    size_t bc_len;
    uint8_t *bc = JS_WriteObject(cc, &bc_len, module_val,
                                 JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(cc, module_val);

    if (!bc) {
        fprintf(stderr, "code_server: bytecode write failed: %s\n", path);
        return NULL;
    }

    *out_bc_len = bc_len;
    return bc;
}

/* ======================================================================
 * Public: deploy (snapshot + compile + populate code_store)
 * ====================================================================== */

int code_server_deploy(code_db_t *cdb, const char *database_id,
                       code_store_t *store) {
    char *tree_hash = code_db_snapshot(cdb, database_id);
    if (!tree_hash) {
        fprintf(stderr, "code_server: no files in working tree\n");
        return -1;
    }

    printf("code_server: tree hash %s\n", tree_hash);

    /* Set up preprocessor registry */
    sjs_preprocessor_registry_t pp;
    sjs_preprocessor_init(&pp);
    sjs_preprocessor_register(&pp, ".ejs", sjs_ejs_transform, NULL);
    sjs_preprocessor_register(&pp, ".ts",  sjs_typescript_transform, NULL);
    sjs_preprocessor_register(&pp, ".tsx", sjs_typescript_transform, NULL);

    deploy_ctx_t dc;
    if (deploy_init(&dc, cdb, database_id, tree_hash, &pp) != 0) {
        free(tree_hash);
        return -1;
    }

    /* Wire TypeScript bindings */
    for (size_t i = 0; i < pp.count; i++) {
        sjs_preprocessor_entry_t *e = &pp.entries[i];
        if (e->transform != sjs_typescript_transform) continue;
        if (strcmp(e->extension, ".tsx") == 0)
            e->user_data = &dc.ts_ctx.tsx_binding;
        else
            e->user_data = &dc.ts_ctx.ts_binding;
    }

    /* List all files in the snapshot */
    code_tree_t tree;
    if (code_db_tree_list(cdb, database_id, tree_hash, &tree) != 0) {
        fprintf(stderr, "code_server: failed to list tree\n");
        deploy_free(&dc);
        free(tree_hash);
        return -1;
    }

    /* Build new code_store contents */
    code_store_update_begin(store);
    code_store_update_set_hash(store, tree_hash);

    int code_count = 0, file_count = 0, errors = 0;

    for (size_t i = 0; i < tree.count; i++) {
        const char *path = tree.entries[i].path;
        const char *sha1 = tree.entries[i].sha1;

        void *content = NULL;
        size_t content_len = 0;
        if (code_db_get_file(cdb, sha1, &content, &content_len) != 0 ||
            !content) {
            fprintf(stderr, "code_server: missing blob %s for %s\n", sha1, path);
            errors++;
            continue;
        }

        /* Null-terminate for JS_Eval (requires it despite length param) */
        content = realloc(content, content_len + 1);
        ((char *)content)[content_len] = '\0';

        if (is_static_file(path)) {
            code_store_update_put_static(store, path, content, content_len);
            file_count++;
        } else {
            size_t bc_len = 0;
            void *bc = deploy_compile(&dc, path, content, content_len, &bc_len);
            if (!bc) {
                errors++;
                free(content);
                continue;
            }

            code_store_update_put(store, path, bc, bc_len, sha1);
            js_free(dc.compile_ctx, bc);
            code_count++;
        }

        free(content);
    }

    code_store_update_end(store);
    code_db_tree_free(&tree);
    deploy_free(&dc);

    if (errors > 0) {
        fprintf(stderr, "code_server: deployed with %d errors\n", errors);
    }

    printf("code_server: deployed %d code, %d static files\n",
           code_count, file_count);
    free(tree_hash);
    return errors > 0 ? -1 : 0;
}

/* ======================================================================
 * Code server HTTP loop (sjs code subcommand)
 * ====================================================================== */

#include <shift_h2.h>
#include <shift.h>

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>

#define CS_MAX_CONNECTIONS 64
#define CS_RING_ENTRIES    256
#define CS_BUF_COUNT       256
#define CS_BUF_SIZE        (64 * 1024)
#define CS_BACKLOG         128

static volatile bool cs_running = true;
static void cs_signal(int sig) { (void)sig; cs_running = false; }

static const char *cs_find_header(const sh2_header_field_t *fields,
                                  uint32_t count, const char *name,
                                  uint32_t *out_len) {
    size_t nlen = strlen(name);
    for (uint32_t i = 0; i < count; i++) {
        if (fields[i].name_len == nlen &&
            memcmp(fields[i].name, name, nlen) == 0) {
            if (out_len) *out_len = fields[i].value_len;
            return fields[i].value;
        }
    }
    return NULL;
}

static char *cs_query_param(const char *path, const char *name) {
    const char *qs = strchr(path, '?');
    if (!qs) return NULL;
    qs++;
    size_t nlen = strlen(name);
    const char *p = qs;
    while (*p) {
        if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            const char *val = p + nlen + 1;
            const char *end = strchr(val, '&');
            size_t vlen = end ? (size_t)(end - val) : strlen(val);
            char *out = malloc(vlen + 1);
            if (!out) return NULL;
            memcpy(out, val, vlen);
            out[vlen] = '\0';
            return out;
        }
        const char *amp = strchr(p, '&');
        if (!amp) break;
        p = amp + 1;
    }
    return NULL;
}

static void cs_respond(shift_t *sh, shift_entity_t e,
                       sh2_component_ids_t *comp,
                       shift_collection_id_t response_in,
                       uint16_t status, const char *content_type,
                       void *body, uint32_t body_len) {
    sh2_status_t *st = NULL;
    shift_entity_get_component(sh, e, comp->status, (void **)&st);
    st->code = status;

    sh2_resp_body_t *rb = NULL;
    shift_entity_get_component(sh, e, comp->resp_body, (void **)&rb);
    rb->data = body;
    rb->len  = body_len;

    sh2_resp_headers_t *rh = NULL;
    shift_entity_get_component(sh, e, comp->resp_headers, (void **)&rh);
    if (content_type) {
        sh2_header_field_t *hf = malloc(sizeof(sh2_header_field_t));
        if (hf) {
            hf[0] = (sh2_header_field_t){
                .name      = strdup("content-type"),
                .name_len  = 12,
                .value     = strdup(content_type),
                .value_len = (uint32_t)strlen(content_type),
            };
            rh->fields = hf;
            rh->count  = 1;
        }
    } else {
        rh->fields = NULL;
        rh->count  = 0;
    }

    shift_entity_move_one(sh, e, response_in);
}

static void cs_respond_json(shift_t *sh, shift_entity_t e,
                            sh2_component_ids_t *comp,
                            shift_collection_id_t response_in,
                            uint16_t status, const char *json) {
    cs_respond(sh, e, comp, response_in, status,
               "application/json; charset=utf-8",
               strdup(json), (uint32_t)strlen(json));
}

int code_server_run(const code_server_config_t *cfg) {
    signal(SIGINT,  cs_signal);
    signal(SIGTERM, cs_signal);

    code_db_t *cdb = NULL;
    if (code_db_open(cfg->db_path, &cdb) != 0) {
        fprintf(stderr, "code_server: failed to open %s\n", cfg->db_path);
        return 1;
    }

    code_store_t *store = cfg->store ? cfg->store : code_store_create();

    /* shift + sh2 setup */
    shift_t *sh = NULL;
    shift_config_t sh_cfg = {
        .max_entities            = 4096,
        .max_components          = 32,
        .max_collections         = 64,
        .deferred_queue_capacity = 4096,
    };
    if (shift_context_create(&sh_cfg, &sh) != shift_ok) {
        fprintf(stderr, "code_server: shift_context_create failed\n");
        code_db_close(cdb);
        return 1;
    }

    sh2_component_ids_t comp;
    if (sh2_register_components(sh, &comp) != sh2_ok) {
        fprintf(stderr, "code_server: sh2_register_components failed\n");
        shift_context_destroy(sh);
        code_db_close(cdb);
        return 1;
    }

    shift_component_id_t all[] = {
        comp.stream_id, comp.session, comp.req_headers, comp.req_body,
        comp.resp_headers, comp.resp_body, comp.status, comp.io_result,
        comp.domain_tag, comp.peer_cert,
    };
    uint32_t nall = sizeof(all) / sizeof(all[0]);

    shift_collection_id_t request_out, response_in, response_out;
    shift_collection_info_t ci;
    ci = (shift_collection_info_t){ .name = "request_out", .comp_ids = all, .comp_count = nall };
    shift_collection_register(sh, &ci, &request_out);
    ci = (shift_collection_info_t){ .name = "response_in", .comp_ids = all, .comp_count = nall };
    shift_collection_register(sh, &ci, &response_in);
    ci = (shift_collection_info_t){ .name = "response_out", .comp_ids = all, .comp_count = nall };
    shift_collection_register(sh, &ci, &response_out);

    sh2_context_t *h2 = NULL;
    sh2_config_t h2cfg = {
        .shift               = sh,
        .comp_ids            = comp,
        .max_connections     = CS_MAX_CONNECTIONS,
        .ring_entries        = CS_RING_ENTRIES,
        .buf_count           = CS_BUF_COUNT,
        .buf_size            = CS_BUF_SIZE,
        .request_out         = request_out,
        .response_in         = response_in,
        .response_out        = response_out,
    };
    if (sh2_context_create(&h2cfg, &h2) != sh2_ok) {
        fprintf(stderr, "code_server: sh2_context_create failed: errno=%d (%s)\n",
                errno, strerror(errno));
        shift_context_destroy(sh);
        code_db_close(cdb);
        return 1;
    }

    if (sh2_listen(h2, cfg->port, CS_BACKLOG) != sh2_ok) {
        fprintf(stderr, "code_server: sh2_listen failed on port %d\n", cfg->port);
        sh2_context_destroy(h2);
        shift_context_destroy(sh);
        code_db_close(cdb);
        return 1;
    }

    const char *database_id = cfg->database_id ? cfg->database_id : "default";
    printf("code_server: listening on port %d, db %s\n", cfg->port, cfg->db_path);

    /* Event loop */
    while (cs_running) {
        if (sh2_poll(h2, 0) != sh2_ok)
            break;

        /* Process requests */
        shift_entity_t *entities = NULL;
        size_t count = 0;
        shift_collection_get_entities(sh, request_out, &entities, &count);

        for (size_t i = 0; i < count; i++) {
            shift_entity_t e = entities[i];

            sh2_req_headers_t *rqh = NULL;
            sh2_req_body_t    *rqb = NULL;
            shift_entity_get_component(sh, e, comp.req_headers, (void **)&rqh);
            shift_entity_get_component(sh, e, comp.req_body, (void **)&rqb);

            uint32_t method_len = 0, path_len = 0;
            const char *method = cs_find_header(rqh->fields, rqh->count,
                                                ":method", &method_len);
            const char *path_raw = cs_find_header(rqh->fields, rqh->count,
                                                  ":path", &path_len);

            if (!method || !path_raw) {
                cs_respond(sh, e, &comp, response_in, 400, NULL,
                           strdup("Bad Request"), 11);
                continue;
            }

            /* Null-terminate path for string ops */
            char path[2048];
            size_t plen = path_len < sizeof(path) - 1 ? path_len : sizeof(path) - 1;
            memcpy(path, path_raw, plen);
            path[plen] = '\0';

            /* PUT /upload?path=<path> — store file in code_db */
            if (strncmp(path, "/upload", 7) == 0 &&
                (path[7] == '?' || path[7] == '\0') &&
                method_len == 3 && memcmp(method, "PUT", 3) == 0) {

                char *fpath = cs_query_param(path, "path");
                if (!fpath || !rqb || !rqb->data || rqb->len == 0) {
                    cs_respond_json(sh, e, &comp, response_in, 400,
                                    "{\"error\":\"missing path or body\"}");
                    free(fpath);
                    continue;
                }

                int rc = code_db_put_file(cdb, database_id,
                                          fpath, rqb->data, rqb->len);
                free(fpath);
                if (rc != 0) {
                    cs_respond_json(sh, e, &comp, response_in, 500,
                                    "{\"error\":\"upload failed\"}");
                } else {
                    cs_respond_json(sh, e, &comp, response_in, 200,
                                    "{\"ok\":true}");
                }

            /* POST /deploy — snapshot + compile + update code_store */
            } else if (strncmp(path, "/deploy", 7) == 0 &&
                       (path[7] == '?' || path[7] == '\0') &&
                       method_len == 4 && memcmp(method, "POST", 4) == 0) {

                int rc = code_server_deploy(cdb, database_id, store);
                if (rc != 0) {
                    cs_respond_json(sh, e, &comp, response_in, 500,
                                    "{\"error\":\"deploy failed\"}");
                } else {
                    cs_respond_json(sh, e, &comp, response_in, 200,
                                    "{\"ok\":true}");
                }

            /* GET /blob/<sha1> — fetch a content-addressed blob */
            } else if (strncmp(path, "/blob/", 6) == 0 &&
                       method_len == 3 && memcmp(method, "GET", 3) == 0) {

                const char *sha1 = path + 6;
                void *blob = NULL;
                size_t blob_len = 0;
                if (code_db_get_file(cdb, sha1, &blob, &blob_len) == 0) {
                    cs_respond(sh, e, &comp, response_in, 200,
                               "application/octet-stream",
                               blob, (uint32_t)blob_len);
                } else {
                    cs_respond_json(sh, e, &comp, response_in, 404,
                                    "{\"error\":\"not found\"}");
                }

            /* GET /bytecode/<path> — fetch compiled bytecode from code_store */
            } else if (strncmp(path, "/bytecode/", 10) == 0 &&
                       method_len == 3 && memcmp(method, "GET", 3) == 0) {

                const char *mod_path = path + 10;
                code_entry_t entry;
                if (code_store_get(store, mod_path, &entry) == 0) {
                    void *copy = malloc(entry.bytecode_len);
                    memcpy(copy, entry.bytecode, entry.bytecode_len);
                    cs_respond(sh, e, &comp, response_in, 200,
                               "application/octet-stream",
                               copy, (uint32_t)entry.bytecode_len);
                } else {
                    cs_respond_json(sh, e, &comp, response_in, 404,
                                    "{\"error\":\"not found\"}");
                }

            /* GET /snapshot — tree hash + all compiled bytecode entries */
            } else if (strncmp(path, "/snapshot", 9) == 0 &&
                       (path[9] == '?' || path[9] == '\0') &&
                       method_len == 3 && memcmp(method, "GET", 3) == 0) {

                /* Return current tree hash. Workers use this to pull
                 * bytecode via /blob/ calls. Full snapshot serialization
                 * will be added when the pull thread is implemented. */
                const char *hash = code_store_tree_hash(store);
                char json[256];
                snprintf(json, sizeof(json),
                         "{\"tree_hash\":\"%s\"}", hash ? hash : "");
                cs_respond_json(sh, e, &comp, response_in, 200, json);

            /* GET /tree — list tree entries */
            } else if (strncmp(path, "/tree", 5) == 0 &&
                       (path[5] == '?' || path[5] == '\0') &&
                       method_len == 3 && memcmp(method, "GET", 3) == 0) {

                const char *hash = code_store_tree_hash(store);
                if (!hash || !hash[0]) {
                    cs_respond_json(sh, e, &comp, response_in, 200, "[]");
                    continue;
                }

                code_tree_t tree;
                if (code_db_tree_list(cdb, database_id, hash, &tree) != 0) {
                    cs_respond_json(sh, e, &comp, response_in, 500,
                                    "{\"error\":\"tree list failed\"}");
                    continue;
                }

                size_t cap = 256 + tree.count * 128;
                char *buf = malloc(cap);
                size_t pos = 0;
                pos += (size_t)snprintf(buf + pos, cap - pos, "[");
                for (size_t t = 0; t < tree.count; t++) {
                    if (pos + 128 + strlen(tree.entries[t].path) > cap) {
                        cap *= 2;
                        buf = realloc(buf, cap);
                    }
                    pos += (size_t)snprintf(buf + pos, cap - pos,
                            "%s{\"path\":\"%s\",\"sha1\":\"%s\"}",
                            t > 0 ? "," : "",
                            tree.entries[t].path, tree.entries[t].sha1);
                }
                pos += (size_t)snprintf(buf + pos, cap - pos, "]");
                code_db_tree_free(&tree);

                cs_respond(sh, e, &comp, response_in, 200,
                           "application/json; charset=utf-8",
                           buf, (uint32_t)pos);

            /* DELETE /file?path=<path> — remove from working tree */
            } else if (strncmp(path, "/file", 5) == 0 &&
                       method_len == 6 && memcmp(method, "DELETE", 6) == 0) {

                char *fpath = cs_query_param(path, "path");
                if (!fpath) {
                    cs_respond_json(sh, e, &comp, response_in, 400,
                                    "{\"error\":\"missing path\"}");
                    continue;
                }
                code_db_tree_delete(cdb, database_id, fpath);
                free(fpath);
                cs_respond_json(sh, e, &comp, response_in, 200,
                                "{\"ok\":true}");

            } else {
                cs_respond_json(sh, e, &comp, response_in, 404,
                                "{\"error\":\"not found\"}");
            }
        }

        /* Drain completed responses — entity destruction triggers
         * component destructors which free the body/headers. */
        {
            shift_entity_t *done = NULL;
            size_t done_count = 0;
            shift_collection_get_entities(sh, response_out,
                                          &done, &done_count);
            for (size_t i = 0; i < done_count; i++)
                shift_entity_destroy_one(sh, done[i]);
        }

        shift_flush(sh);
    }

    printf("code_server: shutting down\n");
    sh2_context_destroy(h2);
    shift_flush(sh);
    shift_context_destroy(sh);
    code_db_close(cdb);
    if (!cfg->store) code_store_destroy(store);
    return 0;
}
