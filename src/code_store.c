#include "code_store.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Simple open-addressing hash map with linear probing. */

typedef struct {
    char   *path;       /* malloc'd key */
    void   *bytecode;   /* malloc'd blob */
    size_t  bytecode_len;
    char    source_hash[41];
    int     is_static;  /* 1 = static file, 0 = bytecode module */
} store_entry_t;

typedef struct store_table {
    store_entry_t *entries;
    size_t         capacity;
    size_t         count;
    char           tree_hash[41];
} store_table_t;

struct code_store {
    _Atomic(store_table_t *) current;   /* read path (lock-free) */
    store_table_t           *building;  /* write path (under lock) */
    pthread_mutex_t          lock;
};

static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static store_table_t *table_create(size_t capacity) {
    store_table_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->capacity = capacity < 16 ? 16 : capacity;
    t->entries = calloc(t->capacity, sizeof(store_entry_t));
    if (!t->entries) { free(t); return NULL; }
    return t;
}

static void table_destroy(store_table_t *t) {
    if (!t) return;
    for (size_t i = 0; i < t->capacity; i++) {
        free(t->entries[i].path);
        free(t->entries[i].bytecode);
    }
    free(t->entries);
    free(t);
}

static store_entry_t *table_find(store_table_t *t, const char *path) {
    if (!t || t->count == 0) return NULL;
    uint32_t idx = fnv1a(path) & (uint32_t)(t->capacity - 1);
    for (size_t i = 0; i < t->capacity; i++) {
        store_entry_t *e = &t->entries[idx];
        if (!e->path) return NULL;
        if (strcmp(e->path, path) == 0) return e;
        idx = (idx + 1) & (uint32_t)(t->capacity - 1);
    }
    return NULL;
}

static void table_insert(store_table_t *t, const char *path,
                          const void *data, size_t len,
                          const char *source_hash, int is_static) {
    /* Grow if > 70% full */
    if (t->count * 10 > t->capacity * 7) {
        size_t new_cap = t->capacity * 2;
        store_entry_t *new_entries = calloc(new_cap, sizeof(store_entry_t));
        if (!new_entries) return;
        for (size_t i = 0; i < t->capacity; i++) {
            store_entry_t *old = &t->entries[i];
            if (!old->path) continue;
            uint32_t idx = fnv1a(old->path) & (uint32_t)(new_cap - 1);
            while (new_entries[idx].path)
                idx = (idx + 1) & (uint32_t)(new_cap - 1);
            new_entries[idx] = *old;
        }
        free(t->entries);
        t->entries = new_entries;
        t->capacity = new_cap;
    }

    uint32_t idx = fnv1a(path) & (uint32_t)(t->capacity - 1);
    while (t->entries[idx].path) {
        if (strcmp(t->entries[idx].path, path) == 0) {
            /* Overwrite existing */
            free(t->entries[idx].bytecode);
            t->entries[idx].bytecode = malloc(len);
            memcpy(t->entries[idx].bytecode, data, len);
            t->entries[idx].bytecode_len = len;
            t->entries[idx].is_static = is_static;
            if (source_hash)
                memcpy(t->entries[idx].source_hash, source_hash, 41);
            else
                t->entries[idx].source_hash[0] = '\0';
            return;
        }
        idx = (idx + 1) & (uint32_t)(t->capacity - 1);
    }

    t->entries[idx].path = strdup(path);
    t->entries[idx].bytecode = malloc(len);
    memcpy(t->entries[idx].bytecode, data, len);
    t->entries[idx].bytecode_len = len;
    t->entries[idx].is_static = is_static;
    if (source_hash)
        memcpy(t->entries[idx].source_hash, source_hash, 41);
    else
        t->entries[idx].source_hash[0] = '\0';
    t->count++;
}

/* ======================================================================
 * Public API
 * ====================================================================== */

code_store_t *code_store_create(void) {
    code_store_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    pthread_mutex_init(&s->lock, NULL);
    store_table_t *t = table_create(16);
    atomic_store(&s->current, t);
    return s;
}

void code_store_destroy(code_store_t *store) {
    if (!store) return;
    table_destroy(atomic_load(&store->current));
    table_destroy(store->building);
    pthread_mutex_destroy(&store->lock);
    free(store);
}

int code_store_get(code_store_t *store, const char *path,
                   code_entry_t *out) {
    store_table_t *t = atomic_load_explicit(&store->current,
                                             memory_order_acquire);
    store_entry_t *e = table_find(t, path);
    if (!e || e->is_static) return -1;
    out->bytecode     = e->bytecode;
    out->bytecode_len = e->bytecode_len;
    memcpy(out->source_hash, e->source_hash, 41);
    return 0;
}

int code_store_get_static(code_store_t *store, const char *path,
                          const void **out_data, size_t *out_len) {
    store_table_t *t = atomic_load_explicit(&store->current,
                                             memory_order_acquire);
    store_entry_t *e = table_find(t, path);
    if (!e || !e->is_static) return -1;
    *out_data = e->bytecode;
    *out_len  = e->bytecode_len;
    return 0;
}

const char *code_store_tree_hash(code_store_t *store) {
    store_table_t *t = atomic_load_explicit(&store->current,
                                             memory_order_acquire);
    return t->tree_hash;
}

void code_store_update_begin(code_store_t *store) {
    pthread_mutex_lock(&store->lock);
    store->building = table_create(64);
}

void code_store_update_put(code_store_t *store, const char *path,
                           const void *bytecode, size_t bytecode_len,
                           const char *source_hash) {
    table_insert(store->building, path, bytecode, bytecode_len,
                 source_hash, 0);
}

void code_store_update_put_static(code_store_t *store, const char *path,
                                  const void *data, size_t len) {
    table_insert(store->building, path, data, len, NULL, 1);
}

void code_store_update_set_hash(code_store_t *store, const char *tree_hash) {
    if (tree_hash)
        snprintf(store->building->tree_hash,
                 sizeof(store->building->tree_hash), "%s", tree_hash);
    else
        store->building->tree_hash[0] = '\0';
}

void code_store_update_end(code_store_t *store) {
    store_table_t *old = atomic_exchange_explicit(&store->current,
                                                   store->building,
                                                   memory_order_release);
    store->building = NULL;
    pthread_mutex_unlock(&store->lock);
    table_destroy(old);
}
