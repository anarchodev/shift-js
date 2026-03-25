#include "kvstore.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s -d <db> <command> [args...]\n\n"
        "Commands:\n"
        "  get <key>                   Print value for key\n"
        "  put <key> <value>           Set key to value (string)\n"
        "  putfile <key> <file>        Set key to file contents\n"
        "  delete <key>                Delete key\n"
        "  range <start> <end> [count] List keys in range\n"
        "  upload <dir> [prefix]       Upload directory tree as __code/<prefix>/...\n"
        "  list [prefix]               List all keys with optional prefix\n"
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

static int upload_dir(kvstore_t *kv, const char *dir_path, const char *prefix) {
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
            if (prefix[0])
                snprintf(subprefix, sizeof(subprefix), "%s/%s", prefix, ent->d_name);
            else
                snprintf(subprefix, sizeof(subprefix), "%s", ent->d_name);

            int sub = upload_dir(kv, filepath, subprefix);
            if (sub > 0) count += sub;
            continue;
        }

        if (!S_ISREG(st.st_mode)) continue;

        /* Build KV key: __code/<prefix>/<filename> */
        char key[4096];
        if (prefix[0])
            snprintf(key, sizeof(key), "__code/%s/%s", prefix, ent->d_name);
        else
            snprintf(key, sizeof(key), "__code/%s", ent->d_name);

        size_t flen;
        char *contents = read_file(filepath, &flen);
        if (!contents) {
            fprintf(stderr, "Cannot read %s\n", filepath);
            continue;
        }

        if (kv_put(kv, key, contents, flen) == 0) {
            printf("  %s → %s (%zu bytes)\n", filepath, key, flen);
            count++;
        } else {
            fprintf(stderr, "  FAILED: %s\n", key);
        }

        free(contents);
    }

    closedir(d);
    return count;
}

int main(int argc, char **argv) {
    const char *db_path = NULL;
    int arg_start = 1;

    /* Parse -d <db> */
    if (argc >= 3 && !strcmp(argv[1], "-d")) {
        db_path = argv[2];
        arg_start = 3;
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

        void  *value = NULL;
        size_t vlen  = 0;
        int r = kv_get(kv, argv[arg_start + 1], &value, &vlen);
        if (r == -1) {
            fprintf(stderr, "Not found: %s\n", argv[arg_start + 1]);
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
        const char *key = argv[arg_start + 1];
        const char *val = argv[arg_start + 2];
        if (kv_put(kv, key, val, strlen(val)) != 0) {
            fprintf(stderr, "Failed to put key\n");
            rc = 1;
        }

    } else if (!strcmp(cmd, "putfile")) {
        if (arg_start + 2 >= argc) { usage(argv[0]); rc = 1; goto done; }
        const char *key = argv[arg_start + 1];
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
        if (kv_delete(kv, argv[arg_start + 1]) != 0) {
            fprintf(stderr, "Failed to delete key\n");
            rc = 1;
        }

    } else if (!strcmp(cmd, "range")) {
        if (arg_start + 2 >= argc) { usage(argv[0]); rc = 1; goto done; }
        const char *start = argv[arg_start + 1];
        const char *end   = argv[arg_start + 2];
        size_t count = 100;
        if (arg_start + 3 < argc) count = (size_t)atol(argv[arg_start + 3]);

        kv_range_result_t result;
        if (kv_range(kv, start, end, count, &result) != 0) {
            fprintf(stderr, "Range query failed\n");
            rc = 1;
        } else {
            for (size_t i = 0; i < result.count; i++) {
                printf("%s\t%zu bytes\n", result.entries[i].key,
                       result.entries[i].value_len);
            }
            kv_range_free(&result);
        }

    } else if (!strcmp(cmd, "upload")) {
        if (arg_start + 1 >= argc) { usage(argv[0]); rc = 1; goto done; }
        const char *dir = argv[arg_start + 1];
        const char *prefix = (arg_start + 2 < argc) ? argv[arg_start + 2] : "";

        int n = upload_dir(kv, dir, prefix);
        printf("Uploaded %d files\n", n);

    } else if (!strcmp(cmd, "list")) {
        const char *prefix = (arg_start + 1 < argc) ? argv[arg_start + 1] : "";

        /* Use range scan: prefix to prefix + max char */
        size_t plen = strlen(prefix);
        char *end = malloc(plen + 2);
        if (!end) { rc = 1; goto done; }
        memcpy(end, prefix, plen);
        end[plen] = '\x7f';  /* DEL — sorts after all printable chars */
        end[plen + 1] = '\0';

        kv_range_result_t result;
        if (kv_range(kv, prefix, end, 10000, &result) != 0) {
            fprintf(stderr, "List failed\n");
            rc = 1;
        } else {
            for (size_t i = 0; i < result.count; i++) {
                printf("%s\t%zu bytes\n", result.entries[i].key,
                       result.entries[i].value_len);
            }
            kv_range_free(&result);
        }
        free(end);

    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        usage(argv[0]);
        rc = 1;
    }

done:
    kv_close(kv);
    return rc;
}
