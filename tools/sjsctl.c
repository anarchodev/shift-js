#include "kvstore.h"
#include "code_db.h"
#include "preprocessor.h"
#include "ejs.h"
#include "typescript.h"

#include <quickjs.h>

#define DMON_IMPL
#include "dmon.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <openssl/evp.h>

/* Forward declarations */
static const char *prefixed_key(const char *prefix, const char *key,
                                char *buf, size_t bufsize);
static char *read_file(const char *path, size_t *out_len);
static void on_code_write(kvstore_t *kv, const char *key,
                          const char *content, size_t content_len,
                          const char *tenant_prefix);
static void on_code_delete(kvstore_t *kv, const char *key,
                           const char *tenant_prefix);

/* Extensions treated as static files (served from __static/ not __code/) */
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

static int write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    return (written == len) ? 0 : -1;
}

static int mkdirs(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    return 0;
}

static int export_code(kvstore_t *kv, const char *out_dir,
                       const char *tenant_prefix) {
    /* Range scan for all __code/ keys */
    char sbuf[4096], ebuf[4096];
    const char *raw_start = "__code/";
    const char *start = prefixed_key(tenant_prefix, raw_start,
                                     sbuf, sizeof(sbuf));
    char raw_end[64];
    snprintf(raw_end, sizeof(raw_end), "__code/\x7f");
    const char *end = prefixed_key(tenant_prefix, raw_end,
                                   ebuf, sizeof(ebuf));

    kv_range_result_t result;
    if (kv_range(kv, start, end, 100000, &result) != 0) {
        fprintf(stderr, "Failed to list code entries\n");
        return -1;
    }

    size_t pfx_len = tenant_prefix ? strlen(tenant_prefix) : 0;
    int count = 0;

    for (size_t i = 0; i < result.count; i++) {
        /* Strip tenant prefix and __code/ to get relative path */
        const char *full_key = result.entries[i].key + pfx_len;
        if (strncmp(full_key, "__code/", 7) != 0) continue;
        const char *rel_path = full_key + 7; /* skip "__code/" */

        char filepath[4096];
        snprintf(filepath, sizeof(filepath), "%s/%s", out_dir, rel_path);

        /* Create parent directories */
        if (mkdirs(filepath) != 0) {
            fprintf(stderr, "Cannot create directories for %s: %s\n",
                    filepath, strerror(errno));
            continue;
        }

        if (write_file(filepath, result.entries[i].value,
                       result.entries[i].value_len) != 0) {
            fprintf(stderr, "  FAILED: %s\n", filepath);
        } else {
            printf("  %s → %s (%zu bytes)\n", full_key, filepath,
                   result.entries[i].value_len);
            count++;
        }
    }

    kv_range_free(&result);
    return count;
}

/* ---- watch command ---------------------------------------------------- */

static volatile sig_atomic_t g_running = 1;

static void sigint_handler(int sig) {
    (void)sig;
    g_running = 0;
}

typedef struct {
    kvstore_t   *kv;
    const char  *dir;
    const char  *code_prefix;
    const char  *tenant_prefix;
} watch_ctx_t;

static void sync_single_file(watch_ctx_t *ctx, const char *relpath) {
    /* Build filesystem path */
    char filepath[4096];
    snprintf(filepath, sizeof(filepath), "%s/%s", ctx->dir, relpath);

    /* Determine if this is a static file or a code file */
    int is_static = is_static_file(relpath);
    const char *kv_ns = is_static ? "__static" : "__code";

    /* Build KV key: [tenant_prefix]<ns>/[code_prefix/]<relpath> */
    char raw_key[4096];
    if (ctx->code_prefix[0])
        snprintf(raw_key, sizeof(raw_key), "%s/%s/%s",
                 kv_ns, ctx->code_prefix, relpath);
    else
        snprintf(raw_key, sizeof(raw_key), "%s/%s", kv_ns, relpath);

    char key[4096];
    const char *actual_key = prefixed_key(ctx->tenant_prefix, raw_key,
                                          key, sizeof(key));

    size_t flen;
    char *contents = read_file(filepath, &flen);
    if (!contents) {
        fprintf(stderr, "  [watch] cannot read %s\n", filepath);
        return;
    }

    if (kv_put(ctx->kv, actual_key, contents, flen) == 0) {
        if (!is_static)
            on_code_write(ctx->kv, actual_key, contents, flen, ctx->tenant_prefix);
        printf("  [watch] %s → %s (%zu bytes)\n", relpath, actual_key, flen);
    } else {
        fprintf(stderr, "  [watch] FAILED: %s\n", actual_key);
    }

    free(contents);
}

static void delete_single_file(watch_ctx_t *ctx, const char *relpath) {
    int is_static = is_static_file(relpath);
    const char *kv_ns = is_static ? "__static" : "__code";

    char raw_key[4096];
    if (ctx->code_prefix[0])
        snprintf(raw_key, sizeof(raw_key), "%s/%s/%s",
                 kv_ns, ctx->code_prefix, relpath);
    else
        snprintf(raw_key, sizeof(raw_key), "%s/%s", kv_ns, relpath);

    char key[4096];
    const char *actual_key = prefixed_key(ctx->tenant_prefix, raw_key,
                                          key, sizeof(key));

    if (kv_delete(ctx->kv, actual_key) == 0) {
        if (!is_static)
            on_code_delete(ctx->kv, actual_key, ctx->tenant_prefix);
        printf("  [watch] deleted %s\n", actual_key);
    }
}

static void watch_callback(dmon_watch_id watch_id, dmon_action action,
                            const char *rootdir, const char *filepath,
                            const char *oldfilepath, void *user) {
    (void)watch_id;
    (void)rootdir;
    watch_ctx_t *ctx = (watch_ctx_t *)user;

    /* Skip hidden files/directories */
    if (filepath[0] == '.') return;
    const char *slash = filepath;
    while ((slash = strchr(slash, '/')) != NULL) {
        slash++;
        if (*slash == '.') return;
    }

    switch (action) {
    case DMON_ACTION_CREATE:
    case DMON_ACTION_MODIFY:
        sync_single_file(ctx, filepath);
        break;
    case DMON_ACTION_DELETE:
        delete_single_file(ctx, filepath);
        break;
    case DMON_ACTION_MOVE:
        if (oldfilepath) delete_single_file(ctx, oldfilepath);
        sync_single_file(ctx, filepath);
        break;
    }
}

/* ---- end watch -------------------------------------------------------- */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s -d <db> [-t <tenant_id>] <command> [args...]\n\n"
        "Commands:\n"
        "  get <key>                   Print value for key\n"
        "  put <key> <value>           Set key to value (string)\n"
        "  putfile <key> <file>        Set key to file contents\n"
        "  delete <key>                Delete key\n"
        "  range <start> <end> [count] List keys in range\n"
        "  upload <dir> [prefix]       Upload directory to code database\n"
        "  deploy                      Compile and deploy code to KV\n"
        "  import <dir> [prefix]       Alias for upload\n"
        "  export <dir>                Export all __code/ entries to directory\n"
        "  watch <dir> [prefix]        Watch directory and sync changes to server\n"
        "  list [prefix]               List all keys with optional prefix\n"
        "  domain-map <host> <id>      Map hostname to tenant ID\n"
        "  domain-unmap <host>         Remove hostname mapping\n"
        "  cert-put <name> <cert> <key> Store cert/key PEM files\n"
        "\n"
        "Options:\n"
        "  -t <tenant_id>  Scope all key operations under tenants/<id>/\n"
        , prog);
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)len);
    if (!buf) { fclose(f); return NULL; }

    size_t nread = fread(buf, 1, (size_t)len, f);
    fclose(f);

    *out_len = nread;
    return buf;
}

/* Prepend tenant prefix to a key. Returns key as-is if prefix is NULL. */
static const char *prefixed_key(const char *prefix, const char *key,
                                char *buf, size_t bufsize) {
    if (!prefix) return key;
    snprintf(buf, bufsize, "%s%s", prefix, key);
    return buf;
}

/* Compute SHA-256 hex string of content. */
static void sha256_hex(const void *data, size_t len, char out[65]) {
    uint8_t digest[32];
    unsigned int dlen = 0;
    EVP_Digest(data, len, digest, &dlen, EVP_sha256(), NULL);
    for (unsigned int i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", digest[i]);
}

/* For a __code/ key write: invalidate bytecode cache, store content hash
 * in __code_meta/, and store content-addressed blob in __code_blob/. */
static void on_code_write(kvstore_t *kv, const char *key,
                          const char *content, size_t content_len,
                          const char *tenant_prefix) {
    /* Work on the unprefixed key — strip tenant prefix if present */
    const char *raw = key;
    if (tenant_prefix && strncmp(raw, tenant_prefix, strlen(tenant_prefix)) == 0)
        raw += strlen(tenant_prefix);

    if (strncmp(raw, "__code/", 7) != 0) return;
    const char *rel = raw + 7;

    /* 1. Invalidate __compiled/ cache (with extension) */
    char raw_cache[4096];
    snprintf(raw_cache, sizeof(raw_cache), "__compiled/%s", rel);
    char cache_key[4096];
    const char *ck = prefixed_key(tenant_prefix, raw_cache,
                                  cache_key, sizeof(cache_key));
    kv_delete(kv, ck);

    /* 2. Compute content hash and store in __code_meta/ */
    char hash[65];
    sha256_hex(content, content_len, hash);

    char meta_raw[4096];
    snprintf(meta_raw, sizeof(meta_raw), "__code_meta/%s", rel);
    char meta_buf[4096];
    const char *meta_key = prefixed_key(tenant_prefix, meta_raw,
                                         meta_buf, sizeof(meta_buf));
    kv_put(kv, meta_key, hash, 64);

    /* 3. Store content-addressed blob (only if not already present) */
    char blob_raw[128];
    snprintf(blob_raw, sizeof(blob_raw), "__code_blob/%s", hash);
    char blob_buf[4096];
    const char *blob_key = prefixed_key(tenant_prefix, blob_raw,
                                         blob_buf, sizeof(blob_buf));
    void *existing = NULL;
    size_t elen = 0;
    if (kv_get(kv, blob_key, &existing, &elen) != 0)
        kv_put(kv, blob_key, content, content_len);
    free(existing);
}

/* For a __code/ key delete: clean up cache and meta. */
static void on_code_delete(kvstore_t *kv, const char *key,
                           const char *tenant_prefix) {
    const char *raw = key;
    if (tenant_prefix && strncmp(raw, tenant_prefix, strlen(tenant_prefix)) == 0)
        raw += strlen(tenant_prefix);

    if (strncmp(raw, "__code/", 7) != 0) return;
    const char *rel = raw + 7;

    /* Delete __compiled/ cache (with extension) */
    char raw_cache[4096];
    snprintf(raw_cache, sizeof(raw_cache), "__compiled/%s", rel);
    char cache_key[4096];
    const char *ck = prefixed_key(tenant_prefix, raw_cache,
                                  cache_key, sizeof(cache_key));
    kv_delete(kv, ck);

    /* Delete __code_meta/ */
    char meta_raw[4096];
    snprintf(meta_raw, sizeof(meta_raw), "__code_meta/%s", rel);
    char meta_buf[4096];
    const char *meta_key = prefixed_key(tenant_prefix, meta_raw,
                                         meta_buf, sizeof(meta_buf));
    kv_delete(kv, meta_key);
}

static int upload_dir(kvstore_t *kv, const char *dir_path,
                      const char *code_prefix, const char *tenant_prefix) {
    DIR *d = opendir(dir_path);
    if (!d) {
        fprintf(stderr, "Cannot open directory: %s: %s\n",
                dir_path, strerror(errno));
        return -1;
    }

    struct dirent *ent;
    int count = 0;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char filepath[4096];
        snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if (stat(filepath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Recurse into subdirectory */
            char subprefix[4096];
            if (code_prefix[0])
                snprintf(subprefix, sizeof(subprefix), "%s/%s",
                         code_prefix, ent->d_name);
            else
                snprintf(subprefix, sizeof(subprefix), "%s", ent->d_name);

            int sub = upload_dir(kv, filepath, subprefix, tenant_prefix);
            if (sub > 0) count += sub;
            continue;
        }

        if (!S_ISREG(st.st_mode)) continue;

        /* Determine if this is a static file or a code file */
        int is_static = is_static_file(ent->d_name);
        const char *kv_ns = is_static ? "__static" : "__code";

        /* Build KV key: [tenant_prefix]<ns>/[code_prefix/]<filename> */
        char raw_key[4096];
        if (code_prefix[0])
            snprintf(raw_key, sizeof(raw_key), "%s/%s/%s",
                     kv_ns, code_prefix, ent->d_name);
        else
            snprintf(raw_key, sizeof(raw_key), "%s/%s", kv_ns, ent->d_name);

        char key[4096];
        const char *actual_key = prefixed_key(tenant_prefix, raw_key,
                                              key, sizeof(key));

        size_t flen;
        char *contents = read_file(filepath, &flen);
        if (!contents) {
            fprintf(stderr, "Cannot read %s\n", filepath);
            continue;
        }

        if (kv_put(kv, actual_key, contents, flen) == 0) {
            if (!is_static)
                on_code_write(kv, actual_key, contents, flen, tenant_prefix);
            printf("  %s → %s (%zu bytes)\n", filepath, actual_key, flen);
            count++;
        } else {
            fprintf(stderr, "  FAILED: %s\n", actual_key);
        }

        free(contents);
    }

    closedir(d);
    return count;
}

/* ---- code_db upload -------------------------------------------------- */

static int upload_dir_code_db(code_db_t *cdb, const char *database_id,
                              const char *dir_path, const char *prefix) {
    DIR *d = opendir(dir_path);
    if (!d) {
        fprintf(stderr, "Cannot open directory: %s: %s\n",
                dir_path, strerror(errno));
        return -1;
    }

    struct dirent *ent;
    int count = 0;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char filepath[4096];
        snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if (stat(filepath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            char subprefix[4096];
            if (prefix[0])
                snprintf(subprefix, sizeof(subprefix), "%s/%s",
                         prefix, ent->d_name);
            else
                snprintf(subprefix, sizeof(subprefix), "%s", ent->d_name);

            int sub = upload_dir_code_db(cdb, database_id, filepath, subprefix);
            if (sub > 0) count += sub;
            continue;
        }

        if (!S_ISREG(st.st_mode)) continue;

        /* Build path: [prefix/]filename */
        char path[4096];
        if (prefix[0])
            snprintf(path, sizeof(path), "%s/%s", prefix, ent->d_name);
        else
            snprintf(path, sizeof(path), "%s", ent->d_name);

        size_t flen;
        char *contents = read_file(filepath, &flen);
        if (!contents) {
            fprintf(stderr, "Cannot read %s\n", filepath);
            continue;
        }

        if (code_db_put_file(cdb, database_id, path, contents, flen) == 0) {
            printf("  %s → %s (%zu bytes)\n", filepath, path, flen);
            count++;
        } else {
            fprintf(stderr, "  FAILED: %s\n", path);
        }

        free(contents);
    }

    closedir(d);
    return count;
}

/* ---- deploy: compile & write to KV ----------------------------------- */

/* Deploy context — passed to module loader via JS_SetRuntimeOpaque. */
typedef struct {
    code_db_t                        *cdb;
    const char                       *database_id;
    const char                       *tree_hash;
    const sjs_preprocessor_registry_t *preprocessors;
    JSRuntime                        *compile_rt;
    JSContext                        *compile_ctx;
    sjs_ts_ctx_t                      ts_ctx;
} deploy_ctx_t;

/* Module loader for deploy-time compilation: resolves imports from code_db. */
static JSModuleDef *deploy_module_loader(JSContext *ctx,
                                         const char *module_name,
                                         void *opaque) {
    deploy_ctx_t *dc = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    if (!dc) return NULL;

    /* Resolve module source from code_db tree */
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
        /* Try extensions in order: .mjs, then preprocessor extensions */
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

    char  *compile_source = source;
    size_t compile_len    = source_len;

    const sjs_preprocessor_entry_t *pp = ext
        ? sjs_preprocessor_find(dc->preprocessors, ext)
        : NULL;

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
        fprintf(stderr, "deploy: failed to initialize Sucrase\n");
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

/* Compile a single source file to bytecode.
 * Returns malloc'd bytecode (caller frees), NULL on error. */
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
            fprintf(stderr, "deploy: preprocess failed: %s\n", path);
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
        fprintf(stderr, "deploy: compile error in %s: %s\n",
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
        fprintf(stderr, "deploy: bytecode write failed: %s\n", path);
        return NULL;
    }

    *out_bc_len = bc_len;
    return bc;
}

static int do_deploy(kvstore_t *kv, code_db_t *cdb, const char *database_id) {
    /* Snapshot working tree. */
    char *tree_hash = code_db_snapshot(cdb, database_id);
    if (!tree_hash) {
        fprintf(stderr, "deploy: no files in working tree\n");
        return -1;
    }

    printf("Tree hash: %s\n", tree_hash);

    /* Set up preprocessor registry. */
    sjs_preprocessor_registry_t pp;
    sjs_preprocessor_init(&pp);
    sjs_preprocessor_register(&pp, ".ejs", sjs_ejs_transform, NULL);
    sjs_preprocessor_register(&pp, ".ts",  sjs_typescript_transform, NULL);
    sjs_preprocessor_register(&pp, ".tsx", sjs_typescript_transform, NULL);

    /* Set up deploy compilation context. */
    deploy_ctx_t dc;
    if (deploy_init(&dc, cdb, database_id, tree_hash, &pp) != 0) {
        free(tree_hash);
        return -1;
    }

    /* Wire up TypeScript bindings to preprocessor entries. */
    for (size_t i = 0; i < pp.count; i++) {
        sjs_preprocessor_entry_t *e = &pp.entries[i];
        if (e->transform != sjs_typescript_transform) continue;
        if (strcmp(e->extension, ".tsx") == 0)
            e->user_data = &dc.ts_ctx.tsx_binding;
        else
            e->user_data = &dc.ts_ctx.ts_binding;
    }

    /* List all files in the tree. */
    code_tree_t tree;
    if (code_db_tree_list(cdb, database_id, tree_hash, &tree) != 0) {
        fprintf(stderr, "deploy: failed to list tree\n");
        deploy_free(&dc);
        free(tree_hash);
        return -1;
    }

    int code_count = 0, file_count = 0, errors = 0;

    for (size_t i = 0; i < tree.count; i++) {
        const char *path = tree.entries[i].path;
        const char *sha1 = tree.entries[i].sha1;

        /* Fetch file content from code_db. */
        void *content = NULL;
        size_t content_len = 0;
        if (code_db_get_file(cdb, sha1, &content, &content_len) != 0) {
            fprintf(stderr, "deploy: missing blob %s for %s\n", sha1, path);
            errors++;
            continue;
        }

        if (is_static_file(path)) {
            /* Write static file to KV. */
            char key[1024];
            snprintf(key, sizeof(key), "database/%s/files/%s/%s",
                     database_id, tree_hash, path);
            if (kv_put(kv, key, content, content_len) == 0) {
                file_count++;
            } else {
                fprintf(stderr, "deploy: kv_put failed: %s\n", key);
                errors++;
            }
        } else {
            /* Compile and write bytecode to KV.
             * Value format: [40 bytes sha1 hex][bytecode] */
            size_t bc_len = 0;
            void *bc = deploy_compile(&dc, path, content, content_len, &bc_len);
            if (!bc) {
                errors++;
                free(content);
                continue;
            }

            size_t val_len = 40 + bc_len;
            char *val = malloc(val_len);
            if (!val) {
                js_free(dc.compile_ctx, bc);
                errors++;
                free(content);
                continue;
            }
            memcpy(val, sha1, 40);
            memcpy(val + 40, bc, bc_len);
            js_free(dc.compile_ctx, bc);

            char key[1024];
            snprintf(key, sizeof(key), "database/%s/code/%s/%s",
                     database_id, tree_hash, path);
            if (kv_put(kv, key, val, val_len) == 0) {
                code_count++;
            } else {
                fprintf(stderr, "deploy: kv_put failed: %s\n", key);
                errors++;
            }
            free(val);
        }

        free(content);
    }

    code_db_tree_free(&tree);
    deploy_free(&dc);

    if (errors > 0) {
        fprintf(stderr, "deploy: %d errors, aborting\n", errors);
        free(tree_hash);
        return -1;
    }

    /* Atomic switch: set current_hash. */
    char hash_key[256];
    snprintf(hash_key, sizeof(hash_key), "database/%s/current_hash", database_id);
    if (kv_put(kv, hash_key, tree_hash, 40) != 0) {
        fprintf(stderr, "deploy: failed to set current_hash\n");
        free(tree_hash);
        return -1;
    }

    printf("Deployed: %d code files, %d static files\n", code_count, file_count);
    free(tree_hash);
    return 0;
}

/* ---- main ------------------------------------------------------------ */

int main(int argc, char **argv) {
    const char *db_path = NULL;
    const char *tenant_prefix_str = NULL;
    char tenant_buf[128];
    int arg_start = 1;

    /* Parse flags: -d <db> [-t <tenant_id>] */
    while (arg_start + 1 < argc) {
        if (!strcmp(argv[arg_start], "-d")) {
            db_path = argv[arg_start + 1];
            arg_start += 2;
        } else if (!strcmp(argv[arg_start], "-t")) {
            long tid = atol(argv[arg_start + 1]);
            if (tid <= 0) {
                fprintf(stderr, "Tenant ID must be a positive integer\n");
                return 1;
            }
            snprintf(tenant_buf, sizeof(tenant_buf),
                     "tenants/%ld/", tid);
            tenant_prefix_str = tenant_buf;
            arg_start += 2;
        } else {
            break;
        }
    }

    if (!db_path || arg_start >= argc) {
        usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[arg_start];

    kvstore_t *kv = NULL;
    if (kv_open(db_path, &kv) != 0) {
        fprintf(stderr, "Failed to open database: %s\n", db_path);
        return 1;
    }

    int rc = 0;

    if (!strcmp(cmd, "get")) {
        if (arg_start + 1 >= argc) { usage(argv[0]); rc = 1; goto done; }

        char kbuf[4096];
        const char *key = prefixed_key(tenant_prefix_str,
                                       argv[arg_start + 1],
                                       kbuf, sizeof(kbuf));

        void  *value = NULL;
        size_t vlen  = 0;
        int r = kv_get(kv, key, &value, &vlen);
        if (r == -1) {
            fprintf(stderr, "Not found: %s\n", key);
            rc = 1;
        } else if (r < 0) {
            fprintf(stderr, "Error reading key\n");
            rc = 1;
        } else {
            fwrite(value, 1, vlen, stdout);
            if (vlen > 0 && ((char *)value)[vlen - 1] != '\n')
                putchar('\n');
            free(value);
        }

    } else if (!strcmp(cmd, "put")) {
        if (arg_start + 2 >= argc) { usage(argv[0]); rc = 1; goto done; }

        char kbuf[4096];
        const char *key = prefixed_key(tenant_prefix_str,
                                       argv[arg_start + 1],
                                       kbuf, sizeof(kbuf));
        const char *val = argv[arg_start + 2];
        if (kv_put(kv, key, val, strlen(val)) != 0) {
            fprintf(stderr, "Failed to put key\n");
            rc = 1;
        } else {
            on_code_write(kv, key, val, strlen(val), tenant_prefix_str);
        }

    } else if (!strcmp(cmd, "putfile")) {
        if (arg_start + 2 >= argc) { usage(argv[0]); rc = 1; goto done; }

        char kbuf[4096];
        const char *key = prefixed_key(tenant_prefix_str,
                                       argv[arg_start + 1],
                                       kbuf, sizeof(kbuf));
        const char *path = argv[arg_start + 2];

        size_t flen;
        char *contents = read_file(path, &flen);
        if (!contents) {
            fprintf(stderr, "Cannot read file: %s\n", path);
            rc = 1;
        } else {
            if (kv_put(kv, key, contents, flen) != 0) {
                fprintf(stderr, "Failed to put key\n");
                rc = 1;
            } else {
                on_code_write(kv, key, contents, flen, tenant_prefix_str);
            }
            free(contents);
        }

    } else if (!strcmp(cmd, "delete")) {
        if (arg_start + 1 >= argc) { usage(argv[0]); rc = 1; goto done; }

        char kbuf[4096];
        const char *key = prefixed_key(tenant_prefix_str,
                                       argv[arg_start + 1],
                                       kbuf, sizeof(kbuf));
        if (kv_delete(kv, key) != 0) {
            fprintf(stderr, "Failed to delete key\n");
            rc = 1;
        }

    } else if (!strcmp(cmd, "range")) {
        if (arg_start + 2 >= argc) { usage(argv[0]); rc = 1; goto done; }

        char sbuf[4096], ebuf[4096];
        const char *start = prefixed_key(tenant_prefix_str,
                                         argv[arg_start + 1],
                                         sbuf, sizeof(sbuf));
        const char *end = prefixed_key(tenant_prefix_str,
                                       argv[arg_start + 2],
                                       ebuf, sizeof(ebuf));
        size_t count = 100;
        if (arg_start + 3 < argc) count = (size_t)atol(argv[arg_start + 3]);

        kv_range_result_t result;
        if (kv_range(kv, start, end, count, &result) != 0) {
            fprintf(stderr, "Range query failed\n");
            rc = 1;
        } else {
            size_t pfx_len = tenant_prefix_str ? strlen(tenant_prefix_str) : 0;
            for (size_t i = 0; i < result.count; i++) {
                const char *display_key = result.entries[i].key + pfx_len;
                printf("%s\t%zu bytes\n", display_key,
                       result.entries[i].value_len);
            }
            kv_range_free(&result);
        }

    } else if (!strcmp(cmd, "upload") || !strcmp(cmd, "import")) {
        if (arg_start + 1 >= argc) { usage(argv[0]); rc = 1; goto done; }
        const char *dir = argv[arg_start + 1];
        const char *prefix = (arg_start + 2 < argc) ? argv[arg_start + 2] : "";

        /* Upload to code_db (content-addressed). */
        char code_db_path[4096];
        snprintf(code_db_path, sizeof(code_db_path), "%s.code", db_path);
        code_db_t *cdb = NULL;
        if (code_db_open(code_db_path, &cdb) != 0) {
            fprintf(stderr, "Failed to open code database: %s\n", code_db_path);
            rc = 1;
            goto done;
        }

        code_db_tree_clear(cdb, "default");
        int n = upload_dir_code_db(cdb, "default", dir, prefix);
        printf("Uploaded %d files to code database\n", n);
        code_db_close(cdb);

        /* Also upload to legacy KV for backward compatibility. */
        {
            char sbuf[4096], ebuf[4096];
            const char *cs = prefixed_key(tenant_prefix_str, "__compiled/",
                                          sbuf, sizeof(sbuf));
            const char *ce = prefixed_key(tenant_prefix_str, "__compiled/\x7f",
                                          ebuf, sizeof(ebuf));
            kv_range_result_t cr;
            if (kv_range(kv, cs, ce, 100000, &cr) == 0) {
                for (size_t ci = 0; ci < cr.count; ci++)
                    kv_delete(kv, cr.entries[ci].key);
                if (cr.count > 0)
                    printf("Cleared %zu cached bytecode entries\n", cr.count);
                kv_range_free(&cr);
            }
        }
        upload_dir(kv, dir, prefix, tenant_prefix_str);

    } else if (!strcmp(cmd, "deploy")) {
        char code_db_path[4096];
        snprintf(code_db_path, sizeof(code_db_path), "%s.code", db_path);
        code_db_t *cdb = NULL;
        if (code_db_open(code_db_path, &cdb) != 0) {
            fprintf(stderr, "Failed to open code database: %s\n", code_db_path);
            rc = 1;
            goto done;
        }

        if (do_deploy(kv, cdb, "default") != 0)
            rc = 1;

        code_db_close(cdb);

    } else if (!strcmp(cmd, "watch")) {
        if (arg_start + 1 >= argc) { usage(argv[0]); rc = 1; goto done; }
        const char *dir = argv[arg_start + 1];
        const char *prefix = (arg_start + 2 < argc) ? argv[arg_start + 2] : "";

        /* Initial sync */
        int n = upload_dir(kv, dir, prefix, tenant_prefix_str);
        printf("Initial sync: %d files\n", n);

        /* Set up file watcher */
        dmon_init();

        watch_ctx_t ctx = {
            .kv = kv,
            .dir = dir,
            .code_prefix = prefix,
            .tenant_prefix = tenant_prefix_str
        };

        dmon_watch_id wid = dmon_watch(dir, watch_callback,
                                        DMON_WATCHFLAGS_RECURSIVE, &ctx);
        if (wid.id == 0) {
            fprintf(stderr, "Failed to watch directory: %s\n", dir);
            dmon_deinit();
            rc = 1;
            goto done;
        }

        signal(SIGINT, sigint_handler);
        signal(SIGTERM, sigint_handler);
        printf("Watching %s for changes (Ctrl-C to stop)...\n", dir);

        while (g_running) {
            usleep(100000); /* 100ms */
        }

        printf("\nStopping watch...\n");
        dmon_deinit();

    } else if (!strcmp(cmd, "export")) {
        if (arg_start + 1 >= argc) { usage(argv[0]); rc = 1; goto done; }
        const char *dir = argv[arg_start + 1];

        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "Cannot create output directory: %s: %s\n",
                    dir, strerror(errno));
            rc = 1;
            goto done;
        }

        int n = export_code(kv, dir, tenant_prefix_str);
        if (n < 0) {
            rc = 1;
        } else {
            printf("Exported %d files\n", n);
        }

    } else if (!strcmp(cmd, "list")) {
        const char *prefix = (arg_start + 1 < argc) ? argv[arg_start + 1] : "";

        char sbuf[4096];
        const char *start = prefixed_key(tenant_prefix_str, prefix,
                                         sbuf, sizeof(sbuf));

        size_t slen = strlen(start);
        char *end = malloc(slen + 2);
        if (!end) { rc = 1; goto done; }
        memcpy(end, start, slen);
        end[slen] = '\x7f';
        end[slen + 1] = '\0';

        kv_range_result_t result;
        if (kv_range(kv, start, end, 10000, &result) != 0) {
            fprintf(stderr, "List failed\n");
            rc = 1;
        } else {
            size_t pfx_len = tenant_prefix_str ? strlen(tenant_prefix_str) : 0;
            for (size_t i = 0; i < result.count; i++) {
                const char *display_key = result.entries[i].key + pfx_len;
                printf("%s\t%zu bytes\n", display_key,
                       result.entries[i].value_len);
            }
            kv_range_free(&result);
        }
        free(end);

    } else if (!strcmp(cmd, "domain-map")) {
        if (arg_start + 2 >= argc) { usage(argv[0]); rc = 1; goto done; }
        const char *hostname  = argv[arg_start + 1];
        const char *tenant_id = argv[arg_start + 2];

        char key[512];
        snprintf(key, sizeof(key), "domains/%s", hostname);

        if (kv_put(kv, key, tenant_id, strlen(tenant_id)) != 0) {
            fprintf(stderr, "Failed to map domain\n");
            rc = 1;
        } else {
            printf("Mapped %s → tenant %s\n", hostname, tenant_id);
        }

    } else if (!strcmp(cmd, "domain-unmap")) {
        if (arg_start + 1 >= argc) { usage(argv[0]); rc = 1; goto done; }
        const char *hostname = argv[arg_start + 1];

        char key[512];
        snprintf(key, sizeof(key), "domains/%s", hostname);

        if (kv_delete(kv, key) != 0) {
            fprintf(stderr, "Failed to unmap domain\n");
            rc = 1;
        } else {
            printf("Unmapped %s\n", hostname);
        }

    } else if (!strcmp(cmd, "cert-put")) {
        if (arg_start + 3 >= argc) { usage(argv[0]); rc = 1; goto done; }
        const char *name      = argv[arg_start + 1];
        const char *cert_path = argv[arg_start + 2];
        const char *key_path  = argv[arg_start + 3];

        size_t cert_len, key_len;
        char *cert_data = read_file(cert_path, &cert_len);
        char *key_data  = read_file(key_path, &key_len);

        if (!cert_data || !key_data) {
            fprintf(stderr, "Cannot read cert/key files\n");
            free(cert_data);
            free(key_data);
            rc = 1;
        } else {
            char ck[512], kk[512];
            snprintf(ck, sizeof(ck), "__certs/%s/cert.pem", name);
            snprintf(kk, sizeof(kk), "__certs/%s/key.pem", name);

            if (kv_put(kv, ck, cert_data, cert_len) != 0 ||
                kv_put(kv, kk, key_data, key_len) != 0) {
                fprintf(stderr, "Failed to store cert/key\n");
                rc = 1;
            } else {
                printf("Stored cert '%s' (%zu + %zu bytes)\n",
                       name, cert_len, key_len);
            }
            free(cert_data);
            free(key_data);
        }

    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        usage(argv[0]);
        rc = 1;
    }

done:
    kv_close(kv);
    return rc;
}
