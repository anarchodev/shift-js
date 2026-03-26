#pragma once

#include "js_runtime.h"
#include <quickjs.h>
#include <stddef.h>
#include <stdint.h>

/* Install the crypto global (crypto.getRandomValues, crypto.randomUUID,
 * crypto.subtle.digest, crypto.subtle.importKey, crypto.subtle.sign,
 * crypto.subtle.verify) into the given JSContext. */
void js_install_crypto(JSContext *ctx);

/* Fill buf with n random bytes. In capture mode, generates fresh random
 * bytes via OpenSSL RAND_bytes and appends them to req->random_tape.
 * In replay mode, reads from the pre-filled tape.
 * Returns 0 on success, -1 on failure. */
int sjs_random_fill(sjs_request_ctx_t *req, uint8_t *buf, size_t n);

/* Free the random tape buffer (call after request completes). */
void sjs_random_tape_free(sjs_request_ctx_t *req);
