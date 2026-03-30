#include "session.h"
#include "crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *sjs_session_generate_id(char *buf, sjs_request_ctx_t *req) {
    uint8_t bytes[16];
    if (sjs_random_fill(req->tape, bytes, 16) != 0)
        return NULL;

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        buf[i * 2]     = hex[bytes[i] >> 4];
        buf[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    buf[SJS_SESSION_ID_LEN] = '\0';
    return buf;
}

char *sjs_session_parse_cookie(const char *cookie_header, size_t header_len) {
    if (!cookie_header || header_len == 0) return NULL;

    const char *name = SJS_SESSION_COOKIE;
    size_t name_len = strlen(name);

    const char *p = cookie_header;
    const char *end = cookie_header + header_len;

    while (p < end) {
        /* Skip whitespace and semicolons */
        while (p < end && (*p == ' ' || *p == ';' || *p == '\t'))
            p++;
        if (p >= end) break;

        /* Check if this cookie matches our name */
        if ((size_t)(end - p) > name_len && p[name_len] == '=' &&
            memcmp(p, name, name_len) == 0) {
            const char *val_start = p + name_len + 1;
            const char *val_end = val_start;
            while (val_end < end && *val_end != ';' && *val_end != ' ')
                val_end++;

            size_t val_len = (size_t)(val_end - val_start);
            if (val_len == SJS_SESSION_ID_LEN) {
                /* Validate hex characters */
                bool valid = true;
                for (size_t i = 0; i < val_len; i++) {
                    char c = val_start[i];
                    if (!((c >= '0' && c <= '9') ||
                          (c >= 'a' && c <= 'f') ||
                          (c >= 'A' && c <= 'F'))) {
                        valid = false;
                        break;
                    }
                }
                if (valid) {
                    char *id = malloc(val_len + 1);
                    if (!id) return NULL;
                    memcpy(id, val_start, val_len);
                    id[val_len] = '\0';
                    return id;
                }
            }
        }

        /* Skip to next cookie */
        while (p < end && *p != ';')
            p++;
    }

    return NULL;
}

char *sjs_session_cookie_header(const char *session_id) {
    char *buf = NULL;
    if (asprintf(&buf, "%s=%s; Path=/; HttpOnly; SameSite=Lax",
                 SJS_SESSION_COOKIE, session_id) < 0)
        return NULL;
    return buf;
}
