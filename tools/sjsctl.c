#include "kvstore.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

/* Prepend tenant prefix to a key. Returns key as-is if prefix is NULL. */
static const char *prefixed_key(const char *prefix, const char *key,
                                char *buf, size_t bufsize);

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

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s -d <db> [-t <tenant_id>] <command> [args...]\n\n"
        "Commands:\n"
        "  get <key>                   Print value for key\n"
        "  put <key> <value>           Set key to value (string)\n"
        "  putfile <key> <file>        Set key to file contents\n"
        "  delete <key>                Delete key\n"
        "  range <start> <end> [count] List keys in range\n"
        "  upload <dir> [prefix]       Upload directory tree as __code/<prefix>/...\n"
        "  import <dir> [prefix]       Alias for upload\n"
        "  export <dir>                Export all __code/ entries to directory\n"
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

        /* Build KV key: [tenant_prefix]__code/[code_prefix/]<filename> */
        char raw_key[4096];
        if (code_prefix[0])
            snprintf(raw_key, sizeof(raw_key), "__code/%s/%s",
                     code_prefix, ent->d_name);
        else
            snprintf(raw_key, sizeof(raw_key), "__code/%s", ent->d_name);

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

        int n = upload_dir(kv, dir, prefix, tenant_prefix_str);
        printf("Imported %d files\n", n);

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
