#pragma once

#include "code_db.h"
#include <stddef.h>

/* Push result for a single request server. */
typedef struct {
    char *server;       /* "host:port" */
    int   status;       /* 0 = success, -1 = connection error, >0 = HTTP error */
    char *error;        /* error message or NULL on success */
} push_result_t;

/* Push a compiled tree's bytecodes to one or more request servers.
 * For each server, sends all tree_modules as POST /_mgmt/tree/<hash>/<path>
 * then POST /_mgmt/tree/<hash>/activate.
 *
 * Returns malloc'd array of results, one per server. Caller frees. */
push_result_t *push_tree(code_db_t *db, const char *tree_hash,
                          const char **servers, size_t server_count,
                          const char *mgmt_secret, size_t mgmt_secret_len);

void push_result_free(push_result_t *r);
