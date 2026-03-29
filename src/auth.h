#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Auth claims extracted from a signed cookie.
 * Cookie format: base64url(json_payload) "." base64url(hmac_sha256)
 * Payload: {"tid":N,"uid":"...","roles":[...],"exp":N} */

typedef struct {
    int64_t  tid;          /* tenant ID */
    char    *uid;          /* user identifier (malloc'd) */
    char   **roles;        /* array of role strings (malloc'd) */
    size_t   role_count;
    int64_t  exp;          /* expiry as Unix timestamp */
} sjs_auth_claims_t;

/* Sign claims into a cookie value string.
 * Returns malloc'd string "base64url(payload).base64url(sig)", or NULL on error.
 * Caller frees. */
char *sjs_auth_sign(const sjs_auth_claims_t *claims,
                    const char *secret, size_t secret_len);

/* Verify and parse a cookie value.
 * Returns 0 on success (out populated), -1 on invalid signature,
 * expired token, or malformed data. On failure, out is zeroed.
 * Caller must call sjs_auth_claims_free(out) on success. */
int sjs_auth_verify(const char *cookie, size_t cookie_len,
                    const char *secret, size_t secret_len,
                    sjs_auth_claims_t *out);

/* Check if claims include a specific role string. */
bool sjs_auth_has_role(const sjs_auth_claims_t *claims, const char *role);

/* Free all malloc'd strings within claims. */
void sjs_auth_claims_free(sjs_auth_claims_t *claims);
