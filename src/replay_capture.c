#include "replay_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

/* ---- Buffer management ---- */

static void buf_init(replay_buf_t *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->count = 0;
}

static void buf_ensure(replay_buf_t *b, size_t need) {
    size_t required = b->len + need;
    if (required <= b->cap) return;
    size_t new_cap = b->cap ? b->cap * 2 : 256;
    while (new_cap < required) new_cap *= 2;
    b->data = realloc(b->data, new_cap);
    b->cap = new_cap;
}

static void buf_append(replay_buf_t *b, const char *s, size_t slen) {
    buf_ensure(b, slen);
    memcpy(b->data + b->len, s, slen);
    b->len += slen;
}

static void buf_append_str(replay_buf_t *b, const char *s) {
    buf_append(b, s, strlen(s));
}

static void buf_start_array(replay_buf_t *b) {
    buf_append(b, "[", 1);
}

static void buf_comma(replay_buf_t *b) {
    if (b->count > 0)
        buf_append(b, ",", 1);
    b->count++;
}

/* Append a JSON-escaped string (with surrounding quotes) */
static void buf_append_json_string(replay_buf_t *b, const char *s, size_t slen) {
    buf_append(b, "\"", 1);
    for (size_t i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  buf_append(b, "\\\"", 2); break;
        case '\\': buf_append(b, "\\\\", 2); break;
        case '\n': buf_append(b, "\\n", 2); break;
        case '\r': buf_append(b, "\\r", 2); break;
        case '\t': buf_append(b, "\\t", 2); break;
        default:
            if (c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", c);
                buf_append(b, esc, 6);
            } else {
                buf_append(b, (const char *)&c, 1);
            }
        }
    }
    buf_append(b, "\"", 1);
}

/* ---- Public API ---- */

void replay_capture_init(sjs_replay_capture_t *cap) {
    memset(cap, 0, sizeof(*cap));
    buf_init(&cap->kv_tape);
    buf_init(&cap->date_tape);
    buf_init(&cap->math_random_tape);
    buf_init(&cap->module_tree);

    buf_start_array(&cap->kv_tape);
    buf_start_array(&cap->date_tape);
    buf_start_array(&cap->math_random_tape);
    buf_start_array(&cap->module_tree);
}

void replay_capture_free(sjs_replay_capture_t *cap) {
    free(cap->kv_tape.data);
    free(cap->date_tape.data);
    free(cap->math_random_tape.data);
    free(cap->module_tree.data);
    free(cap->session_json);
    memset(cap, 0, sizeof(*cap));
}

void replay_capture_finalize(replay_buf_t *buf) {
    buf_append(buf, "]", 1);
    /* NUL-terminate for convenience */
    buf_ensure(buf, 1);
    buf->data[buf->len] = '\0';
}

void replay_capture_kv_get(sjs_replay_capture_t *cap,
                           const char *key, const void *value, size_t vlen) {
    if (!cap) return;
    replay_buf_t *b = &cap->kv_tape;
    buf_comma(b);
    buf_append_str(b, "{\"op\":\"get\",\"key\":");
    buf_append_json_string(b, key, strlen(key));
    buf_append_str(b, ",\"value\":");
    if (value && vlen > 0)
        buf_append_json_string(b, value, vlen);
    else
        buf_append_str(b, "null");
    buf_append(b, "}", 1);
}

void replay_capture_kv_range(sjs_replay_capture_t *cap,
                             const char *start, const char *end,
                             const kv_range_result_t *result,
                             size_t prefix_len) {
    if (!cap) return;
    replay_buf_t *b = &cap->kv_tape;
    buf_comma(b);
    buf_append_str(b, "{\"op\":\"range\",\"start\":");
    buf_append_json_string(b, start, strlen(start));
    buf_append_str(b, ",\"end\":");
    buf_append_json_string(b, end, strlen(end));
    buf_append_str(b, ",\"results\":[");
    for (size_t i = 0; i < result->count; i++) {
        if (i > 0) buf_append(b, ",", 1);
        buf_append_str(b, "{\"key\":");
        const char *visible_key = result->entries[i].key + prefix_len;
        buf_append_json_string(b, visible_key, strlen(visible_key));
        buf_append_str(b, ",\"value\":");
        buf_append_json_string(b, result->entries[i].value,
                               result->entries[i].value_len);
        buf_append(b, "}", 1);
    }
    buf_append_str(b, "]}");
}

void replay_capture_date_now(sjs_replay_capture_t *cap, int64_t ms) {
    if (!cap) return;
    replay_buf_t *b = &cap->date_tape;
    buf_comma(b);
    char num[24];
    int n = snprintf(num, sizeof(num), "%" PRId64, ms);
    buf_append(b, num, (size_t)n);
}

void replay_capture_math_random(sjs_replay_capture_t *cap, double value) {
    if (!cap) return;
    replay_buf_t *b = &cap->math_random_tape;
    buf_comma(b);
    char num[32];
    int n = snprintf(num, sizeof(num), "%.17g", value);
    buf_append(b, num, (size_t)n);
}

void replay_capture_module(sjs_replay_capture_t *cap,
                           const char *path, const char *content_hash) {
    if (!cap) return;
    replay_buf_t *b = &cap->module_tree;
    buf_comma(b);
    buf_append_str(b, "{\"path\":");
    buf_append_json_string(b, path, strlen(path));
    buf_append_str(b, ",\"content_hash\":");
    if (content_hash)
        buf_append_json_string(b, content_hash, strlen(content_hash));
    else
        buf_append_str(b, "null");
    buf_append(b, "}", 1);
}

void replay_capture_session(sjs_replay_capture_t *cap,
                            const char *json, size_t len) {
    if (!cap) return;
    free(cap->session_json);
    cap->session_json = strndup(json, len);
    cap->session_json_len = len;
}
