#include "crypto.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <quickjs.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ======================================================================
 * Random byte capture/replay
 * ====================================================================== */

int sjs_random_fill(sjs_random_tape_t *tape, uint8_t *buf, size_t n) {
    if (n == 0) return 0;

    /* Generate fresh random bytes and capture to tape */
    if (RAND_bytes(buf, (int)n) != 1)
        return -1;

    /* Append to tape */
    size_t need = tape->len + n;
    if (need > tape->cap) {
        size_t new_cap = tape->cap ? tape->cap * 2 : 256;
        while (new_cap < need) new_cap *= 2;
        uint8_t *new_buf = realloc(tape->data, new_cap);
        if (!new_buf) return -1;
        tape->data = new_buf;
        tape->cap = new_cap;
    }
    memcpy(tape->data + tape->len, buf, n);
    tape->len += n;
    return 0;
}

/* ======================================================================
 * Helpers
 * ====================================================================== */

static sjs_request_ctx_t *js_get_req_ctx(JSContext *ctx) {
    return JS_GetContextOpaque(ctx);
}

/* Extract raw bytes from a BufferSource (ArrayBuffer or TypedArray).
 * Sets *out_ptr and *out_len. Caller must JS_FreeValue(ctx, *out_ab). */
static int js_get_buffer_source(JSContext *ctx, JSValue val,
                                uint8_t **out_ptr, size_t *out_len,
                                JSValue *out_ab) {
    size_t len;
    uint8_t *ptr;

    /* Try ArrayBuffer first */
    ptr = JS_GetArrayBuffer(ctx, &len, val);
    if (ptr) {
        *out_ptr = ptr;
        *out_len = len;
        *out_ab = JS_DupValue(ctx, val);
        return 0;
    }

    /* Try TypedArray */
    size_t offset, byte_length, bpe;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, val, &offset, &byte_length, &bpe);
    if (JS_IsException(ab)) {
        JS_ThrowTypeError(ctx, "expected BufferSource (ArrayBuffer or TypedArray)");
        return -1;
    }

    ptr = JS_GetArrayBuffer(ctx, &len, ab);
    if (!ptr) {
        JS_FreeValue(ctx, ab);
        JS_ThrowTypeError(ctx, "could not get ArrayBuffer from TypedArray");
        return -1;
    }
    *out_ptr = ptr + offset;
    *out_len = byte_length;
    *out_ab = ab;
    return 0;
}

/* Map algorithm name string to OpenSSL EVP_MD. */
static const EVP_MD *evp_md_from_name(const char *name) {
    if (strcmp(name, "SHA-1") == 0)   return EVP_sha1();
    if (strcmp(name, "SHA-256") == 0) return EVP_sha256();
    if (strcmp(name, "SHA-384") == 0) return EVP_sha384();
    if (strcmp(name, "SHA-512") == 0) return EVP_sha512();
    return NULL;
}

/* Create an already-resolved promise wrapping value. Takes ownership of value. */
static JSValue js_resolved_promise(JSContext *ctx, JSValue value) {
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, value);
        return JS_EXCEPTION;
    }
    JSValue ret = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, &value);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

/* Create an already-rejected promise. err_msg is used to create a TypeError. */
static JSValue js_rejected_promise(JSContext *ctx, const char *err_msg) {
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise))
        return JS_EXCEPTION;
    JSValue err = JS_ThrowTypeError(ctx, "%s", err_msg);
    (void)err;
    JSValue exc = JS_GetException(ctx);
    JSValue ret = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, &exc);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, exc);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

/* ======================================================================
 * crypto.getRandomValues(typedArray)
 * ====================================================================== */

static JSValue js_crypto_getRandomValues(JSContext *ctx, JSValue this_val,
                                         int argc, JSValue *argv) {
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (!req) return JS_ThrowInternalError(ctx, "no request context");

    size_t offset, byte_length, bpe;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &offset, &byte_length, &bpe);
    if (JS_IsException(ab))
        return JS_ThrowTypeError(ctx, "argument must be a TypedArray");

    if (byte_length > 65536) {
        JS_FreeValue(ctx, ab);
        return JS_ThrowRangeError(ctx,
            "getRandomValues: byte length %zu exceeds 65536", byte_length);
    }

    size_t ab_size;
    uint8_t *ptr = JS_GetArrayBuffer(ctx, &ab_size, ab);
    JS_FreeValue(ctx, ab);
    if (!ptr)
        return JS_ThrowInternalError(ctx, "could not access ArrayBuffer");

    if (sjs_random_fill(req->tape, ptr + offset, byte_length) != 0)
        return JS_ThrowInternalError(ctx, "random generation failed");

    return JS_DupValue(ctx, argv[0]);
}

/* ======================================================================
 * crypto.randomUUID()
 * ====================================================================== */

static JSValue js_crypto_randomUUID(JSContext *ctx, JSValue this_val,
                                    int argc, JSValue *argv) {
    sjs_request_ctx_t *req = js_get_req_ctx(ctx);
    if (!req) return JS_ThrowInternalError(ctx, "no request context");

    uint8_t bytes[16];
    if (sjs_random_fill(req->tape, bytes, 16) != 0)
        return JS_ThrowInternalError(ctx, "random generation failed");

    /* UUID v4: set version and variant bits */
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;

    char buf[37];
    static const char hex[] = "0123456789abcdef";
    int pos = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            buf[pos++] = '-';
        buf[pos++] = hex[bytes[i] >> 4];
        buf[pos++] = hex[bytes[i] & 0x0f];
    }
    buf[36] = '\0';

    return JS_NewStringLen(ctx, buf, 36);
}

/* ======================================================================
 * crypto.subtle.digest(algorithm, data)
 * ====================================================================== */

static JSValue js_subtle_digest(JSContext *ctx, JSValue this_val,
                                int argc, JSValue *argv) {
    const char *algo = JS_ToCString(ctx, argv[0]);
    if (!algo) return js_rejected_promise(ctx, "algorithm must be a string");

    const EVP_MD *md = evp_md_from_name(algo);
    JS_FreeCString(ctx, algo);
    if (!md)
        return js_rejected_promise(ctx, "unsupported digest algorithm");

    uint8_t *data_ptr;
    size_t data_len;
    JSValue ab_ref;
    if (js_get_buffer_source(ctx, argv[1], &data_ptr, &data_len, &ab_ref) != 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return js_rejected_promise(ctx, "data must be a BufferSource");
    }

    uint8_t digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len;
    int ok = EVP_Digest(data_ptr, data_len, digest, &digest_len, md, NULL);
    JS_FreeValue(ctx, ab_ref);

    if (!ok)
        return js_rejected_promise(ctx, "digest computation failed");

    JSValue result = JS_NewArrayBufferCopy(ctx, digest, digest_len);
    return js_resolved_promise(ctx, result);
}

/* ======================================================================
 * crypto.subtle.importKey("raw", keyData, algorithm, extractable, usages)
 * ====================================================================== */

static JSValue js_subtle_importKey(JSContext *ctx, JSValue this_val,
                                   int argc, JSValue *argv) {
    /* Validate format = "raw" */
    const char *format = JS_ToCString(ctx, argv[0]);
    if (!format) return js_rejected_promise(ctx, "format must be a string");
    if (strcmp(format, "raw") != 0) {
        JS_FreeCString(ctx, format);
        return js_rejected_promise(ctx, "only \"raw\" format is supported");
    }
    JS_FreeCString(ctx, format);

    /* Extract key data */
    uint8_t *key_ptr;
    size_t key_len;
    JSValue key_ab;
    if (js_get_buffer_source(ctx, argv[1], &key_ptr, &key_len, &key_ab) != 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return js_rejected_promise(ctx, "keyData must be a BufferSource");
    }

    /* Parse algorithm: string or {name, hash} */
    const char *algo_name = NULL;
    const char *hash_name = NULL;

    if (JS_IsString(argv[2])) {
        algo_name = JS_ToCString(ctx, argv[2]);
    } else {
        JSValue name_val = JS_GetPropertyStr(ctx, argv[2], "name");
        algo_name = JS_ToCString(ctx, name_val);
        JS_FreeValue(ctx, name_val);

        JSValue hash_val = JS_GetPropertyStr(ctx, argv[2], "hash");
        if (JS_IsString(hash_val)) {
            hash_name = JS_ToCString(ctx, hash_val);
        } else if (JS_IsObject(hash_val)) {
            JSValue hn = JS_GetPropertyStr(ctx, hash_val, "name");
            hash_name = JS_ToCString(ctx, hn);
            JS_FreeValue(ctx, hn);
        }
        JS_FreeValue(ctx, hash_val);
    }

    if (!algo_name ||
        (strcmp(algo_name, "HMAC") != 0 && strcmp(algo_name, "PBKDF2") != 0)) {
        if (algo_name) JS_FreeCString(ctx, algo_name);
        if (hash_name) JS_FreeCString(ctx, hash_name);
        JS_FreeValue(ctx, key_ab);
        return js_rejected_promise(ctx, "unsupported algorithm (expected HMAC or PBKDF2)");
    }

    bool is_hmac = (strcmp(algo_name, "HMAC") == 0);
    JS_FreeCString(ctx, algo_name);

    /* HMAC requires a hash; PBKDF2 does not (hash is specified in deriveBits) */
    if (is_hmac && (!hash_name || !evp_md_from_name(hash_name))) {
        if (hash_name) JS_FreeCString(ctx, hash_name);
        JS_FreeValue(ctx, key_ab);
        return js_rejected_promise(ctx, "unsupported or missing hash algorithm");
    }

    /* Build CryptoKey as a plain JS object with hidden properties */
    JSValue key_obj = JS_NewObject(ctx);

    JSValue key_data_copy = JS_NewArrayBufferCopy(ctx, key_ptr, key_len);
    JS_FreeValue(ctx, key_ab);

    JS_DefinePropertyValueStr(ctx, key_obj, "__type",
        JS_NewString(ctx, "CryptoKey"), 0);
    JS_DefinePropertyValueStr(ctx, key_obj, "__algorithm",
        JS_NewString(ctx, is_hmac ? "HMAC" : "PBKDF2"), 0);
    JS_DefinePropertyValueStr(ctx, key_obj, "__hash",
        hash_name ? JS_NewString(ctx, hash_name) : JS_NULL, 0);
    JS_DefinePropertyValueStr(ctx, key_obj, "__keyData",
        key_data_copy, 0);
    JS_DefinePropertyValueStr(ctx, key_obj, "__extractable",
        argv[3], 0);
    JS_DefinePropertyValueStr(ctx, key_obj, "__usages",
        JS_DupValue(ctx, argv[4]), 0);

    if (hash_name) JS_FreeCString(ctx, hash_name);

    return js_resolved_promise(ctx, key_obj);
}

/* Helper: extract HMAC key bytes and hash from a CryptoKey JS object. */
static int js_extract_hmac_key(JSContext *ctx, JSValue key_obj,
                               uint8_t **out_key, size_t *out_key_len,
                               const EVP_MD **out_md) {
    JSValue type_val = JS_GetPropertyStr(ctx, key_obj, "__type");
    const char *type = JS_ToCString(ctx, type_val);
    JS_FreeValue(ctx, type_val);
    if (!type || strcmp(type, "CryptoKey") != 0) {
        if (type) JS_FreeCString(ctx, type);
        return -1;
    }
    JS_FreeCString(ctx, type);

    JSValue hash_val = JS_GetPropertyStr(ctx, key_obj, "__hash");
    const char *hash = JS_ToCString(ctx, hash_val);
    JS_FreeValue(ctx, hash_val);
    if (!hash) return -1;

    *out_md = evp_md_from_name(hash);
    JS_FreeCString(ctx, hash);
    if (!*out_md) return -1;

    JSValue kd = JS_GetPropertyStr(ctx, key_obj, "__keyData");
    *out_key = JS_GetArrayBuffer(ctx, out_key_len, kd);
    JS_FreeValue(ctx, kd);
    if (!*out_key) return -1;

    return 0;
}

/* Parse algorithm argument: accepts "HMAC" string or {name: "HMAC"} object. */
static int js_parse_hmac_algo(JSContext *ctx, JSValue algo_val) {
    const char *name = NULL;
    if (JS_IsString(algo_val)) {
        name = JS_ToCString(ctx, algo_val);
    } else if (JS_IsObject(algo_val)) {
        JSValue n = JS_GetPropertyStr(ctx, algo_val, "name");
        name = JS_ToCString(ctx, n);
        JS_FreeValue(ctx, n);
    }
    if (!name) return -1;
    int ok = (strcmp(name, "HMAC") == 0) ? 0 : -1;
    JS_FreeCString(ctx, name);
    return ok;
}

/* ======================================================================
 * crypto.subtle.sign("HMAC", key, data)
 * ====================================================================== */

static JSValue js_subtle_sign(JSContext *ctx, JSValue this_val,
                              int argc, JSValue *argv) {
    if (js_parse_hmac_algo(ctx, argv[0]) != 0)
        return js_rejected_promise(ctx, "only HMAC algorithm is supported");

    uint8_t *key_ptr;
    size_t key_len;
    const EVP_MD *md;
    if (js_extract_hmac_key(ctx, argv[1], &key_ptr, &key_len, &md) != 0)
        return js_rejected_promise(ctx, "invalid CryptoKey");

    uint8_t *data_ptr;
    size_t data_len;
    JSValue ab_ref;
    if (js_get_buffer_source(ctx, argv[2], &data_ptr, &data_len, &ab_ref) != 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return js_rejected_promise(ctx, "data must be a BufferSource");
    }

    uint8_t mac[EVP_MAX_MD_SIZE];
    unsigned int mac_len;
    unsigned char *result = HMAC(md, key_ptr, (int)key_len,
                                 data_ptr, data_len, mac, &mac_len);
    JS_FreeValue(ctx, ab_ref);

    if (!result)
        return js_rejected_promise(ctx, "HMAC computation failed");

    JSValue ab = JS_NewArrayBufferCopy(ctx, mac, mac_len);
    return js_resolved_promise(ctx, ab);
}

/* ======================================================================
 * crypto.subtle.verify("HMAC", key, signature, data)
 * ====================================================================== */

static JSValue js_subtle_verify(JSContext *ctx, JSValue this_val,
                                int argc, JSValue *argv) {
    if (js_parse_hmac_algo(ctx, argv[0]) != 0)
        return js_rejected_promise(ctx, "only HMAC algorithm is supported");

    uint8_t *key_ptr;
    size_t key_len;
    const EVP_MD *md;
    if (js_extract_hmac_key(ctx, argv[1], &key_ptr, &key_len, &md) != 0)
        return js_rejected_promise(ctx, "invalid CryptoKey");

    /* Extract signature */
    uint8_t *sig_ptr;
    size_t sig_len;
    JSValue sig_ab;
    if (js_get_buffer_source(ctx, argv[2], &sig_ptr, &sig_len, &sig_ab) != 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return js_rejected_promise(ctx, "signature must be a BufferSource");
    }

    /* Extract data */
    uint8_t *data_ptr;
    size_t data_len;
    JSValue data_ab;
    if (js_get_buffer_source(ctx, argv[3], &data_ptr, &data_len, &data_ab) != 0) {
        JS_FreeValue(ctx, sig_ab);
        JS_FreeValue(ctx, JS_GetException(ctx));
        return js_rejected_promise(ctx, "data must be a BufferSource");
    }

    /* Compute expected HMAC */
    uint8_t expected[EVP_MAX_MD_SIZE];
    unsigned int expected_len;
    unsigned char *result = HMAC(md, key_ptr, (int)key_len,
                                 data_ptr, data_len, expected, &expected_len);
    JS_FreeValue(ctx, sig_ab);
    JS_FreeValue(ctx, data_ab);

    if (!result)
        return js_rejected_promise(ctx, "HMAC computation failed");

    /* Constant-time comparison */
    bool match = (sig_len == expected_len) &&
                 (CRYPTO_memcmp(sig_ptr, expected, expected_len) == 0);

    return js_resolved_promise(ctx, JS_NewBool(ctx, match));
}

/* ======================================================================
 * crypto.subtle.deriveBits(algorithm, baseKey, length)
 *
 * algorithm = { name: "PBKDF2", salt, iterations, hash }
 * baseKey   = CryptoKey from importKey("raw", password, "PBKDF2", ...)
 * length    = output bits (must be multiple of 8)
 * ====================================================================== */

static JSValue js_subtle_deriveBits(JSContext *ctx, JSValue this_val,
                                    int argc, JSValue *argv) {
    /* Parse algorithm object */
    if (!JS_IsObject(argv[0]))
        return js_rejected_promise(ctx, "algorithm must be an object");

    JSValue name_val = JS_GetPropertyStr(ctx, argv[0], "name");
    const char *name = JS_ToCString(ctx, name_val);
    JS_FreeValue(ctx, name_val);
    if (!name || strcmp(name, "PBKDF2") != 0) {
        if (name) JS_FreeCString(ctx, name);
        return js_rejected_promise(ctx, "only PBKDF2 algorithm is supported for deriveBits");
    }
    JS_FreeCString(ctx, name);

    /* Extract salt */
    JSValue salt_val = JS_GetPropertyStr(ctx, argv[0], "salt");
    uint8_t *salt_ptr;
    size_t salt_len;
    JSValue salt_ab;
    if (js_get_buffer_source(ctx, salt_val, &salt_ptr, &salt_len, &salt_ab) != 0) {
        JS_FreeValue(ctx, salt_val);
        JS_FreeValue(ctx, JS_GetException(ctx));
        return js_rejected_promise(ctx, "salt must be a BufferSource");
    }
    JS_FreeValue(ctx, salt_val);

    /* Extract iterations */
    JSValue iter_val = JS_GetPropertyStr(ctx, argv[0], "iterations");
    int64_t iterations;
    if (JS_ToInt64(ctx, &iterations, iter_val)) {
        JS_FreeValue(ctx, iter_val);
        JS_FreeValue(ctx, salt_ab);
        return js_rejected_promise(ctx, "iterations must be a number");
    }
    JS_FreeValue(ctx, iter_val);
    if (iterations <= 0) {
        JS_FreeValue(ctx, salt_ab);
        return js_rejected_promise(ctx, "iterations must be positive");
    }

    /* Extract hash algorithm */
    JSValue hash_val = JS_GetPropertyStr(ctx, argv[0], "hash");
    const char *hash_str = NULL;
    if (JS_IsString(hash_val)) {
        hash_str = JS_ToCString(ctx, hash_val);
    } else if (JS_IsObject(hash_val)) {
        JSValue hn = JS_GetPropertyStr(ctx, hash_val, "name");
        hash_str = JS_ToCString(ctx, hn);
        JS_FreeValue(ctx, hn);
    }
    JS_FreeValue(ctx, hash_val);

    const EVP_MD *md = hash_str ? evp_md_from_name(hash_str) : NULL;
    if (hash_str) JS_FreeCString(ctx, hash_str);
    if (!md) {
        JS_FreeValue(ctx, salt_ab);
        return js_rejected_promise(ctx, "unsupported hash algorithm for PBKDF2");
    }

    /* Extract base key data */
    JSValue algo_val = JS_GetPropertyStr(ctx, argv[1], "__algorithm");
    const char *key_algo = JS_ToCString(ctx, algo_val);
    JS_FreeValue(ctx, algo_val);
    if (!key_algo || strcmp(key_algo, "PBKDF2") != 0) {
        if (key_algo) JS_FreeCString(ctx, key_algo);
        JS_FreeValue(ctx, salt_ab);
        return js_rejected_promise(ctx, "key must be a PBKDF2 CryptoKey");
    }
    JS_FreeCString(ctx, key_algo);

    JSValue kd = JS_GetPropertyStr(ctx, argv[1], "__keyData");
    size_t pass_len;
    uint8_t *pass_ptr = JS_GetArrayBuffer(ctx, &pass_len, kd);
    JS_FreeValue(ctx, kd);
    if (!pass_ptr) {
        JS_FreeValue(ctx, salt_ab);
        return js_rejected_promise(ctx, "invalid key data");
    }

    /* Extract output length in bits */
    int64_t length_bits;
    if (JS_ToInt64(ctx, &length_bits, argv[2])) {
        JS_FreeValue(ctx, salt_ab);
        return js_rejected_promise(ctx, "length must be a number");
    }
    if (length_bits <= 0 || length_bits % 8 != 0) {
        JS_FreeValue(ctx, salt_ab);
        return js_rejected_promise(ctx, "length must be a positive multiple of 8");
    }
    size_t out_bytes = (size_t)(length_bits / 8);
    if (out_bytes > 4096) {
        JS_FreeValue(ctx, salt_ab);
        return js_rejected_promise(ctx, "length too large (max 32768 bits)");
    }

    /* Derive key bytes */
    uint8_t *out = alloca(out_bytes);
    int ok = PKCS5_PBKDF2_HMAC((const char *)pass_ptr, (int)pass_len,
                                salt_ptr, (int)salt_len,
                                (int)iterations, md,
                                (int)out_bytes, out);
    JS_FreeValue(ctx, salt_ab);

    if (!ok)
        return js_rejected_promise(ctx, "PBKDF2 derivation failed");

    JSValue result = JS_NewArrayBufferCopy(ctx, out, out_bytes);
    return js_resolved_promise(ctx, result);
}

/* ======================================================================
 * Global installation
 * ====================================================================== */

void js_install_crypto(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue crypto = JS_NewObject(ctx);
    JSValue subtle = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, crypto, "getRandomValues",
        JS_NewCFunction(ctx, js_crypto_getRandomValues, "getRandomValues", 1));
    JS_SetPropertyStr(ctx, crypto, "randomUUID",
        JS_NewCFunction(ctx, js_crypto_randomUUID, "randomUUID", 0));

    JS_SetPropertyStr(ctx, subtle, "digest",
        JS_NewCFunction(ctx, js_subtle_digest, "digest", 2));
    JS_SetPropertyStr(ctx, subtle, "importKey",
        JS_NewCFunction(ctx, js_subtle_importKey, "importKey", 5));
    JS_SetPropertyStr(ctx, subtle, "sign",
        JS_NewCFunction(ctx, js_subtle_sign, "sign", 3));
    JS_SetPropertyStr(ctx, subtle, "verify",
        JS_NewCFunction(ctx, js_subtle_verify, "verify", 4));
    JS_SetPropertyStr(ctx, subtle, "deriveBits",
        JS_NewCFunction(ctx, js_subtle_deriveBits, "deriveBits", 3));

    JS_SetPropertyStr(ctx, crypto, "subtle", subtle);
    JS_SetPropertyStr(ctx, global, "crypto", crypto);
    JS_FreeValue(ctx, global);
}
