#pragma once

#include "kvstore.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Dynamically grown buffer for building JSON arrays */
typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
    int     count;   /* number of entries appended (for comma handling) */
} replay_buf_t;

typedef struct sjs_replay_capture {
    replay_buf_t kv_tape;
    replay_buf_t date_tape;
    replay_buf_t math_random_tape;
    replay_buf_t module_tree;
    replay_buf_t source_maps;

    char        *session_json;     /* session data at request start (malloc'd) */
    size_t       session_json_len;
} sjs_replay_capture_t;

void replay_capture_init(sjs_replay_capture_t *cap);
void replay_capture_free(sjs_replay_capture_t *cap);

/* Finalize a JSON array buffer — closes with ']'.
 * Returns the data pointer (caller must free via replay_capture_free).
 * After finalize, the buffer should not be appended to. */
void replay_capture_finalize(replay_buf_t *buf);

/* Append a kv.get result (value=NULL, vlen=0 for miss) */
void replay_capture_kv_get(sjs_replay_capture_t *cap,
                           const char *key, const void *value, size_t vlen);

/* Append a kv.range result */
void replay_capture_kv_range(sjs_replay_capture_t *cap,
                             const char *start, const char *end,
                             const kv_range_result_t *result, size_t prefix_len);

/* Record a Date.now() return value */
void replay_capture_date_now(sjs_replay_capture_t *cap, int64_t ms);

/* Record a Math.random() return value */
void replay_capture_math_random(sjs_replay_capture_t *cap, double value);

/* Record a module load (path + content hash) */
void replay_capture_module(sjs_replay_capture_t *cap,
                           const char *path, const char *content_hash);

/* Record a source map (path + JSON source map) */
void replay_capture_sourcemap(sjs_replay_capture_t *cap,
                              const char *path, const char *sourcemap_json);

/* Record session state at request start */
void replay_capture_session(sjs_replay_capture_t *cap,
                            const char *json, size_t len);
