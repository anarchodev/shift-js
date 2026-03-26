#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Forward-declare to avoid circular include with js_runtime.h */
typedef struct sjs_request_ctx sjs_request_ctx_t;

/* Session cookie name */
#define SJS_SESSION_COOKIE "_sjs_sid"

/* Session ID length (hex-encoded 16 bytes = 32 chars) */
#define SJS_SESSION_ID_LEN 32

/* Generate a cryptographically random session ID.
 * buf must be at least SJS_SESSION_ID_LEN + 1 bytes.
 * Random bytes are routed through req's random tape for deterministic replay.
 * Returns buf on success, NULL on failure. */
char *sjs_session_generate_id(char *buf, sjs_request_ctx_t *req);

/* Extract session ID from a Cookie header value.
 * Returns a malloc'd string with the session ID, or NULL if not found. */
char *sjs_session_parse_cookie(const char *cookie_header, size_t header_len);

/* Build a Set-Cookie header value for the given session ID.
 * Returns a malloc'd string. */
char *sjs_session_cookie_header(const char *session_id);
