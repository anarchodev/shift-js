#include "auth.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ======================================================================
 * Base64url encoding/decoding (RFC 4648 §5, no padding)
 * ====================================================================== */

static const char B64URL[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static char *base64url_encode(const void *data, size_t len, size_t *out_len) {
    size_t olen = ((len + 2) / 3) * 4;
    char *out = malloc(olen + 1);
    if (!out) return NULL;

    const unsigned char *in = data;
    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned int v = (unsigned int)in[i] << 16;
        if (i + 1 < len) v |= (unsigned int)in[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned int)in[i + 2];
        out[j++] = B64URL[(v >> 18) & 0x3F];
        out[j++] = B64URL[(v >> 12) & 0x3F];
        out[j++] = (i + 1 < len) ? B64URL[(v >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < len) ? B64URL[(v)      & 0x3F] : '=';
    }
    /* Strip padding */
    while (j > 0 && out[j - 1] == '=') j--;
    out[j] = '\0';
    if (out_len) *out_len = j;
    return out;
}

static int b64url_char(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

static unsigned char *base64url_decode(const char *in, size_t in_len,
                                        size_t *out_len) {
    /* Pad to multiple of 4 */
    size_t padded = in_len;
    while (padded % 4) padded++;

    unsigned char *out = malloc((padded / 4) * 3);
    if (!out) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < padded; i += 4) {
        int a = (i     < in_len) ? b64url_char((unsigned char)in[i])     : 0;
        int b = (i + 1 < in_len) ? b64url_char((unsigned char)in[i + 1]) : 0;
        int c = (i + 2 < in_len) ? b64url_char((unsigned char)in[i + 2]) : -1;
        int d = (i + 3 < in_len) ? b64url_char((unsigned char)in[i + 3]) : -1;
        if (a < 0 || b < 0) { free(out); return NULL; }

        unsigned int v = ((unsigned int)a << 18) | ((unsigned int)b << 12);
        out[j++] = (unsigned char)(v >> 16);
        if (c >= 0) {
            v |= (unsigned int)c << 6;
            out[j++] = (unsigned char)((v >> 8) & 0xFF);
        }
        if (d >= 0) {
            v |= (unsigned int)d;
            out[j++] = (unsigned char)(v & 0xFF);
        }
    }
    *out_len = j;
    return out;
}

/* ======================================================================
 * Minimal JSON helpers (write-only for sign, simple parse for verify)
 * ====================================================================== */

/* Build JSON payload string from claims. Returns malloc'd string. */
static char *claims_to_json(const sjs_auth_claims_t *claims, size_t *out_len) {
    /* Estimate size */
    size_t cap = 128;
    if (claims->uid) cap += strlen(claims->uid) * 2;
    for (size_t i = 0; i < claims->role_count; i++)
        cap += strlen(claims->roles[i]) * 2 + 4;

    char *buf = malloc(cap);
    if (!buf) return NULL;

    int n = snprintf(buf, cap, "{\"tid\":%lld,\"uid\":\"",
                     (long long)claims->tid);

    /* Append uid (escaped) */
    size_t pos = (size_t)n;
    if (claims->uid) {
        for (const char *p = claims->uid; *p; p++) {
            if (*p == '"' || *p == '\\') buf[pos++] = '\\';
            buf[pos++] = *p;
        }
    }

    pos += (size_t)snprintf(buf + pos, cap - pos, "\",\"roles\":[");

    for (size_t i = 0; i < claims->role_count; i++) {
        if (i > 0) buf[pos++] = ',';
        buf[pos++] = '"';
        for (const char *p = claims->roles[i]; *p; p++) {
            if (*p == '"' || *p == '\\') buf[pos++] = '\\';
            buf[pos++] = *p;
        }
        buf[pos++] = '"';
    }

    pos += (size_t)snprintf(buf + pos, cap - pos, "],\"exp\":%lld}",
                            (long long)claims->exp);
    buf[pos] = '\0';
    if (out_len) *out_len = pos;
    return buf;
}

/* Minimal JSON parser for claims. Expects well-formed output from claims_to_json. */
static int json_to_claims(const char *json, size_t len, sjs_auth_claims_t *out) {
    memset(out, 0, sizeof(*out));

    /* Find "tid": */
    const char *p = strstr(json, "\"tid\":");
    if (!p) return -1;
    out->tid = strtoll(p + 6, NULL, 10);

    /* Find "uid":"..." */
    p = strstr(json, "\"uid\":\"");
    if (!p) return -1;
    p += 7;
    const char *end = strchr(p, '"');
    /* Handle escaped quotes */
    while (end && end > p && *(end - 1) == '\\')
        end = strchr(end + 1, '"');
    if (!end) return -1;
    out->uid = strndup(p, (size_t)(end - p));

    /* Find "roles":[...] */
    p = strstr(json, "\"roles\":[");
    if (!p) return -1;
    p += 9;
    end = strchr(p, ']');
    if (!end) return -1;

    /* Count roles */
    size_t count = 0;
    for (const char *s = p; s < end; s++)
        if (*s == '"') { count++; s = strchr(s + 1, '"'); if (!s || s >= end) break; }
    count /= 2; /* each role has open+close quote */

    out->roles = calloc(count ? count : 1, sizeof(char *));
    out->role_count = 0;

    const char *s = p;
    while (s < end) {
        const char *q1 = strchr(s, '"');
        if (!q1 || q1 >= end) break;
        const char *q2 = strchr(q1 + 1, '"');
        if (!q2 || q2 > end) break;
        out->roles[out->role_count++] = strndup(q1 + 1, (size_t)(q2 - q1 - 1));
        s = q2 + 1;
    }

    /* Find "exp": */
    p = strstr(json, "\"exp\":");
    if (!p) return -1;
    out->exp = strtoll(p + 6, NULL, 10);

    return 0;
}

/* ======================================================================
 * Public API
 * ====================================================================== */

char *sjs_auth_sign(const sjs_auth_claims_t *claims,
                    const char *secret, size_t secret_len) {
    if (!claims || !secret) return NULL;

    /* Build JSON payload */
    size_t json_len;
    char *json = claims_to_json(claims, &json_len);
    if (!json) return NULL;

    /* Base64url-encode the payload */
    size_t payload_b64_len;
    char *payload_b64 = base64url_encode(json, json_len, &payload_b64_len);
    free(json);
    if (!payload_b64) return NULL;

    /* HMAC-SHA256 the base64url payload */
    unsigned char sig[32];
    unsigned int sig_len = 0;
    HMAC(EVP_sha256(), secret, (int)secret_len,
         (unsigned char *)payload_b64, payload_b64_len,
         sig, &sig_len);

    /* Base64url-encode the signature */
    size_t sig_b64_len;
    char *sig_b64 = base64url_encode(sig, sig_len, &sig_b64_len);
    if (!sig_b64) { free(payload_b64); return NULL; }

    /* Combine: payload.signature */
    size_t total = payload_b64_len + 1 + sig_b64_len;
    char *result = malloc(total + 1);
    if (!result) { free(payload_b64); free(sig_b64); return NULL; }

    memcpy(result, payload_b64, payload_b64_len);
    result[payload_b64_len] = '.';
    memcpy(result + payload_b64_len + 1, sig_b64, sig_b64_len);
    result[total] = '\0';

    free(payload_b64);
    free(sig_b64);
    return result;
}

int sjs_auth_verify(const char *cookie, size_t cookie_len,
                    const char *secret, size_t secret_len,
                    sjs_auth_claims_t *out) {
    memset(out, 0, sizeof(*out));
    if (!cookie || !secret || cookie_len == 0) return -1;

    /* Find the dot separator */
    const char *dot = memchr(cookie, '.', cookie_len);
    if (!dot) return -1;

    size_t payload_b64_len = (size_t)(dot - cookie);
    size_t sig_b64_len = cookie_len - payload_b64_len - 1;
    const char *sig_b64 = dot + 1;

    if (payload_b64_len == 0 || sig_b64_len == 0) return -1;

    /* Recompute HMAC over the payload portion */
    unsigned char expected_sig[32];
    unsigned int expected_len = 0;
    HMAC(EVP_sha256(), secret, (int)secret_len,
         (unsigned char *)cookie, payload_b64_len,
         expected_sig, &expected_len);

    /* Base64url-encode the expected signature for comparison */
    size_t expected_b64_len;
    char *expected_b64 = base64url_encode(expected_sig, expected_len,
                                           &expected_b64_len);
    if (!expected_b64) return -1;

    /* Constant-time comparison */
    int valid = (expected_b64_len == sig_b64_len);
    if (valid) {
        volatile unsigned char diff = 0;
        for (size_t i = 0; i < sig_b64_len; i++)
            diff |= (unsigned char)sig_b64[i] ^ (unsigned char)expected_b64[i];
        valid = (diff == 0);
    }
    free(expected_b64);

    if (!valid) return -1;

    /* Decode the payload */
    size_t json_len;
    unsigned char *json = base64url_decode(cookie, payload_b64_len, &json_len);
    if (!json) return -1;

    /* Parse claims */
    if (json_to_claims((const char *)json, json_len, out) != 0) {
        free(json);
        return -1;
    }
    free(json);

    /* Check expiry */
    time_t now = time(NULL);
    if (out->exp > 0 && (int64_t)now >= out->exp) {
        sjs_auth_claims_free(out);
        return -1;
    }

    return 0;
}

bool sjs_auth_has_role(const sjs_auth_claims_t *claims, const char *role) {
    if (!claims || !role) return false;
    for (size_t i = 0; i < claims->role_count; i++) {
        if (strcmp(claims->roles[i], role) == 0)
            return true;
    }
    return false;
}

void sjs_auth_claims_free(sjs_auth_claims_t *claims) {
    if (!claims) return;
    free(claims->uid);
    for (size_t i = 0; i < claims->role_count; i++)
        free(claims->roles[i]);
    free(claims->roles);
    memset(claims, 0, sizeof(*claims));
}
