#define _GNU_SOURCE

#include <shift_h2.h>
#include <shift.h>

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================
 * Shift + sh2 client setup
 * ====================================================================== */

typedef struct {
    shift_t             *sh;
    sh2_context_t       *ctx;
    sh2_component_ids_t  comp;

    shift_collection_id_t request_out;
    shift_collection_id_t response_in;
    shift_collection_id_t response_result_out;

    shift_collection_id_t connect_out;
    shift_collection_id_t connect_result_out;
    shift_collection_id_t client_request_in;
    shift_collection_id_t client_response_out;
    shift_collection_id_t client_result_out;

    shift_entity_t session_entity;
    bool           connected;
} ctl_client_t;

static int client_init(ctl_client_t *c) {
    memset(c, 0, sizeof(*c));

    shift_config_t sh_cfg = {
        .max_entities            = 4096,
        .max_components          = 32,
        .max_collections         = 32,
        .deferred_queue_capacity = 4096,
    };
    if (shift_context_create(&sh_cfg, &c->sh) != shift_ok)
        return -1;

    if (sh2_register_components(c->sh, &c->comp) != sh2_ok) {
        shift_context_destroy(c->sh);
        return -1;
    }

    /* Server-path collections (required even in client-only mode) */
    shift_component_id_t all[] = {
        c->comp.stream_id, c->comp.session, c->comp.req_headers,
        c->comp.req_body, c->comp.resp_headers, c->comp.resp_body,
        c->comp.status, c->comp.io_result, c->comp.domain_tag,
        c->comp.peer_cert,
    };
    uint32_t nall = sizeof(all) / sizeof(all[0]);

    shift_collection_info_t ci;

    ci = (shift_collection_info_t){ .name = "request_out", .comp_ids = all, .comp_count = nall };
    shift_collection_register(c->sh, &ci, &c->request_out);
    ci = (shift_collection_info_t){ .name = "response_in", .comp_ids = all, .comp_count = nall };
    shift_collection_register(c->sh, &ci, &c->response_in);
    ci = (shift_collection_info_t){ .name = "response_result_out", .comp_ids = all, .comp_count = nall };
    shift_collection_register(c->sh, &ci, &c->response_result_out);

    /* Client-path collections */
    shift_component_id_t conn[] = {
        c->comp.connect_target, c->comp.session, c->comp.io_result,
    };

    ci = (shift_collection_info_t){ .name = "connect_out", .comp_ids = conn, .comp_count = 3 };
    shift_collection_register(c->sh, &ci, &c->connect_out);
    ci = (shift_collection_info_t){ .name = "connect_result_out", .comp_ids = all, .comp_count = nall };
    shift_collection_register(c->sh, &ci, &c->connect_result_out);
    ci = (shift_collection_info_t){ .name = "client_request_in", .comp_ids = all, .comp_count = nall };
    shift_collection_register(c->sh, &ci, &c->client_request_in);
    ci = (shift_collection_info_t){ .name = "client_response_out", .comp_ids = all, .comp_count = nall };
    shift_collection_register(c->sh, &ci, &c->client_response_out);
    ci = (shift_collection_info_t){ .name = "client_result_out", .comp_ids = all, .comp_count = nall };
    shift_collection_register(c->sh, &ci, &c->client_result_out);

    sh2_config_t cfg = {
        .shift               = c->sh,
        .comp_ids            = c->comp,
        .max_connections     = 4,
        .ring_entries        = 64,
        .buf_count           = 64,
        .buf_size            = 64 * 1024,
        .request_out         = c->request_out,
        .response_in         = c->response_in,
        .response_result_out = c->response_result_out,
        .enable_connect      = true,
        .client_colls = {
            .connect_out         = c->connect_out,
            .connect_result_out  = c->connect_result_out,
            .request_in          = c->client_request_in,
            .response_out        = c->client_response_out,
            .response_result_out = c->client_result_out,
        },
    };

    if (sh2_context_create(&cfg, &c->ctx) != sh2_ok) {
        shift_context_destroy(c->sh);
        return -1;
    }

    return 0;
}

static int client_connect(ctl_client_t *c, const char *host, uint16_t port) {
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    inet_pton(AF_INET, host, &addr.sin_addr);

    shift_entity_t ce;
    shift_entity_create_one_begin(c->sh, c->connect_out, &ce);

    sh2_connect_target_t *tgt = NULL;
    shift_entity_get_component(c->sh, ce, c->comp.connect_target, (void **)&tgt);
    tgt->addr         = addr;
    tgt->hostname     = host;
    tgt->hostname_len = (uint32_t)strlen(host);

    shift_entity_create_one_end(c->sh, ce);

    /* Poll until connected */
    while (!c->connected) {
        if (sh2_poll(c->ctx, 0) != sh2_ok)
            return -1;

        shift_entity_t *entities = NULL;
        size_t count = 0;
        shift_collection_get_entities(c->sh, c->connect_result_out,
                                      &entities, &count);
        for (size_t i = 0; i < count; i++) {
            sh2_io_result_t *io = NULL;
            shift_entity_get_component(c->sh, entities[i], c->comp.io_result,
                                       (void **)&io);
            if (io && io->error == 0) {
                sh2_session_t *sess = NULL;
                shift_entity_get_component(c->sh, entities[i], c->comp.session,
                                           (void **)&sess);
                c->session_entity = sess->entity;
                c->connected = true;
            } else {
                fprintf(stderr, "sjs ctl: connect failed\n");
                shift_entity_destroy_one(c->sh, entities[i]);
                return -1;
            }
            shift_entity_destroy_one(c->sh, entities[i]);
        }
        shift_flush(c->sh);
    }
    return 0;
}

typedef struct {
    uint16_t  status;
    char     *body;
    size_t    body_len;
} ctl_response_t;

static int client_request(ctl_client_t *c, const char *host,
                          const char *method, const char *path,
                          const void *body, size_t body_len,
                          const char *token,
                          ctl_response_t *resp) {
    memset(resp, 0, sizeof(*resp));

    /* Build headers */
    uint32_t nhdr = 4;
    if (token) nhdr++;
    if (body && body_len > 0) nhdr++;

    sh2_header_field_t *fields = calloc(nhdr, sizeof(sh2_header_field_t));
    uint32_t hi = 0;
    fields[hi++] = (sh2_header_field_t){
        .name = ":method", .name_len = 7,
        .value = method, .value_len = (uint32_t)strlen(method),
    };
    fields[hi++] = (sh2_header_field_t){
        .name = ":path", .name_len = 5,
        .value = path, .value_len = (uint32_t)strlen(path),
    };
    fields[hi++] = (sh2_header_field_t){
        .name = ":scheme", .name_len = 7,
        .value = "http", .value_len = 4,
    };
    fields[hi++] = (sh2_header_field_t){
        .name = ":authority", .name_len = 10,
        .value = host, .value_len = (uint32_t)strlen(host),
    };
    if (token) {
        char *auth_val = NULL;
        if (asprintf(&auth_val, "Bearer %s", token) < 0) {
            free(fields);
            return -1;
        }
        fields[hi++] = (sh2_header_field_t){
            .name = "authorization", .name_len = 13,
            .value = auth_val, .value_len = (uint32_t)strlen(auth_val),
        };
    }
    if (body && body_len > 0) {
        fields[hi++] = (sh2_header_field_t){
            .name = "content-type", .name_len = 12,
            .value = "application/octet-stream", .value_len = 24,
        };
    }

    /* Submit request entity */
    shift_entity_t re;
    shift_entity_create_one_begin(c->sh, c->client_request_in, &re);

    sh2_session_t *rsess = NULL;
    shift_entity_get_component(c->sh, re, c->comp.session, (void **)&rsess);
    rsess->entity = c->session_entity;

    sh2_req_headers_t *rh = NULL;
    shift_entity_get_component(c->sh, re, c->comp.req_headers, (void **)&rh);
    rh->fields = fields;
    rh->count  = hi;

    if (body && body_len > 0) {
        sh2_req_body_t *rb = NULL;
        shift_entity_get_component(c->sh, re, c->comp.req_body, (void **)&rb);
        void *body_copy = malloc(body_len);
        memcpy(body_copy, body, body_len);
        rb->data = body_copy;
        rb->len  = (uint32_t)body_len;
    }

    shift_entity_create_one_end(c->sh, re);

    /* Poll until response complete */
    bool done = false;
    while (!done) {
        if (sh2_poll(c->ctx, 0) != sh2_ok)
            return -1;

        shift_entity_t *entities = NULL;
        size_t count = 0;
        shift_collection_get_entities(c->sh, c->client_result_out,
                                      &entities, &count);
        for (size_t i = 0; i < count; i++) {
            sh2_status_t *st = NULL;
            shift_entity_get_component(c->sh, entities[i], c->comp.status,
                                       (void **)&st);
            sh2_resp_body_t *rb = NULL;
            shift_entity_get_component(c->sh, entities[i], c->comp.resp_body,
                                       (void **)&rb);

            resp->status = st ? st->code : 0;
            if (rb && rb->data && rb->len > 0) {
                resp->body = malloc(rb->len + 1);
                memcpy(resp->body, rb->data, rb->len);
                resp->body[rb->len] = '\0';
                resp->body_len = rb->len;
            }

            shift_entity_destroy_one(c->sh, entities[i]);
            done = true;
        }
        shift_flush(c->sh);
    }
    return 0;
}

static void client_destroy(ctl_client_t *c) {
    if (c->ctx) sh2_context_destroy(c->ctx);
    if (c->sh) {
        shift_flush(c->sh);
        shift_context_destroy(c->sh);
    }
}

/* ======================================================================
 * URL encoding helper
 * ====================================================================== */

static char *url_encode_param(const char *key, const char *value,
                              char *buf, size_t bufsize) {
    /* Simple encoding — just handles the common case.
     * For admin keys this is sufficient. */
    int n = snprintf(buf, bufsize, "%s=", key);
    size_t pos = (size_t)n;
    for (const char *p = value; *p && pos < bufsize - 4; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~' || c == '/') {
            buf[pos++] = (char)c;
        } else {
            pos += (size_t)snprintf(buf + pos, bufsize - pos, "%%%02X", c);
        }
    }
    buf[pos] = '\0';
    return buf;
}

/* ======================================================================
 * Commands
 * ====================================================================== */

static int cmd_get(ctl_client_t *c, const char *host, const char *token,
                   int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "Usage: sjs ctl get <key>\n");
        return 1;
    }
    char param[1024];
    url_encode_param("key", argv[0], param, sizeof(param));
    char path[2048];
    snprintf(path, sizeof(path), "/_admin/kv?%s", param);

    ctl_response_t resp;
    if (client_request(c, host, "GET", path, NULL, 0, token, &resp) != 0)
        return 1;

    if (resp.status == 200 && resp.body) {
        fwrite(resp.body, 1, resp.body_len, stdout);
        if (resp.body_len > 0 && resp.body[resp.body_len - 1] != '\n')
            putchar('\n');
    } else if (resp.status == 404) {
        fprintf(stderr, "not found\n");
        free(resp.body);
        return 1;
    } else {
        fprintf(stderr, "error: %u %s\n", resp.status,
                resp.body ? resp.body : "");
        free(resp.body);
        return 1;
    }
    free(resp.body);
    return 0;
}

static int cmd_put(ctl_client_t *c, const char *host, const char *token,
                   int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: sjs ctl put <key> <value>\n");
        return 1;
    }
    char param[1024];
    url_encode_param("key", argv[0], param, sizeof(param));
    char path[2048];
    snprintf(path, sizeof(path), "/_admin/kv?%s", param);

    ctl_response_t resp;
    if (client_request(c, host, "PUT", path,
                       argv[1], strlen(argv[1]), token, &resp) != 0)
        return 1;

    if (resp.status != 200) {
        fprintf(stderr, "error: %u %s\n", resp.status,
                resp.body ? resp.body : "");
        free(resp.body);
        return 1;
    }
    free(resp.body);
    return 0;
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    *out_len = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    return buf;
}

static int cmd_putfile(ctl_client_t *c, const char *host, const char *token,
                       int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: sjs ctl putfile <key> <file>\n");
        return 1;
    }
    size_t flen = 0;
    char *fdata = read_file(argv[1], &flen);
    if (!fdata) {
        fprintf(stderr, "error: cannot read %s\n", argv[1]);
        return 1;
    }

    char param[1024];
    url_encode_param("key", argv[0], param, sizeof(param));
    char path[2048];
    snprintf(path, sizeof(path), "/_admin/kv?%s", param);

    ctl_response_t resp;
    int rc = client_request(c, host, "PUT", path, fdata, flen, token, &resp);
    free(fdata);
    if (rc != 0) return 1;

    if (resp.status != 200) {
        fprintf(stderr, "error: %u %s\n", resp.status,
                resp.body ? resp.body : "");
        free(resp.body);
        return 1;
    }
    free(resp.body);
    return 0;
}

static int cmd_delete(ctl_client_t *c, const char *host, const char *token,
                      int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "Usage: sjs ctl delete <key>\n");
        return 1;
    }
    char param[1024];
    url_encode_param("key", argv[0], param, sizeof(param));
    char path[2048];
    snprintf(path, sizeof(path), "/_admin/kv?%s", param);

    ctl_response_t resp;
    if (client_request(c, host, "DELETE", path, NULL, 0, token, &resp) != 0)
        return 1;

    if (resp.status != 200) {
        fprintf(stderr, "error: %u %s\n", resp.status,
                resp.body ? resp.body : "");
        free(resp.body);
        return 1;
    }
    free(resp.body);
    return 0;
}

static int cmd_range(ctl_client_t *c, const char *host, const char *token,
                     int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: sjs ctl range <start> <end> [limit]\n");
        return 1;
    }
    char sp[1024], ep[1024];
    url_encode_param("start", argv[0], sp, sizeof(sp));
    url_encode_param("end", argv[1], ep, sizeof(ep));

    char path[4096];
    if (argc >= 3)
        snprintf(path, sizeof(path), "/_admin/kv/range?%s&%s&limit=%s",
                 sp, ep, argv[2]);
    else
        snprintf(path, sizeof(path), "/_admin/kv/range?%s&%s", sp, ep);

    ctl_response_t resp;
    if (client_request(c, host, "GET", path, NULL, 0, token, &resp) != 0)
        return 1;

    if (resp.status == 200 && resp.body) {
        printf("%s\n", resp.body);
    } else {
        fprintf(stderr, "error: %u %s\n", resp.status,
                resp.body ? resp.body : "");
        free(resp.body);
        return 1;
    }
    free(resp.body);
    return 0;
}

static int cmd_list(ctl_client_t *c, const char *host, const char *token,
                    int argc, char **argv) {
    const char *prefix = argc > 0 ? argv[0] : "";

    char sp[1024], ep[1024];
    url_encode_param("start", prefix, sp, sizeof(sp));

    /* End key = prefix + 0x7f (ASCII DEL, sorts after all printable) */
    char end_val[512];
    snprintf(end_val, sizeof(end_val), "%s\x7f", prefix);
    url_encode_param("end", end_val, ep, sizeof(ep));

    char path[4096];
    snprintf(path, sizeof(path), "/_admin/kv/range?%s&%s&limit=10000", sp, ep);

    ctl_response_t resp;
    if (client_request(c, host, "GET", path, NULL, 0, token, &resp) != 0)
        return 1;

    if (resp.status == 200 && resp.body) {
        printf("%s\n", resp.body);
    } else {
        fprintf(stderr, "error: %u %s\n", resp.status,
                resp.body ? resp.body : "");
        free(resp.body);
        return 1;
    }
    free(resp.body);
    return 0;
}

static int cmd_domain_map(ctl_client_t *c, const char *host, const char *token,
                          int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: sjs ctl domain-map <hostname> <tenant_id>\n");
        return 1;
    }
    char hp[1024], ip[1024];
    url_encode_param("host", argv[0], hp, sizeof(hp));
    url_encode_param("id", argv[1], ip, sizeof(ip));
    char path[2048];
    snprintf(path, sizeof(path), "/_admin/domain?%s&%s", hp, ip);

    ctl_response_t resp;
    if (client_request(c, host, "PUT", path, NULL, 0, token, &resp) != 0)
        return 1;
    if (resp.status != 200) {
        fprintf(stderr, "error: %u %s\n", resp.status,
                resp.body ? resp.body : "");
        free(resp.body);
        return 1;
    }
    free(resp.body);
    return 0;
}

static int cmd_domain_unmap(ctl_client_t *c, const char *host,
                            const char *token, int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "Usage: sjs ctl domain-unmap <hostname>\n");
        return 1;
    }
    char hp[1024];
    url_encode_param("host", argv[0], hp, sizeof(hp));
    char path[2048];
    snprintf(path, sizeof(path), "/_admin/domain?%s", hp);

    ctl_response_t resp;
    if (client_request(c, host, "DELETE", path, NULL, 0, token, &resp) != 0)
        return 1;
    if (resp.status != 200) {
        fprintf(stderr, "error: %u %s\n", resp.status,
                resp.body ? resp.body : "");
        free(resp.body);
        return 1;
    }
    free(resp.body);
    return 0;
}

static int cmd_cert_put(ctl_client_t *c, const char *host, const char *token,
                        int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: sjs ctl cert-put <name> <cert_file> <key_file>\n");
        return 1;
    }

    const char *name = argv[0];
    const char *cert_path = argv[1];
    const char *key_path  = argv[2];

    /* Upload cert */
    size_t cert_len = 0;
    char *cert_data = read_file(cert_path, &cert_len);
    if (!cert_data) {
        fprintf(stderr, "error: cannot read %s\n", cert_path);
        return 1;
    }
    char np[1024];
    url_encode_param("name", name, np, sizeof(np));
    char path[2048];
    snprintf(path, sizeof(path), "/_admin/cert?%s&type=cert", np);

    ctl_response_t resp;
    int rc = client_request(c, host, "PUT", path, cert_data, cert_len,
                            token, &resp);
    free(cert_data);
    if (rc != 0) return 1;
    if (resp.status != 200) {
        fprintf(stderr, "error uploading cert: %u %s\n", resp.status,
                resp.body ? resp.body : "");
        free(resp.body);
        return 1;
    }
    free(resp.body);

    /* Upload key */
    size_t key_len = 0;
    char *key_data = read_file(key_path, &key_len);
    if (!key_data) {
        fprintf(stderr, "error: cannot read %s\n", key_path);
        return 1;
    }
    snprintf(path, sizeof(path), "/_admin/cert?%s&type=key", np);

    rc = client_request(c, host, "PUT", path, key_data, key_len, token, &resp);
    free(key_data);
    if (rc != 0) return 1;
    if (resp.status != 200) {
        fprintf(stderr, "error uploading key: %u %s\n", resp.status,
                resp.body ? resp.body : "");
        free(resp.body);
        return 1;
    }
    free(resp.body);
    return 0;
}

/* ======================================================================
 * Entry point
 * ====================================================================== */

static void ctl_usage(void) {
    fprintf(stderr,
        "Usage: sjs ctl [options] <command> [args...]\n\n"
        "Commands:\n"
        "  get <key>                       Get value for key\n"
        "  put <key> <value>               Set key to value\n"
        "  putfile <key> <file>            Set key to file contents\n"
        "  delete <key>                    Delete key\n"
        "  range <start> <end> [limit]     List keys in range\n"
        "  list [prefix]                   List all keys with prefix\n"
        "  domain-map <host> <tenant_id>   Map hostname to tenant\n"
        "  domain-unmap <host>             Remove hostname mapping\n"
        "  cert-put <name> <cert> <key>    Store TLS cert/key pair\n"
        "\n"
        "Options:\n"
        "  -h <host>    Server host (default: 127.0.0.1)\n"
        "  -p <port>    Server port (default: 9000)\n"
        "  -t <token>   Auth token\n"
        "\n");
}

int cmd_ctl(int argc, char **argv) {
    const char *host  = "127.0.0.1";
    uint16_t    port  = 9000;
    const char *token = NULL;

    /* Parse options before subcommand */
    int i = 0;
    while (i < argc) {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            host = argv[++i]; i++;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]); i++;
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            token = argv[++i]; i++;
        } else if (strcmp(argv[i], "--help") == 0) {
            ctl_usage();
            return 0;
        } else {
            break;
        }
    }

    if (i >= argc) {
        ctl_usage();
        return 1;
    }

    const char *cmd = argv[i];
    int cmd_argc = argc - i - 1;
    char **cmd_argv = argv + i + 1;

    /* Connect */
    ctl_client_t client;
    if (client_init(&client) != 0) {
        fprintf(stderr, "sjs ctl: failed to initialize\n");
        return 1;
    }
    if (client_connect(&client, host, port) != 0) {
        fprintf(stderr, "sjs ctl: failed to connect to %s:%u\n", host, port);
        client_destroy(&client);
        return 1;
    }

    int rc;
    if (strcmp(cmd, "get") == 0)
        rc = cmd_get(&client, host, token, cmd_argc, cmd_argv);
    else if (strcmp(cmd, "put") == 0)
        rc = cmd_put(&client, host, token, cmd_argc, cmd_argv);
    else if (strcmp(cmd, "putfile") == 0)
        rc = cmd_putfile(&client, host, token, cmd_argc, cmd_argv);
    else if (strcmp(cmd, "delete") == 0)
        rc = cmd_delete(&client, host, token, cmd_argc, cmd_argv);
    else if (strcmp(cmd, "range") == 0)
        rc = cmd_range(&client, host, token, cmd_argc, cmd_argv);
    else if (strcmp(cmd, "list") == 0)
        rc = cmd_list(&client, host, token, cmd_argc, cmd_argv);
    else if (strcmp(cmd, "domain-map") == 0)
        rc = cmd_domain_map(&client, host, token, cmd_argc, cmd_argv);
    else if (strcmp(cmd, "domain-unmap") == 0)
        rc = cmd_domain_unmap(&client, host, token, cmd_argc, cmd_argv);
    else if (strcmp(cmd, "cert-put") == 0)
        rc = cmd_cert_put(&client, host, token, cmd_argc, cmd_argv);
    else {
        fprintf(stderr, "sjs ctl: unknown command '%s'\n\n", cmd);
        ctl_usage();
        rc = 1;
    }

    client_destroy(&client);
    return rc;
}
