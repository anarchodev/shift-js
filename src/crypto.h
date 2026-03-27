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
 * bytes via OpenSSL RAND_bytes and appends them to tape.
 * In replay mode, reads from the pre-filled tape.
 * Returns 0 on success, -1 on failure. */
int sjs_random_fill(sjs_random_tape_t *tape, uint8_t *buf, size_t n);
