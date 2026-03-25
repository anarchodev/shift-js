#include "ejs.h"

#include <stdlib.h>
#include <string.h>

/* ======================================================================
 * Growable output buffer
 * ====================================================================== */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} ejs_buf_t;

static void buf_init(ejs_buf_t *b) {
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

static int buf_ensure(ejs_buf_t *b, size_t need) {
    if (b->len + need <= b->cap) return 0;
    size_t newcap = (b->cap + need) * 2;
    if (newcap < 256) newcap = 256;
    char *p = realloc(b->data, newcap);
    if (!p) return -1;
    b->data = p;
    b->cap  = newcap;
    return 0;
}

static int buf_append(ejs_buf_t *b, const char *s, size_t n) {
    if (buf_ensure(b, n) != 0) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    return 0;
}

static int buf_str(ejs_buf_t *b, const char *s) {
    return buf_append(b, s, strlen(s));
}

/* Append text as a JS string literal body (no surrounding quotes).
 * Escapes \, ", \n, \r, \t. */
static int buf_js_escaped(ejs_buf_t *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        switch (c) {
        case '\\': if (buf_str(b, "\\\\") != 0) return -1; break;
        case '"':  if (buf_str(b, "\\\"") != 0) return -1; break;
        case '\n': if (buf_str(b, "\\n")  != 0) return -1; break;
        case '\r': if (buf_str(b, "\\r")  != 0) return -1; break;
        case '\t': if (buf_str(b, "\\t")  != 0) return -1; break;
        default:
            if (buf_ensure(b, 1) != 0) return -1;
            b->data[b->len++] = c;
            break;
        }
    }
    return 0;
}

/* ======================================================================
 * EJS scanner
 * ====================================================================== */

static const char PREAMBLE[] =
    "function __esc(s){return String(s).replace(/&/g,'&amp;')"
    ".replace(/</g,'&lt;').replace(/>/g,'&gt;')"
    ".replace(/\"/g,'&quot;').replace(/'/g,'&#39;');}\n"
    "function __render(){\n"
    "let __out=\"\";\n";

static const char *METHODS[] = {
    "get", "post", "put", "patch", "destroy", "head", "options"
};
#define N_METHODS (sizeof(METHODS) / sizeof(METHODS[0]))

char *sjs_ejs_transform(const char *source, size_t len, size_t *out_len) {
    ejs_buf_t buf;
    buf_init(&buf);

    if (buf_str(&buf, PREAMBLE) != 0) goto fail;

    const char *p   = source;
    const char *end = source + len;

    while (p < end) {
        /* Scan for next <% */
        const char *tag = p;
        while (tag + 1 < end && !(tag[0] == '<' && tag[1] == '%'))
            tag++;

        /* Did we actually find <% ? */
        bool found_tag = (tag + 1 < end && tag[0] == '<' && tag[1] == '%');

        /* Emit text before the tag (or all remaining text if no tag) */
        const char *text_end = found_tag ? tag : end;
        if (text_end > p) {
            if (buf_str(&buf, "__out+=\"") != 0) goto fail;
            if (buf_js_escaped(&buf, p, (size_t)(text_end - p)) != 0) goto fail;
            if (buf_str(&buf, "\";\n") != 0) goto fail;
        }

        if (!found_tag) break;

        /* Skip <% */
        const char *code = tag + 2;

        /* Find closing %> */
        const char *close = code;
        while (close + 1 < end && !(close[0] == '%' && close[1] == '>'))
            close++;

        bool found_close = (close + 1 < end && close[0] == '%' && close[1] == '>');
        if (!found_close) {
            /* Unclosed tag — emit remaining as text */
            if (buf_str(&buf, "__out+=\"") != 0) goto fail;
            if (buf_js_escaped(&buf, tag, (size_t)(end - tag)) != 0) goto fail;
            if (buf_str(&buf, "\";\n") != 0) goto fail;
            break;
        }

        /* Determine tag type from first char after <% */
        if (code < close && code[0] == '=') {
            /* <%= expr %> — escaped output */
            code++;
            if (buf_str(&buf, "__out+=__esc(") != 0) goto fail;
            if (buf_append(&buf, code, (size_t)(close - code)) != 0) goto fail;
            if (buf_str(&buf, ");\n") != 0) goto fail;
        } else if (code < close && code[0] == '-') {
            /* <%- expr %> — raw output */
            code++;
            if (buf_str(&buf, "__out+=(") != 0) goto fail;
            if (buf_append(&buf, code, (size_t)(close - code)) != 0) goto fail;
            if (buf_str(&buf, ");\n") != 0) goto fail;
        } else if (code < close && code[0] == '#') {
            /* <%# comment %> — skip */
        } else {
            /* <% code %> — verbatim JS */
            if (buf_append(&buf, code, (size_t)(close - code)) != 0) goto fail;
            if (buf_str(&buf, "\n") != 0) goto fail;
        }

        p = close + 2;  /* skip %> */
    }

    /* Close __render and emit method exports */
    if (buf_str(&buf, "return __out;\n}\n") != 0) goto fail;

    for (size_t i = 0; i < N_METHODS; i++) {
        if (buf_str(&buf, "export function ") != 0) goto fail;
        if (buf_str(&buf, METHODS[i]) != 0) goto fail;
        if (buf_str(&buf, "(){response.header(\"content-type\","
                          "\"text/html\");return __render();}\n") != 0)
            goto fail;
    }

    /* Null-terminate for convenience (not counted in out_len) */
    if (buf_ensure(&buf, 1) != 0) goto fail;
    buf.data[buf.len] = '\0';

    *out_len = buf.len;
    return buf.data;

fail:
    free(buf.data);
    return NULL;
}
