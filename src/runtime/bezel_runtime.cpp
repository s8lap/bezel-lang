// bezel_runtime.cpp — Bezel standard library runtime.
// All functions are extern "C" (no mangling) so LLVM IR can call them directly.
#include "bezel_runtime.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>

// POSIX headers for File I/O, paths, time, threads
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <pthread.h>
#include <libgen.h>

// C++ std for the thread-pool implementation
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <thread>
#include <functional>
#include <unordered_set>

// ---------------------------------------------------------------------------
// bz_array_t — dynamic array
// ---------------------------------------------------------------------------

struct bz_array_t {
    void*   data;       // heap buffer holding count*elem_size bytes
    int64_t count;
    int64_t capacity;
};

void* bz_array_alloc(int64_t elem_size, int64_t initial_cap) {
    auto* arr = (bz_array_t*)calloc(1, sizeof(bz_array_t));
    if (initial_cap > 0) {
        arr->data = calloc((size_t)initial_cap, (size_t)elem_size);
        arr->capacity = initial_cap;
    }
    return arr;
}

int64_t bz_array_len(void* arrp) {
    return arrp ? ((bz_array_t*)arrp)->count : 0;
}

static void bz_array_grow(bz_array_t* arr, int64_t elem_size, int64_t min_cap) {
    int64_t new_cap = arr->capacity < 4 ? 4 : arr->capacity * 2;
    if (new_cap < min_cap) new_cap = min_cap;
    arr->data = realloc(arr->data, (size_t)(new_cap * elem_size));
    // Zero new entries for safety
    if (new_cap > arr->capacity) {
        memset((char*)arr->data + arr->capacity * elem_size, 0,
               (size_t)((new_cap - arr->capacity) * elem_size));
    }
    arr->capacity = new_cap;
}

void bz_array_add(void* arrp, const void* elem, int64_t elem_size) {
    auto* arr = (bz_array_t*)arrp;
    if (arr->count >= arr->capacity) bz_array_grow(arr, elem_size, arr->count + 1);
    memcpy((char*)arr->data + arr->count * elem_size, elem, (size_t)elem_size);
    arr->count++;
}

void bz_array_get(void* arrp, int64_t idx, void* out, int64_t elem_size) {
    auto* arr = (bz_array_t*)arrp;
    memcpy(out, (char*)arr->data + idx * elem_size, (size_t)elem_size);
}

void bz_array_set(void* arrp, int64_t idx, const void* elem, int64_t elem_size) {
    auto* arr = (bz_array_t*)arrp;
    memcpy((char*)arr->data + idx * elem_size, elem, (size_t)elem_size);
}

// issue #388 / stdlib §4.5: OwnedArray<T>'s `set(owned)` indexer — dispose the
// element currently at idx (a pointer-sized owned object reference) before
// overwriting the slot with the new owner. Reading the old pointer before the
// store guarantees exactly-once disposal of the displaced owner.
void bz_array_set_disposing(void* arrp, int64_t idx, const void* elem,
                            int64_t elem_size, void (*dispose_elem)(void*)) {
    if (!arrp) return;
    auto* arr = (bz_array_t*)arrp;
    if (dispose_elem && arr->data) {
        void* prev = ((void**)arr->data)[idx];
        if (prev) dispose_elem(prev);
    }
    memcpy((char*)arr->data + idx * elem_size, elem, (size_t)elem_size);
}

void bz_array_remove_at(void* arrp, int64_t idx, int64_t elem_size) {
    auto* arr = (bz_array_t*)arrp;
    if (idx < 0 || idx >= arr->count) return;
    // Shift elements left
    int64_t tail = arr->count - idx - 1;
    if (tail > 0) {
        memmove((char*)arr->data + idx * elem_size,
                (char*)arr->data + (idx + 1) * elem_size,
                (size_t)(tail * elem_size));
    }
    arr->count--;
}

// stdlib §4.5: insert at idx, shifting [idx, count) up by one.
void bz_array_insert_at(void* arrp, int64_t idx, const void* elem,
                        int64_t elem_size) {
    auto* arr = (bz_array_t*)arrp;
    if (idx < 0) idx = 0;
    if (idx > arr->count) idx = arr->count;            // clamp; idx==count appends
    if (arr->count >= arr->capacity)
        bz_array_grow(arr, elem_size, arr->count + 1);
    int64_t tail = arr->count - idx;                    // elements to shift up
    if (tail > 0) {
        memmove((char*)arr->data + (idx + 1) * elem_size,
                (char*)arr->data + idx * elem_size,
                (size_t)(tail * elem_size));
    }
    memcpy((char*)arr->data + idx * elem_size, elem, (size_t)elem_size);
    arr->count++;
}

void bz_array_clear(void* arrp) {
    if (arrp) ((bz_array_t*)arrp)->count = 0;
}

// stdlib §4.5: dispose every non-null class-typed element, then empty.
void bz_array_clear_disposing(void* arrp, void (*dispose_elem)(void*)) {
    if (!arrp) return;
    auto* arr = (bz_array_t*)arrp;
    if (dispose_elem && arr->data) {
        for (int64_t i = 0; i < arr->count; ++i) {
            void* elem_ptr = ((void**)arr->data)[i];
            // Null the slot before disposing so the live range never re-reaches
            // a disposed object (mirrors the spec's move-and-null pattern).
            ((void**)arr->data)[i] = nullptr;
            if (elem_ptr) dispose_elem(elem_ptr);
        }
    }
    arr->count = 0;
}

// stdlib §4.5: dispose the non-null element at idx, then remove it.
void bz_array_remove_at_disposing(void* arrp, int64_t idx, int64_t elem_size,
                                  void (*dispose_elem)(void*)) {
    if (!arrp) return;
    auto* arr = (bz_array_t*)arrp;
    if (idx < 0 || idx >= arr->count) return;
    if (dispose_elem && arr->data) {
        void* elem_ptr = ((void**)arr->data)[idx];
        if (elem_ptr) dispose_elem(elem_ptr);
    }
    bz_array_remove_at(arrp, idx, elem_size);
}

void bz_array_free(void* arrp, void (*dispose_elem)(void*)) {
    if (!arrp) return;
    auto* arr = (bz_array_t*)arrp;
    if (dispose_elem && arr->data) {
        // Call dispose for each element (element size unknown here — codegen
        // must pass dispose only when elements are pointer-typed).
        // For pointer-sized elements, elem_size = sizeof(void*).
        // We can't know elem_size here; the codegen generates a loop instead
        // for owned arrays. bz_array_free with non-null dispose is for
        // pointer-element arrays only (the codegen ensures this).
        for (int64_t i = 0; i < arr->count; ++i) {
            void* elem_ptr = ((void**)arr->data)[i];
            if (elem_ptr) dispose_elem(elem_ptr);
        }
    }
    free(arr->data);
    free(arr);
}

void bz_array_sort(void* arrp, int64_t elem_size,
                   int (*cmp)(const void*, const void*)) {
    auto* arr = (bz_array_t*)arrp;
    if (arr && arr->count > 1)
        qsort(arr->data, (size_t)arr->count, (size_t)elem_size, cmp);
}

void* bz_array_ptr_at(void* arrp, int64_t idx, int64_t elem_size) {
    auto* arr = (bz_array_t*)arrp;
    return (char*)arr->data + idx * elem_size;
}

void bz_array_reserve(void* arrp, int64_t elem_size, int64_t cap) {
    auto* arr = (bz_array_t*)arrp;
    if (arr && cap > arr->capacity) bz_array_grow(arr, elem_size, cap);
}

// ---------------------------------------------------------------------------
// bz_dict_t — open-addressing hash table (string keys, void* values)
// ---------------------------------------------------------------------------

struct bz_entry_t {
    char*   key;        // malloc'd copy, null = empty slot
    int64_t key_len;
    void*   val;
    uint32_t hash;
    bool    deleted;
};

struct bz_dict_t {
    bz_entry_t* buckets;
    int64_t     count;
    int64_t     capacity;  // always a power of 2
    // Generic-key mode (issue #343, §4.8 "K must provide consistent Equals and
    // GetHashCode"). When key_size > 0 the table holds raw key bytes and uses
    // the synthesized hash/eq function pointers instead of the string path.
    int64_t     key_size;  // 0 = string-key mode; >0 = generic key byte count
    uint64_t  (*hash_fn)(const void* key);
    bool      (*eq_fn)(const void* a, const void* b);
};

static uint32_t fnv1a(const char* s, int64_t len) {
    uint32_t h = 2166136261u;
    for (int64_t i = 0; i < len; i++)
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static int64_t dict_find(bz_dict_t* d, const char* k, int64_t klen, uint32_t h) {
    int64_t mask = d->capacity - 1;
    int64_t idx = h & mask;
    for (int64_t probe = 0; probe < d->capacity; probe++) {
        int64_t i = (idx + probe) & mask;
        bz_entry_t& e = d->buckets[i];
        if (!e.key && !e.deleted) return -(i + 1); // empty: not found, insert here
        if (e.deleted) continue;
        if (e.hash == h && e.key_len == klen && memcmp(e.key, k, (size_t)klen) == 0)
            return i; // found
    }
    return -1; // table full (shouldn't happen with load factor management)
}

static void dict_rehash(bz_dict_t* d) {
    int64_t old_cap = d->capacity;
    bz_entry_t* old = d->buckets;
    int64_t new_cap = old_cap < 8 ? 8 : old_cap * 2;
    d->buckets = (bz_entry_t*)calloc((size_t)new_cap, sizeof(bz_entry_t));
    d->capacity = new_cap;
    d->count = 0;
    for (int64_t i = 0; i < old_cap; i++) {
        bz_entry_t& e = old[i];
        if (!e.key || e.deleted) continue;
        // Re-insert
        int64_t pos = e.hash & (new_cap - 1);
        for (int64_t p = 0; p < new_cap; p++) {
            int64_t j = (pos + p) & (new_cap - 1);
            if (!d->buckets[j].key) {
                d->buckets[j] = e;
                d->count++;
                break;
            }
        }
    }
    free(old);
}

void* bz_dict_alloc(void) {
    auto* d = (bz_dict_t*)calloc(1, sizeof(bz_dict_t));
    d->capacity = 8;
    d->buckets = (bz_entry_t*)calloc(8, sizeof(bz_entry_t));
    return d;
}

void bz_dict_free(void* dp, void (*dispose_val)(void*)) {
    if (!dp) return;
    auto* d = (bz_dict_t*)dp;
    for (int64_t i = 0; i < d->capacity; i++) {
        bz_entry_t& e = d->buckets[i];
        if (e.key && !e.deleted) {
            free(e.key);
            if (dispose_val && e.val) dispose_val(e.val);
        }
    }
    free(d->buckets);
    free(d);
}

bool bz_dict_try_get_str(void* dp, const char* k, int64_t k_len, void** out) {
    auto* d = (bz_dict_t*)dp;
    if (!d || !k) return false;
    uint32_t h = fnv1a(k, k_len);
    int64_t idx = dict_find(d, k, k_len, h);
    if (idx < 0) return false;
    if (out) *out = d->buckets[idx].val;
    return true;
}

void bz_dict_set_str(void* dp, const char* k, int64_t k_len, void* val) {
    auto* d = (bz_dict_t*)dp;
    // Rehash at 75% load
    if (d->count * 4 >= d->capacity * 3) dict_rehash(d);
    uint32_t h = fnv1a(k, k_len);
    int64_t idx = dict_find(d, k, k_len, h);
    if (idx >= 0) {
        d->buckets[idx].val = val;
        return;
    }
    // Insert at the indicated empty slot
    int64_t ins = -(idx + 1);
    d->buckets[ins].key = strndup(k, (size_t)k_len);
    d->buckets[ins].key_len = k_len;
    d->buckets[ins].val = val;
    d->buckets[ins].hash = h;
    d->buckets[ins].deleted = false;
    d->count++;
}

// issue #388 / stdlib §4.9: OwnedDictionary<string,V>'s `set(owned)` indexer.
// If `k` is already present, dispose the value stored under it before replacing
// it (exactly-once disposal of the displaced owner); a fresh key just inserts.
void bz_dict_set_str_disposing(void* dp, const char* k, int64_t k_len,
                               void* val, void (*dispose_val)(void*)) {
    auto* d = (bz_dict_t*)dp;
    if (!d) return;
    uint32_t h = fnv1a(k, k_len);
    int64_t idx = dict_find(d, k, k_len, h);
    if (idx >= 0) {
        if (dispose_val && d->buckets[idx].val) dispose_val(d->buckets[idx].val);
        d->buckets[idx].val = val;
        return;
    }
    bz_dict_set_str(dp, k, k_len, val);   // fresh key: nothing to dispose
}

bool bz_dict_remove_str(void* dp, const char* k, int64_t k_len) {
    auto* d = (bz_dict_t*)dp;
    uint32_t h = fnv1a(k, k_len);
    int64_t idx = dict_find(d, k, k_len, h);
    if (idx < 0) return false;
    free(d->buckets[idx].key);
    d->buckets[idx].key = nullptr;
    d->buckets[idx].deleted = true;
    d->count--;
    return true;
}

bool bz_dict_contains_str(void* dp, const char* k, int64_t k_len) {
    return bz_dict_try_get_str(dp, k, k_len, nullptr);
}

int64_t bz_dict_count(void* dp) {
    return dp ? ((bz_dict_t*)dp)->count : 0;
}

void bz_dict_keys_str(void* dp, char*** keys_out, int64_t** lens_out, int64_t* n_out) {
    auto* d = (bz_dict_t*)dp;
    int64_t n = d ? d->count : 0;
    auto** keys = (char**)malloc((size_t)(n * sizeof(char*)));
    auto*  lens = (int64_t*)malloc((size_t)(n * sizeof(int64_t)));
    int64_t j = 0;
    if (d) {
        for (int64_t i = 0; i < d->capacity && j < n; i++) {
            bz_entry_t& e = d->buckets[i];
            if (e.key && !e.deleted) {
                keys[j] = strndup(e.key, (size_t)e.key_len);
                lens[j] = e.key_len;
                j++;
            }
        }
    }
    if (keys_out) *keys_out = keys; else free(keys);
    if (lens_out) *lens_out = lens; else free(lens);
    if (n_out)    *n_out    = j;
}

// Generic-key variants (issue #343, §4.8). Keys are raw byte blobs of
// `key_size` bytes; hashing and equality are delegated to the synthesized
// GetHashCode / Equals of the key type (§8.3 / §9.1 / §9.2) via the function
// pointers the codegen passes at allocation time. This makes records (and any
// value type with the equality contract) usable as Dictionary/Set keys.
//
// Storage reuses bz_entry_t: `key` holds a malloc'd copy of the key bytes and
// `key_len` holds key_size, so bz_dict_free / dict_rehash work unchanged.

// Find a generic key in the table. Returns the bucket index, or -(idx+1) for
// the empty insertion slot, or -1 if full. Mirrors dict_find but compares with
// the user-provided equality function (and matches the cached hash first).
static int64_t dict_find_gen(bz_dict_t* d, const void* k, uint32_t h) {
    int64_t mask = d->capacity - 1;
    int64_t idx = h & mask;
    for (int64_t probe = 0; probe < d->capacity; probe++) {
        int64_t i = (idx + probe) & mask;
        bz_entry_t& e = d->buckets[i];
        if (!e.key && !e.deleted) return -(i + 1); // empty: not found, insert here
        if (e.deleted) continue;
        if (e.hash == h && d->eq_fn(e.key, k))
            return i; // found
    }
    return -1; // table full (shouldn't happen with load factor management)
}

void* bz_dict_alloc_generic(int64_t key_size,
                              uint64_t (*hash)(const void* key),
                              bool     (*eq)(const void* a, const void* b)) {
    auto* d = (bz_dict_t*)calloc(1, sizeof(bz_dict_t));
    d->capacity = 8;
    d->buckets = (bz_entry_t*)calloc(8, sizeof(bz_entry_t));
    d->key_size = key_size > 0 ? key_size : 1;
    d->hash_fn = hash;
    d->eq_fn = eq;
    return d;
}

bool bz_dict_try_get_gen(void* dp, const void* key, void** out) {
    auto* d = (bz_dict_t*)dp;
    if (!d || !key || d->key_size <= 0) return false;
    uint32_t h = (uint32_t)d->hash_fn(key);
    int64_t idx = dict_find_gen(d, key, h);
    if (idx < 0) return false;
    if (out) *out = d->buckets[idx].val;
    return true;
}

void bz_dict_set_gen(void* dp, const void* key, void* val) {
    auto* d = (bz_dict_t*)dp;
    if (!d || !key || d->key_size <= 0) return;
    // Rehash at 75% load (same policy as the string variant).
    if (d->count * 4 >= d->capacity * 3) dict_rehash(d);
    uint32_t h = (uint32_t)d->hash_fn(key);
    int64_t idx = dict_find_gen(d, key, h);
    if (idx >= 0) {
        d->buckets[idx].val = val;
        return;
    }
    int64_t ins = -(idx + 1);
    char* kc = (char*)malloc((size_t)d->key_size);
    memcpy(kc, key, (size_t)d->key_size);
    d->buckets[ins].key = kc;
    d->buckets[ins].key_len = d->key_size;
    d->buckets[ins].val = val;
    d->buckets[ins].hash = h;
    d->buckets[ins].deleted = false;
    d->count++;
}

// issue #388 / stdlib §4.9: generic-key counterpart to
// bz_dict_set_str_disposing — dispose the value under `key` (if present) before
// replacing it; a fresh key just inserts.
void bz_dict_set_gen_disposing(void* dp, const void* key, void* val,
                               void (*dispose_val)(void*)) {
    auto* d = (bz_dict_t*)dp;
    if (!d || !key || d->key_size <= 0) return;
    uint32_t h = (uint32_t)d->hash_fn(key);
    int64_t idx = dict_find_gen(d, key, h);
    if (idx >= 0) {
        if (dispose_val && d->buckets[idx].val) dispose_val(d->buckets[idx].val);
        d->buckets[idx].val = val;
        return;
    }
    bz_dict_set_gen(dp, key, val);        // fresh key: nothing to dispose
}

bool bz_dict_remove_gen(void* dp, const void* key) {
    auto* d = (bz_dict_t*)dp;
    if (!d || !key || d->key_size <= 0) return false;
    uint32_t h = (uint32_t)d->hash_fn(key);
    int64_t idx = dict_find_gen(d, key, h);
    if (idx < 0) return false;
    free(d->buckets[idx].key);
    d->buckets[idx].key = nullptr;
    d->buckets[idx].deleted = true;
    d->count--;
    return true;
}

bool bz_dict_contains_gen(void* dp, const void* key) {
    return bz_dict_try_get_gen(dp, key, nullptr);
}

// stdlib §4.10 / §4.8: empty a hash table in place, keeping it allocated for
// reuse. Frees each live key's malloc'd copy and, if `dispose_val` is non-null,
// disposes each live value; then zeroes every bucket and resets the count. Used
// by Set<T>.Clear() / Dictionary<K,V>.Clear() over the generic table. The
// capacity, key_size, and hash/eq function pointers are preserved. For a
// dict-backed Set the value slots are non-owned sentinels, so callers pass null
// for `dispose_val`.
void bz_dict_clear(void* dp, void (*dispose_val)(void*)) {
    if (!dp) return;
    auto* d = (bz_dict_t*)dp;
    for (int64_t i = 0; i < d->capacity; i++) {
        bz_entry_t& e = d->buckets[i];
        if (e.key && !e.deleted) {
            free(e.key);
            if (dispose_val && e.val) dispose_val(e.val);
        }
    }
    memset(d->buckets, 0, (size_t)d->capacity * sizeof(bz_entry_t));
    d->count = 0;
}

// Enumerate the generic table's keys (issue #370, §4.8 Dictionary.Keys /
// §4.10 Set iteration). Returns a malloc'd array of `n` pointers, each a
// malloc'd copy of one live key's `key_size` bytes. Mirrors bz_dict_keys_str
// (which strdup's string keys) but copies the raw key blob instead, so it
// serves int/enum/string/record keys uniformly. Iteration order is the
// table's slot order — the same order bz_dict_keys_str already yields for the
// string-key path (the open-addressing table does not track insertion order).
void bz_dict_keys_gen(void* dp, void*** keys_out, int64_t* n_out) {
    auto* d = (bz_dict_t*)dp;
    int64_t n = d ? d->count : 0;
    auto** keys = (void**)malloc((size_t)(n > 0 ? n : 1) * sizeof(void*));
    int64_t j = 0;
    if (d && d->key_size > 0) {
        for (int64_t i = 0; i < d->capacity && j < n; i++) {
            bz_entry_t& e = d->buckets[i];
            if (e.key && !e.deleted) {
                void* kc = malloc((size_t)d->key_size);
                memcpy(kc, e.key, (size_t)d->key_size);
                keys[j++] = kc;
            }
        }
    }
    if (keys_out) *keys_out = keys; else free(keys);
    if (n_out)    *n_out    = j;
}

// issue #389 / stdlib §4.10: fold a snapshot of `other`'s elements into/against
// a dict-backed Set `self`. See the header for the `op` codes and the snapshot
// contract. Frees the snapshot before returning. UnionWith/ExceptWith iterate
// the snapshot and mutate self (no mutate-while-iterate hazard — the snapshot is
// a private copy). IntersectWith and the subset/superset queries first build a
// temporary membership table from the snapshot using self's own hash/eq/key_size
// (so equality follows T's contract, §8.3), then compare; IntersectWith also
// snapshots self's keys before removing, so it never mutates a table it iterates.
int32_t bz_set_algebra_gen(void* selfp, void** keys, int64_t n, int32_t op) {
    auto* self = (bz_dict_t*)selfp;
    int32_t result = 0;
    if (!self || self->key_size <= 0) { /* fall through to free */ }
    else if (op == 0) {                 // UnionWith
        for (int64_t i = 0; i < n; i++)
            bz_dict_set_gen(self, keys[i], (void*)1);
    } else if (op == 1) {               // ExceptWith
        for (int64_t i = 0; i < n; i++)
            bz_dict_remove_gen(self, keys[i]);
    } else {                            // IntersectWith / IsSubsetOf / IsSupersetOf
        // Build a temp membership set from the snapshot, keyed like self.
        auto* other = (bz_dict_t*)bz_dict_alloc_generic(
            self->key_size, self->hash_fn, self->eq_fn);
        for (int64_t i = 0; i < n; i++)
            bz_dict_set_gen(other, keys[i], (void*)1);
        if (op == 2) {                  // IntersectWith: keep self ∩ other
            // Snapshot self's keys first, then remove every key not in `other`.
            void** selfKeys = nullptr; int64_t sn = 0;
            bz_dict_keys_gen(self, &selfKeys, &sn);
            for (int64_t i = 0; i < sn; i++) {
                if (!bz_dict_contains_gen(other, selfKeys[i]))
                    bz_dict_remove_gen(self, selfKeys[i]);
                free(selfKeys[i]);
            }
            free(selfKeys);
        } else if (op == 3) {           // IsSubsetOf: every self element in other
            result = 1;
            void** selfKeys = nullptr; int64_t sn = 0;
            bz_dict_keys_gen(self, &selfKeys, &sn);
            for (int64_t i = 0; i < sn; i++) {
                if (!bz_dict_contains_gen(other, selfKeys[i])) result = 0;
                free(selfKeys[i]);
            }
            free(selfKeys);
        } else if (op == 4) {           // IsSupersetOf: every other element in self
            result = 1;
            for (int64_t i = 0; i < n; i++)
                if (!bz_dict_contains_gen(self, keys[i])) result = 0;
        }
        bz_dict_free(other, nullptr);   // sentinel values: nothing to dispose
    }
    for (int64_t i = 0; i < n; i++) free(keys[i]);
    free(keys);
    return result;
}

// ---------------------------------------------------------------------------
// Random
// ---------------------------------------------------------------------------

struct bz_rng_t { uint64_t state[2]; }; // xorshift128+

static uint64_t xorshift128_next(uint64_t* s) {
    uint64_t t = s[0];
    uint64_t const r = s[1];
    s[0] = r;
    t ^= t << 23;
    t ^= t >> 17;
    t ^= r ^ (r >> 26);
    s[1] = t;
    return t + r;
}

void* bz_random_alloc(int64_t seed) {
    auto* r = (bz_rng_t*)malloc(sizeof(bz_rng_t));
    r->state[0] = (uint64_t)seed ^ 0xdeadbeef12345678ULL;
    r->state[1] = (uint64_t)seed * 6364136223846793005ULL + 1442695040888963407ULL;
    if (r->state[0] == 0) r->state[0] = 1;
    if (r->state[1] == 0) r->state[1] = 1;
    return r;
}

void bz_random_free(void* r) { free(r); }

int32_t bz_random_next_int(void* rp, int32_t lo, int32_t hi_exclusive) {
    auto* r = (bz_rng_t*)rp;
    uint64_t v = xorshift128_next(r->state);
    if (hi_exclusive <= lo) return lo;
    return lo + (int32_t)(v % (uint64_t)(hi_exclusive - lo));
}

double bz_random_next_double(void* rp) {
    auto* r = (bz_rng_t*)rp;
    uint64_t v = xorshift128_next(r->state);
    // Map to [0, 1)
    return (double)(v >> 11) * (1.0 / 9007199254740992.0);
}

void bz_random_next_bytes(void* rp, uint8_t* buf, int64_t len) {
    auto* r = (bz_rng_t*)rp;
    for (int64_t i = 0; i < len; i += 8) {
        uint64_t v = xorshift128_next(r->state);
        int64_t chunk = (len - i) < 8 ? (len - i) : 8;
        memcpy(buf + i, &v, (size_t)chunk);
    }
}

// ---------------------------------------------------------------------------
// DateTime — .NET compatible tick epoch (0001-01-01 00:00:00 UTC)
// POSIX epoch is 1970-01-01 00:00:00 UTC = tick 621355968000000000
// ---------------------------------------------------------------------------

static const int64_t POSIX_EPOCH_TICKS = 621355968000000000LL;
static const int64_t TICKS_PER_SECOND  = 10000000LL;
static const int64_t TICKS_PER_MS      = 10000LL;

int64_t bz_datetime_now_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return POSIX_EPOCH_TICKS
         + (int64_t)ts.tv_sec * TICKS_PER_SECOND
         + (int64_t)ts.tv_nsec / 100;
}

int64_t bz_datetime_utcnow_ticks(void) {
    return bz_datetime_now_ticks(); // CLOCK_REALTIME is UTC
}

void bz_datetime_ticks_to_parts(int64_t ticks,
                                 int32_t* year,  int32_t* month,  int32_t* day,
                                 int32_t* hour,  int32_t* minute, int32_t* second,
                                 int32_t* ms) {
    int64_t posix_sec = (ticks - POSIX_EPOCH_TICKS) / TICKS_PER_SECOND;
    int64_t rem_ticks = (ticks - POSIX_EPOCH_TICKS) % TICKS_PER_SECOND;
    time_t t = (time_t)posix_sec;
    struct tm* tm_info = gmtime(&t);
    if (year)   *year   = tm_info->tm_year + 1900;
    if (month)  *month  = tm_info->tm_mon  + 1;
    if (day)    *day    = tm_info->tm_mday;
    if (hour)   *hour   = tm_info->tm_hour;
    if (minute) *minute = tm_info->tm_min;
    if (second) *second = tm_info->tm_sec;
    if (ms)     *ms     = (int32_t)(rem_ticks / TICKS_PER_MS);
}

int64_t bz_datetime_parts_to_ticks(int32_t year, int32_t month,  int32_t day,
                                    int32_t hour, int32_t minute, int32_t second,
                                    int32_t ms) {
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min  = minute;
    t.tm_sec  = second;
    time_t posix = timegm(&t);
    return POSIX_EPOCH_TICKS
         + (int64_t)posix * TICKS_PER_SECOND
         + (int64_t)ms * TICKS_PER_MS;
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

int32_t bz_file_read_all_bytes(const char* path, uint8_t** out, int64_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return -errno;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc((size_t)sz + 1);
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = 0;
    fclose(f);
    *out = buf;
    if (len) *len = (int64_t)n;
    return 0;
}

int32_t bz_file_read_all_text(const char* path, char** out, int64_t* byte_len) {
    uint8_t* buf; int64_t len;
    int32_t r = bz_file_read_all_bytes(path, &buf, &len);
    *out = (char*)buf;
    if (byte_len) *byte_len = len;
    return r;
}

// bz.string layout (§4.6, issue #422 strings increment):
//   { void* data (off 0), int64_t len (off 8), int8_t width (off 16),
//     int64_t rc (off 24) } = 32 bytes.
// The reference count `rc` was appended AFTER `width` so the three original
// fields keep their offsets (0/8/16): codegen accesses them by StructGEP index
// (layout-agnostic) and the runtime touches them by raw byte offset, so neither
// side moves. `rc` starts at 1 (the producer's reference) at every heap-alloc
// site; bz_string_retain / bz_string_release maintain it; a static/literal
// string carries the SENTINEL rc and is never freed (see below).
static const int64_t BZ_STRING_SIZEOF = 32;
// Byte offset of the rc field inside the header (after data/len/width + pad).
static const int64_t BZ_STRING_RC_OFFSET = 24;
// Sentinel refcount for literal/global strings: retain/release are no-ops on it
// so a constant LLVM global is never decremented toward free(). A live heap
// string starts at 1 and could never reach this by retaining (2^63 retains).
static const int64_t BZ_STRING_RC_SENTINEL = 0x7FFFFFFFFFFFFFFFLL;

// Initialize the rc field of a freshly heap-allocated bz_string header to 1.
// The header was calloc'd (rc reads 0); the producer holds the first reference.
static inline void bz_string_init_rc(void* s) {
    if (!s) return;
    int64_t one = 1;
    memcpy((uint8_t*)s + BZ_STRING_RC_OFFSET, &one, sizeof(int64_t));
}

void* bz_file_read_all_lines(const char* path) {
    uint8_t* buf; int64_t len;
    if (bz_file_read_all_bytes(path, &buf, &len) != 0)
        return bz_array_alloc(8, 0);  // empty array on error
    void* arr = bz_array_alloc(8, 16);  // array of bz.string* pointers
    int64_t start = 0;
    for (int64_t i = 0; i <= len; ++i) {
        if (i == len || buf[i] == '\n') {
            int64_t lineLen = i - start;
            if (lineLen > 0 && buf[start + lineLen - 1] == '\r') lineLen--;
            // Build a bz.string struct: { data ptr, len i64, width i8 }
            char* lineData = (char*)malloc((size_t)(lineLen + 1));
            memcpy(lineData, buf + start, (size_t)lineLen);
            lineData[lineLen] = '\0';
            uint8_t* s = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
            memcpy(s, &lineData, sizeof(void*));            // data
            memcpy(s + 8, &lineLen, sizeof(int64_t));      // len
            s[16] = 1;                                      // width = 1 (ASCII)
            bz_string_init_rc(s);  // #422: producer's reference, rc := 1
            bz_array_add(arr, &s, 8);  // store the pointer to s
            start = i + 1;
        }
    }
    free(buf);
    return arr;
}

int32_t bz_file_write_all_bytes(const char* path, const uint8_t* data, int64_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return -errno;
    fwrite(data, 1, (size_t)len, f);
    fclose(f);
    return 0;
}

int32_t bz_file_write_all_text(const char* path, const char* data, int64_t len) {
    return bz_file_write_all_bytes(path, (const uint8_t*)data, len);
}

int32_t bz_file_append_all_text(const char* path, const char* data, int64_t len) {
    FILE* f = fopen(path, "ab");
    if (!f) return -errno;
    fwrite(data, 1, (size_t)len, f);
    fclose(f);
    return 0;
}

bool bz_file_exists(const char* path) {
    return access(path, F_OK) == 0;
}

bool bz_file_delete(const char* path) {
    return unlink(path) == 0;
}

int32_t bz_file_copy(const char* src, const char* dst, bool overwrite) {
    if (!overwrite && bz_file_exists(dst)) return -EEXIST;
    uint8_t* buf; int64_t len;
    int32_t r = bz_file_read_all_bytes(src, &buf, &len);
    if (r) return r;
    r = bz_file_write_all_bytes(dst, buf, len);
    free(buf);
    return r;
}

int32_t bz_file_move(const char* src, const char* dst) {
    if (rename(src, dst) == 0) return 0;
    int32_t r = bz_file_copy(src, dst, true);
    if (r == 0) unlink(src);
    return r;
}

bool bz_dir_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int32_t bz_dir_create(const char* path) {
    // Create all intermediate directories (like mkdir -p)
    char* tmp = strdup(path);
    char* p = tmp + 1;
    while ((p = strchr(p, '/'))) {
        *p = '\0';
        mkdir(tmp, 0755);
        *p = '/';
        p++;
    }
    int r = mkdir(tmp, 0755);
    free(tmp);
    return (r == 0 || errno == EEXIST) ? 0 : -errno;
}

static int rmdir_recursive(const char* path) {
    DIR* d = opendir(path);
    if (!d) return -errno;
    struct dirent* entry;
    char buf[4096];
    while ((entry = readdir(d))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        snprintf(buf, sizeof(buf), "%s/%s", path, entry->d_name);
        struct stat st;
        if (stat(buf, &st) == 0) {
            if (S_ISDIR(st.st_mode)) rmdir_recursive(buf);
            else unlink(buf);
        }
    }
    closedir(d);
    return rmdir(path) == 0 ? 0 : -errno;
}

int32_t bz_dir_delete(const char* path, bool recursive) {
    if (recursive) return rmdir_recursive(path);
    return rmdir(path) == 0 ? 0 : -errno;
}

void* bz_dir_get_files(const char* path, int64_t* count_out) {
    DIR* d = opendir(path);
    if (!d) { if (count_out) *count_out = 0; return nullptr; }
    std::vector<char*> results;
    struct dirent* entry;
    char fullpath[4096];
    while ((entry = readdir(d))) {
        if (entry->d_name[0] == '.') continue;
        struct stat st;
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);
        if (stat(fullpath, &st) == 0 && S_ISREG(st.st_mode))
            results.push_back(strdup(fullpath));
    }
    closedir(d);
    char** arr = (char**)malloc(results.size() * sizeof(char*));
    for (size_t i = 0; i < results.size(); i++) arr[i] = results[i];
    if (count_out) *count_out = (int64_t)results.size();
    return arr;
}

void* bz_dir_get_dirs(const char* path, int64_t* count_out) {
    DIR* d = opendir(path);
    if (!d) { if (count_out) *count_out = 0; return nullptr; }
    std::vector<char*> results;
    struct dirent* entry;
    char fullpath[4096];
    while ((entry = readdir(d))) {
        if (entry->d_name[0] == '.') continue;
        struct stat st;
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);
        if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode))
            results.push_back(strdup(fullpath));
    }
    closedir(d);
    char** arr = (char**)malloc(results.size() * sizeof(char*));
    for (size_t i = 0; i < results.size(); i++) arr[i] = results[i];
    if (count_out) *count_out = (int64_t)results.size();
    return arr;
}

char* bz_path_combine(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    bool slash = la > 0 && a[la - 1] != '/';
    char* out = (char*)malloc(la + lb + 2);
    memcpy(out, a, la);
    if (slash) out[la++] = '/';
    memcpy(out + la, b, lb + 1);
    return out;
}

char* bz_path_filename(const char* path) {
    return strdup(basename((char*)path));  // POSIX basename
}

char* bz_path_extension(const char* path) {
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char* dot = strrchr(base, '.');
    return dot ? strdup(dot) : strdup("");
}

char* bz_path_stem(const char* path) {
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char* dot = strrchr(base, '.');
    if (!dot) return strdup(base);
    size_t n = (size_t)(dot - base);
    char* out = (char*)malloc(n + 1);
    memcpy(out, base, n); out[n] = '\0';
    return out;
}

char* bz_path_dirname(const char* path) {
    char* copy = strdup(path);
    char* dir = dirname(copy);  // POSIX dirname
    char* result = strdup(dir);
    free(copy);
    return result;
}

char* bz_path_absolute(const char* path) {
    char buf[4096];
    return realpath(path, buf) ? strdup(buf) : strdup(path);
}

// ---------------------------------------------------------------------------
// Task scheduler — pthread-based thread pool
// ---------------------------------------------------------------------------

struct bz_task_t {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool is_error = false;
    char* error_msg = nullptr;
    void* result = nullptr;
    int64_t result_size = 0;
    // Continuation chain
    struct Cont { void (*fn)(void*, void*); void* state; };
    std::vector<Cont> continuations;
};

struct BzScheduler {
    std::vector<std::thread> threads;
    std::queue<std::function<void()>> queue;
    std::mutex mtx;
    std::condition_variable cv;
    bool shutdown = false;

    void run_worker() {
        for (;;) {
            std::function<void()> work;
            {
                std::unique_lock<std::mutex> lk(mtx);
                cv.wait(lk, [this]{ return !queue.empty() || shutdown; });
                if (shutdown && queue.empty()) return;
                work = std::move(queue.front());
                queue.pop();
            }
            work();
        }
    }

    void submit(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            queue.push(std::move(f));
        }
        cv.notify_one();
    }
};

static BzScheduler* g_scheduler = nullptr;

void bz_scheduler_init(int num_threads) {
    if (g_scheduler) return;
    g_scheduler = new BzScheduler();
    for (int i = 0; i < num_threads; i++)
        g_scheduler->threads.emplace_back([](){ g_scheduler->run_worker(); });
}

void bz_scheduler_shutdown(void) {
    if (!g_scheduler) return;
    { std::lock_guard<std::mutex> lk(g_scheduler->mtx); g_scheduler->shutdown = true; }
    g_scheduler->cv.notify_all();
    for (auto& t : g_scheduler->threads) t.join();
    delete g_scheduler;
    g_scheduler = nullptr;
}

void* bz_task_alloc(void) {
    return new bz_task_t();
}

void bz_task_schedule(void* tp, void (*fn)(void* task, void* state), void* state) {
    if (!g_scheduler) bz_scheduler_init(4); // lazy init
    auto* task = (bz_task_t*)tp;
    g_scheduler->submit([task, fn, state]() { fn(task, state); });
}

static void task_notify(bz_task_t* task) {
    // Run continuations, then wake any sync-awaiter
    std::vector<bz_task_t::Cont> conts;
    {
        std::lock_guard<std::mutex> lk(task->mtx);
        conts = task->continuations;
    }
    for (auto& c : conts)
        if (g_scheduler) g_scheduler->submit([c, task](){ c.fn(task, c.state); });
        else c.fn(task, c.state);
    task->cv.notify_all();
}

void bz_task_complete(void* tp, const void* result, int64_t result_size) {
    auto* task = (bz_task_t*)tp;
    {
        std::lock_guard<std::mutex> lk(task->mtx);
        if (result_size > 0 && result) {
            task->result = malloc((size_t)result_size);
            memcpy(task->result, result, (size_t)result_size);
            task->result_size = result_size;
        }
        task->done = true;
    }
    task_notify(task);
}

void bz_task_complete_error(void* tp, const char* error_msg) {
    auto* task = (bz_task_t*)tp;
    { std::lock_guard<std::mutex> lk(task->mtx);
      task->error_msg = strdup(error_msg);
      task->is_error = true;
      task->done = true; }
    task_notify(task);
}

bool bz_task_is_done(void* tp) {
    auto* task = (bz_task_t*)tp;
    std::lock_guard<std::mutex> lk(task->mtx);
    return task->done;
}

bool bz_task_is_error(void* tp) {
    auto* task = (bz_task_t*)tp;
    std::lock_guard<std::mutex> lk(task->mtx);
    return task->is_error;
}

void* bz_task_await_sync(void* tp) {
    auto* task = (bz_task_t*)tp;
    std::unique_lock<std::mutex> lk(task->mtx);
    task->cv.wait(lk, [task]{ return task->done; });
    return task->is_error ? nullptr : task->result;
}

const char* bz_task_error(void* tp) {
    return ((bz_task_t*)tp)->error_msg;
}

void bz_task_then(void* tp, void (*cont)(void* task, void* state), void* state) {
    auto* task = (bz_task_t*)tp;
    bool already_done;
    {
        std::lock_guard<std::mutex> lk(task->mtx);
        already_done = task->done;
        if (!already_done) task->continuations.push_back({cont, state});
    }
    if (already_done) {
        if (g_scheduler) g_scheduler->submit([cont, task, state](){ cont(task, state); });
        else cont(task, state);
    }
}

void bz_task_free(void* tp) {
    auto* task = (bz_task_t*)tp;
    free(task->result);
    free(task->error_msg);
    delete task;
}

// ---------------------------------------------------------------------------
// StringBuilder helper
// ---------------------------------------------------------------------------

// The bz.string struct layout: {ptr data, i64 len, i8 width}
struct bz_string_struct {
    void*   data;
    int64_t len;
    int8_t  width;
};

// bz_array_enum_t: vtable-backed array enumerator for IEnumerator<T> protocol.
// Vtable layout matches our iterator state machines: [td, Dispose, MoveNext, Current].
struct bz_array_enum_t {
    void* vtable;      // points to bz_array_enum_vtable
    void* arr;         // the underlying bz_array_t*
    int64_t idx;       // current index (-1 = before first)
    int64_t esz;       // element size in bytes
};

static int32_t bz_array_enum_movenext(bz_array_enum_t* e) {
    e->idx++;
    return (e->idx < bz_array_len(e->arr)) ? 1 : 0;
}
static void* bz_array_enum_current(bz_array_enum_t* e) {
    return bz_array_ptr_at(e->arr, e->idx, e->esz);
}
static void bz_array_enum_dispose(bz_array_enum_t* e) {
    free(e);
}

// Static vtable: [self-ptr, Dispose, MoveNext, get_Current]
static void* bz_array_enum_vtable[4] = {
    nullptr,  // slot 0: filled at startup (see bz_array_enum_alloc)
    (void*)bz_array_enum_dispose,
    (void*)bz_array_enum_movenext,
    (void*)bz_array_enum_current
};

void* bz_array_enum_alloc(void* arr, int64_t elem_size) {
    auto* e = (bz_array_enum_t*)calloc(1, sizeof(bz_array_enum_t));
    bz_array_enum_vtable[0] = bz_array_enum_vtable;  // self-referential td
    e->vtable = bz_array_enum_vtable;
    e->arr = arr;
    e->idx = -1;
    e->esz = elem_size;
    return e;
}

// ---------------------------------------------------------------------------
// Lazy LINQ enumerators (stdlib §5: "All are lazy unless documented otherwise").
//
// Every LINQ operator returns a deferred enumerator that does no work until it
// is enumerated (by a for-loop or a materializer like ToArray). The enumerators
// share the iterator vtable protocol [td, Dispose, MoveNext, Current] used by
// the codegen iterator state machines (LlvmBackend_Iterators.cpp) and the array
// enumerator above:
//   slot 0 = type descriptor (self-pointer sentinel)
//   slot 1 = Dispose(self) -> void
//   slot 2 = MoveNext(self) -> int32 (1 = advanced, 0 = exhausted)
//   slot 3 = Current(self)  -> void* (pointer to the current element's bytes)
//
// Element data is type-erased (raw bytes of `esz` bytes). Closures (predicates,
// selectors, key functions) are invoked through codegen-emitted trampolines
// with fixed C signatures so the runtime stays element-type-agnostic:
//   predicate trampoline: int32_t f(void* closure, const void* elem_ptr)
//   selector  trampoline: void    f(void* closure, const void* elem_ptr, void* out_ptr)
//   key       trampoline: int64_t f(void* closure, const void* elem_ptr)
// A lazy operator over an Array<T> wraps the array via bz_lazy_from_array; over
// another lazy enumerator it stores the upstream enumerator pointer directly,
// so chains (`a.Where(..).Select(..).Take(n)`) compose without materializing.
// ---------------------------------------------------------------------------

typedef int32_t (*bz_pred_fn)(void* closure, const void* elem);
typedef void    (*bz_sel_fn)(void* closure, const void* elem, void* out);
typedef int64_t (*bz_key_fn)(void* closure, const void* elem);
// Element equality trampoline (Distinct): content-aware comparison the runtime
// cannot do over type-erased bytes (e.g. string content equality vs. raw bytes).
typedef int32_t (*bz_eq_fn)(const void* a, const void* b);
// SelectMany selector trampoline: maps one element to a fresh lazy enumerator
// (the inner IEnumerable<U>). Returns an opaque bz_lazy_t* the runtime drains.
typedef void* (*bz_flatsel_fn)(void* closure, const void* elem);
// Zip combine trampoline: pairs one element from each source into the output.
typedef void  (*bz_comb_fn)(void* closure, const void* a, const void* b,
                            void* out);

enum bz_lazy_kind {
    BZ_LAZY_ARRAY,     // source = bz_array_t*, iterate by index
    BZ_LAZY_WHERE,     // filter via predicate
    BZ_LAZY_SELECT,    // project via selector (esz = output element size)
    BZ_LAZY_TAKE,      // first `n` elements
    BZ_LAZY_SKIP,      // skip first `n` elements
    BZ_LAZY_DISTINCT,  // de-duplicate (raw-byte equality, order preserved)
    BZ_LAZY_ORDERBY,   // materialize-on-first-MoveNext: sorted snapshot
    BZ_LAZY_SET,       // dict-backed Set<T> source: owned key snapshot by index
    BZ_LAZY_CONCAT,    // §5 Combination: yield all of `up`, then all of `down`
    BZ_LAZY_TAKEWHILE, // §5 Slicing: prefix while predicate holds, then stop
    BZ_LAZY_SKIPWHILE, // §5 Slicing: drop leading run satisfying predicate
    BZ_LAZY_SELECTMANY,// §5 Projection: flatten (T)->IEnumerable<U>
    BZ_LAZY_ZIP,       // §5 Combination: pair `up` and `down` via combine
};

struct bz_lazy_t {
    void*   vtable;     // shared bz_lazy_vtable
    int32_t kind;
    int64_t esz;        // element size in bytes (output size for SELECT)
    // Source: either an upstream lazy enumerator (`up`) or an array (`arr`).
    bz_lazy_t* up;      // upstream enumerator (null for ARRAY)
    bz_lazy_t* down;    // second source (CONCAT/ZIP)
    void*   arr;        // bz_array_t* (ARRAY kind)
    int64_t idx;        // ARRAY: current index; TAKE/SKIP: count consumed
    int64_t n;          // TAKE/SKIP threshold
    // Closure + trampoline (WHERE/SELECT/ORDERBY).
    void*   closure;
    bz_pred_fn pred;
    bz_sel_fn  sel;
    bz_key_fn  keyfn;
    bz_eq_fn   eqfn;    // DISTINCT: element equality (null = raw memcmp)
    bz_flatsel_fn flatsel;  // SELECTMANY: element -> inner lazy enumerator
    bz_comb_fn comb;        // ZIP: (a, b) -> out
    int32_t descending; // ORDERBY direction
    int64_t up_esz;     // ZIP: byte stride of the `up`/left element
    int64_t down_esz;   // ZIP: byte stride of the `down`/right element
    // Scratch storage for the current element bytes (Current returns cur).
    void*   cur;        // malloc'd, esz bytes
    // DISTINCT: a dynamic array of seen elements.
    void*   seen;       // bz_array_t* of element bytes (DISTINCT)
    // ORDERBY: materialized + sorted snapshot, iterated by index.
    void*   sorted;     // bz_array_t* (ORDERBY)
    int32_t skipped;    // SKIP/SKIPWHILE: have we already dropped the prefix?
    int32_t materialized;
    int32_t phase;      // CONCAT: 0 = draining `up`, 1 = draining `down`
    int32_t stopped;    // TAKEWHILE: predicate has failed; sequence ended
    bz_lazy_t* inner;   // SELECTMANY: current inner enumerator (null = none)
    // #422 / §28.9 / §28.3: when the operator's closure was produced INLINE (a
    // lambda literal — a +1 transient this enumerator owns), codegen sets
    // own_closure := 1 so bz_lazy_dispose releases it; a closure arriving via a
    // variable/field is a BORROW (own_closure stays 0 from calloc) and is never
    // freed here. Mirrors the GetOrAdd factoryIsInlineLambda guard.
    int32_t own_closure;
};

// Advance the upstream enumerator and return a pointer to its current element,
// or null when exhausted.
static const void* bz_lazy_up_next(bz_lazy_t* up) {
    typedef int32_t (*mn_t)(void*);
    typedef void*   (*cur_t)(void*);
    void** vt = (void**)up->vtable;
    if (!((mn_t)vt[2])(up)) return nullptr;
    return ((cur_t)vt[3])(up);
}

extern "C" void bz_closure_release(void* c);  // defined below; released on dispose

static int32_t bz_lazy_movenext(bz_lazy_t* e);
static void*   bz_lazy_current(bz_lazy_t* e) { return e->cur; }
static void    bz_lazy_dispose(bz_lazy_t* e) {
    if (!e) return;
    if (e->up) bz_lazy_dispose(e->up);
    if (e->down) bz_lazy_dispose(e->down);   // CONCAT/ZIP second source
    if (e->inner) bz_lazy_dispose(e->inner);  // SELECTMANY current inner
    if (e->cur) free(e->cur);
    if (e->seen) bz_array_free(e->seen, nullptr);
    if (e->sorted) bz_array_free(e->sorted, nullptr);
    // #422 / §28.9 / §28.3: release the operator's closure ONLY when it was an
    // inline lambda we own (own_closure set by codegen). The closure BORROWS its
    // captures, so this frees only the cell + env, never a capture — sound, no
    // double-dispose. A borrowed (variable-form) closure has own_closure == 0 and
    // is left untouched. Dispose runs exactly once per enumerator (cascaded from
    // the for-loop / ToArray drain), so this is a single release, no double-free.
    if (e->own_closure && e->closure) bz_closure_release(e->closure);
    free(e);
}

static void* bz_lazy_vtable[4] = {
    nullptr,
    (void*)bz_lazy_dispose,
    (void*)bz_lazy_movenext,
    (void*)bz_lazy_current,
};

static bz_lazy_t* bz_lazy_new(int32_t kind, int64_t esz) {
    auto* e = (bz_lazy_t*)calloc(1, sizeof(bz_lazy_t));
    bz_lazy_vtable[0] = bz_lazy_vtable;  // self-referential td sentinel
    e->vtable = bz_lazy_vtable;
    e->kind = kind;
    e->esz = esz;
    e->idx = 0;
    if (esz > 0) e->cur = malloc((size_t)esz);
    return e;
}

extern "C" void* bz_lazy_from_array(void* arr, int64_t esz) {
    auto* e = bz_lazy_new(BZ_LAZY_ARRAY, esz);
    e->arr = arr;
    return e;
}

// #422 / §28.9 / §28.3: mark a freshly-built lazy enumerator as the OWNER of its
// operator closure, so bz_lazy_dispose releases it (frees the +1 inline-lambda
// cell + env once the pipeline is disposed). Codegen calls this immediately
// after a lazy-operator constructor ONLY when the closure argument is an inline
// lambda literal — never for a borrowed (variable-form) closure. Returns the
// enumerator so the call can be threaded inline at the lowering site. Null-safe.
extern "C" void* bz_lazy_own_closure(void* lazy) {
    if (lazy) ((bz_lazy_t*)lazy)->own_closure = 1;
    return lazy;
}

// issue #389 / stdlib §5 / §4.10: a dict-backed Set<T> as a lazy source. Snapshot
// the set's element keys (bz_dict_keys_gen returns malloc'd esz-byte blobs) into
// a fresh owned bz_array_t iterated by index, then free the snapshot pointers.
// The array is stored in `sorted` so bz_lazy_dispose frees it (the set itself is
// not owned by the enumerator). Set elements are value-semantic, so copying the
// raw key bytes into the snapshot array is the correct (and only) representation.
extern "C" void* bz_lazy_from_set(void* set, int64_t esz) {
    auto* e = bz_lazy_new(BZ_LAZY_SET, esz);
    e->sorted = bz_array_alloc(esz, 8);
    void** keys = nullptr; int64_t n = 0;
    bz_dict_keys_gen(set, &keys, &n);
    for (int64_t i = 0; i < n; i++) {
        bz_array_add(e->sorted, keys[i], esz);
        free(keys[i]);
    }
    free(keys);
    return e;
}

extern "C" void* bz_lazy_where(void* up, void* closure, void* pred) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_WHERE, u->esz);
    e->up = u;
    e->closure = closure;
    e->pred = (bz_pred_fn)pred;
    return e;
}

extern "C" void* bz_lazy_select(void* up, int64_t out_esz,
                                void* closure, void* sel) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_SELECT, out_esz);
    e->up = u;
    e->closure = closure;
    e->sel = (bz_sel_fn)sel;
    return e;
}

extern "C" void* bz_lazy_take(void* up, int64_t n) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_TAKE, u->esz);
    e->up = u;
    e->n = n;
    return e;
}

extern "C" void* bz_lazy_skip(void* up, int64_t n) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_SKIP, u->esz);
    e->up = u;
    e->n = n;
    return e;
}

extern "C" void* bz_lazy_distinct(void* up, void* eqfn) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_DISTINCT, u->esz);
    e->up = u;
    e->eqfn = (bz_eq_fn)eqfn;
    e->seen = bz_array_alloc(u->esz, 8);
    return e;
}

extern "C" void* bz_lazy_orderby(void* up, void* closure, void* keyfn,
                                 int32_t descending) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_ORDERBY, u->esz);
    e->up = u;
    e->closure = closure;
    e->keyfn = (bz_key_fn)keyfn;
    e->descending = descending;
    return e;
}

// stdlib §5 Combination: Concat yields all of `up`, then all of `down`. Both
// have the same element size (same T). Element bytes are copied through `cur`.
extern "C" void* bz_lazy_concat(void* up, void* down) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_CONCAT, u->esz);
    e->up = u;
    e->down = (bz_lazy_t*)down;
    e->phase = 0;
    return e;
}

// stdlib §5 Slicing: TakeWhile yields the prefix while `pred` holds, then ends
// the sequence permanently (later passing elements are NOT yielded).
extern "C" void* bz_lazy_takewhile(void* up, void* closure, void* pred) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_TAKEWHILE, u->esz);
    e->up = u;
    e->closure = closure;
    e->pred = (bz_pred_fn)pred;
    return e;
}

// stdlib §5 Slicing: SkipWhile drops the leading run satisfying `pred`, then
// yields the remainder verbatim (including elements that satisfy `pred`).
extern "C" void* bz_lazy_skipwhile(void* up, void* closure, void* pred) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_SKIPWHILE, u->esz);
    e->up = u;
    e->closure = closure;
    e->pred = (bz_pred_fn)pred;
    return e;
}

// stdlib §5 Projection: SelectMany flattens. `out_esz` is the inner element U's
// byte size. The selector trampoline returns a fresh inner lazy enumerator per
// upstream element; the runtime drains each before pulling the next upstream.
extern "C" void* bz_lazy_selectmany(void* up, int64_t out_esz,
                                    void* closure, void* sel) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_SELECTMANY, out_esz);
    e->up = u;
    e->closure = closure;
    e->flatsel = (bz_flatsel_fn)sel;
    e->inner = nullptr;
    return e;
}

// stdlib §5 Combination: Zip pairs `up` (left) and `down` (right) positionally,
// applying `comb` to produce one `out_esz`-byte output per pair. Ends when
// either source is exhausted (result length = min of the two).
extern "C" void* bz_lazy_zip(void* up, void* down, int64_t out_esz,
                             void* closure, void* comb) {
    auto* u = (bz_lazy_t*)up;
    auto* d = (bz_lazy_t*)down;
    auto* e = bz_lazy_new(BZ_LAZY_ZIP, out_esz);
    e->up = u;
    e->down = d;
    e->closure = closure;
    e->comb = (bz_comb_fn)comb;
    e->up_esz = u->esz;
    e->down_esz = d->esz;
    return e;
}

// ORDERBY: drain upstream into a snapshot, then stable-sort by the int64 key.
static void bz_lazy_orderby_materialize(bz_lazy_t* e) {
    e->sorted = bz_array_alloc(e->esz, 8);
    const void* p;
    while ((p = bz_lazy_up_next(e->up)) != nullptr)
        bz_array_add(e->sorted, p, e->esz);
    auto* arr = (bz_array_t*)e->sorted;
    int64_t cnt = arr->count;
    // Stable insertion sort by int64 key (matches the spec's stable-ordering
    // guarantee for OrderBy; n is small in test scenarios).
    void* tmp = malloc((size_t)e->esz);
    for (int64_t i = 1; i < cnt; ++i) {
        memcpy(tmp, (char*)arr->data + i * e->esz, (size_t)e->esz);
        int64_t key_i = e->keyfn(e->closure, tmp);
        int64_t j = i - 1;
        while (j >= 0) {
            int64_t key_j = e->keyfn(e->closure, (char*)arr->data + j * e->esz);
            int shift = e->descending ? (key_j < key_i) : (key_j > key_i);
            if (!shift) break;
            memcpy((char*)arr->data + (j + 1) * e->esz,
                   (char*)arr->data + j * e->esz, (size_t)e->esz);
            --j;
        }
        memcpy((char*)arr->data + (j + 1) * e->esz, tmp, (size_t)e->esz);
    }
    free(tmp);
    e->idx = 0;
    e->materialized = 1;
}

static int32_t bz_lazy_movenext(bz_lazy_t* e) {
    switch (e->kind) {
    case BZ_LAZY_ARRAY: {
        if (e->idx >= bz_array_len(e->arr)) return 0;
        memcpy(e->cur, bz_array_ptr_at(e->arr, e->idx, e->esz), (size_t)e->esz);
        e->idx++;
        return 1;
    }
    case BZ_LAZY_WHERE: {
        const void* p;
        while ((p = bz_lazy_up_next(e->up)) != nullptr) {
            if (e->pred(e->closure, p)) {
                memcpy(e->cur, p, (size_t)e->esz);
                return 1;
            }
        }
        return 0;
    }
    case BZ_LAZY_SELECT: {
        const void* p = bz_lazy_up_next(e->up);
        if (!p) return 0;
        e->sel(e->closure, p, e->cur);  // selector writes esz output bytes
        return 1;
    }
    case BZ_LAZY_TAKE: {
        if (e->idx >= e->n) return 0;
        const void* p = bz_lazy_up_next(e->up);
        if (!p) return 0;
        memcpy(e->cur, p, (size_t)e->esz);
        e->idx++;
        return 1;
    }
    case BZ_LAZY_SKIP: {
        if (!e->skipped) {
            const void* p;
            while (e->idx < e->n && (p = bz_lazy_up_next(e->up)) != nullptr)
                e->idx++;
            e->skipped = 1;
        }
        const void* p = bz_lazy_up_next(e->up);
        if (!p) return 0;
        memcpy(e->cur, p, (size_t)e->esz);
        return 1;
    }
    case BZ_LAZY_DISTINCT: {
        const void* p;
        auto* seen = (bz_array_t*)e->seen;
        while ((p = bz_lazy_up_next(e->up)) != nullptr) {
            int dup = 0;
            for (int64_t i = 0; i < seen->count; ++i) {
                const void* s = (char*)seen->data + i * e->esz;
                int same = e->eqfn ? e->eqfn(s, p)
                                   : (memcmp(s, p, (size_t)e->esz) == 0);
                if (same) { dup = 1; break; }
            }
            if (!dup) {
                bz_array_add(e->seen, p, e->esz);
                memcpy(e->cur, p, (size_t)e->esz);
                return 1;
            }
        }
        return 0;
    }
    case BZ_LAZY_ORDERBY: {
        if (!e->materialized) bz_lazy_orderby_materialize(e);
        auto* arr = (bz_array_t*)e->sorted;
        if (e->idx >= arr->count) return 0;
        memcpy(e->cur, bz_array_ptr_at(e->sorted, e->idx, e->esz), (size_t)e->esz);
        e->idx++;
        return 1;
    }
    case BZ_LAZY_SET: {
        // issue #389: iterate the owned key snapshot (built in bz_lazy_from_set).
        auto* arr = (bz_array_t*)e->sorted;
        if (e->idx >= arr->count) return 0;
        memcpy(e->cur, bz_array_ptr_at(e->sorted, e->idx, e->esz), (size_t)e->esz);
        e->idx++;
        return 1;
    }
    case BZ_LAZY_CONCAT: {
        // §5 Combination: drain `up` (phase 0), then `down` (phase 1).
        if (e->phase == 0) {
            const void* p = bz_lazy_up_next(e->up);
            if (p) { memcpy(e->cur, p, (size_t)e->esz); return 1; }
            e->phase = 1;  // fall through to the second source
        }
        const void* p = bz_lazy_up_next(e->down);
        if (!p) return 0;
        memcpy(e->cur, p, (size_t)e->esz);
        return 1;
    }
    case BZ_LAZY_TAKEWHILE: {
        // §5 Slicing: yield while the predicate holds; the first failure ends
        // the sequence (subsequent MoveNext calls return 0).
        if (e->stopped) return 0;
        const void* p = bz_lazy_up_next(e->up);
        if (!p) { e->stopped = 1; return 0; }
        if (!e->pred(e->closure, p)) { e->stopped = 1; return 0; }
        memcpy(e->cur, p, (size_t)e->esz);
        return 1;
    }
    case BZ_LAZY_SKIPWHILE: {
        // §5 Slicing: on first MoveNext, drop the leading run satisfying the
        // predicate and yield the first element that fails it; thereafter yield
        // every remaining element verbatim.
        if (!e->skipped) {
            const void* p;
            while ((p = bz_lazy_up_next(e->up)) != nullptr) {
                if (!e->pred(e->closure, p)) {
                    e->skipped = 1;
                    memcpy(e->cur, p, (size_t)e->esz);  // first non-skipped elem
                    return 1;
                }
            }
            e->skipped = 1;  // upstream exhausted entirely within the skip run
            return 0;
        }
        const void* p = bz_lazy_up_next(e->up);
        if (!p) return 0;
        memcpy(e->cur, p, (size_t)e->esz);
        return 1;
    }
    case BZ_LAZY_SELECTMANY: {
        // §5 Projection: pull from the current inner enumerator; when it is
        // exhausted, advance the upstream to obtain the next inner sequence.
        for (;;) {
            if (e->inner) {
                const void* p = bz_lazy_up_next(e->inner);
                if (p) { memcpy(e->cur, p, (size_t)e->esz); return 1; }
                bz_lazy_dispose(e->inner);  // inner drained — release it
                e->inner = nullptr;
            }
            const void* up_elem = bz_lazy_up_next(e->up);
            if (!up_elem) return 0;          // upstream exhausted
            e->inner = (bz_lazy_t*)e->flatsel(e->closure, up_elem);
            if (!e->inner) return 0;          // defensive: null inner sequence
        }
    }
    case BZ_LAZY_ZIP: {
        // §5 Combination: advance both sources; stop at the shorter. Each source
        // owns its own `cur`, so read the left/right element pointers directly.
        const void* a = bz_lazy_up_next(e->up);
        if (!a) return 0;
        const void* b = bz_lazy_up_next(e->down);
        if (!b) return 0;
        e->comb(e->closure, a, b, e->cur);  // writes esz output bytes
        return 1;
    }
    }
    return 0;
}

// Materialize a lazy enumerator into a fresh owned Array<T> (stdlib §5 ToArray,
// the eager materialization point). Consumes (drains + disposes) the pipeline.
extern "C" void* bz_lazy_to_array(void* lazy, int64_t esz) {
    auto* e = (bz_lazy_t*)lazy;
    void* arr = bz_array_alloc(esz, 8);
    void** vt = (void**)e->vtable;
    typedef int32_t (*mn_t)(void*);
    typedef void*   (*cur_t)(void*);
    while (((mn_t)vt[2])(e))
        bz_array_add(arr, ((cur_t)vt[3])(e), esz);
    bz_lazy_dispose(e);
    return arr;
}

// issue #389 / stdlib §4.10: drain a lazy enumerator into the bz_dict_keys_gen
// snapshot shape — a malloc'd array of `n` pointers, each a malloc'd esz-byte
// copy of one element. Used to feed a non-Set IEnumerable<T> `other` into
// bz_set_algebra_gen. Disposes the drained enumerator. First materialize into a
// growable array (count unknown up front), then repackage as pointer-per-element.
extern "C" void bz_set_collect_enumerator(void* lazy, int64_t esz,
                                          void*** keys_out, int64_t* n_out) {
    auto* e = (bz_lazy_t*)lazy;
    auto* tmp = (bz_array_t*)bz_array_alloc(esz, 8);
    void** vt = (void**)e->vtable;
    typedef int32_t (*mn_t)(void*);
    typedef void*   (*cur_t)(void*);
    while (((mn_t)vt[2])(e))
        bz_array_add(tmp, ((cur_t)vt[3])(e), esz);
    bz_lazy_dispose(e);
    int64_t n = tmp->count;
    auto** keys = (void**)malloc((size_t)(n > 0 ? n : 1) * sizeof(void*));
    for (int64_t i = 0; i < n; i++) {
        void* kc = malloc((size_t)esz);
        memcpy(kc, (char*)tmp->data + i * esz, (size_t)esz);
        keys[i] = kc;
    }
    bz_array_free(tmp, nullptr);
    if (keys_out) *keys_out = keys; else free(keys);
    if (n_out)    *n_out    = n;
}

// ---------------------------------------------------------------------------
// Events (language spec §24). An EventSource holds an ordered handler list.
// Each handler is a Bezel closure value (a { fn_ptr, env_ptr } struct pointer).
// Raise is lowered in codegen; the runtime only stores/produces the list.
// ---------------------------------------------------------------------------

struct bz_event_handler_t {
    void*   closure;  // closure { fn_ptr, env_ptr } struct pointer
    int64_t token;    // unique, monotonically increasing (>0)
    bool    active;   // false once removed (tombstone)
};

struct bz_event_source_t {
    bz_event_handler_t* handlers;
    int64_t             count;     // number of slots in use (incl. tombstones)
    int64_t             capacity;
    int64_t             next_token;
};

struct bz_subscription_t {
    bz_event_source_t* src;
    int64_t            token;
    bool               disposed;
};

extern "C" void* bz_event_source_new(void) {
    auto* s = (bz_event_source_t*)calloc(1, sizeof(bz_event_source_t));
    s->next_token = 1;  // tokens are >0 so 0 can mean "none"
    return s;
}

extern "C" int64_t bz_event_add(void* srcp, void* closure) {
    auto* s = (bz_event_source_t*)srcp;
    if (!s) return 0;
    if (s->count >= s->capacity) {
        int64_t nc = s->capacity < 4 ? 4 : s->capacity * 2;
        // Capture the realloc result: on failure the original block is still
        // valid and must not be overwritten with NULL (would leak it and then
        // NULL-deref on the writes below).
        void* tmp = realloc(s->handlers,
                            (size_t)nc * sizeof(bz_event_handler_t));
        if (!tmp) return 0;
        s->handlers = (bz_event_handler_t*)tmp;
        s->capacity = nc;
    }
    int64_t tok = s->next_token++;
    s->handlers[s->count].closure = closure;
    s->handlers[s->count].token = tok;
    s->handlers[s->count].active = true;
    s->count++;
    return tok;
}

extern "C" int64_t bz_event_count(void* srcp) {
    auto* s = (bz_event_source_t*)srcp;
    if (!s) return 0;
    int64_t n = 0;
    for (int64_t i = 0; i < s->count; ++i)
        if (s->handlers[i].active) ++n;
    return n;
}

extern "C" void* bz_event_handler_at(void* srcp, int64_t idx) {
    auto* s = (bz_event_source_t*)srcp;
    if (!s) return nullptr;
    int64_t n = 0;
    for (int64_t i = 0; i < s->count; ++i) {
        if (!s->handlers[i].active) continue;
        if (n == idx) return s->handlers[i].closure;
        ++n;
    }
    return nullptr;
}

extern "C" void bz_event_remove_closure(void* srcp, void* closure) {
    auto* s = (bz_event_source_t*)srcp;
    if (!s) return;
    // §24.4: -= removes the FIRST matching handler.
    for (int64_t i = 0; i < s->count; ++i) {
        if (s->handlers[i].active && s->handlers[i].closure == closure) {
            s->handlers[i].active = false;
            return;
        }
    }
}

extern "C" void bz_event_remove_token(void* srcp, int64_t token) {
    auto* s = (bz_event_source_t*)srcp;
    if (!s || token == 0) return;
    for (int64_t i = 0; i < s->count; ++i) {
        if (s->handlers[i].token == token) {
            s->handlers[i].active = false;  // idempotent: re-clearing is harmless
            return;
        }
    }
}

// Set of live (subscribed, not-yet-disposed) bz_subscription_t* nodes. A node
// is inserted by bz_event_subscribe and erased+freed by bz_subscription_dispose.
// Membership is the authority for "is this handle still alive": Dispose must be
// idempotent (§2.11), but the fix below frees the node on dispose, so a second
// Dispose with the same (now-freed) pointer must NOT dereference it. Checking
// the set before any field access keeps the second call a safe no-op with no
// use-after-free. (Codegen relies on Subscription.Dispose being idempotent —
// e.g. an explicit `.Dispose()` plus a scope-exit/`using var` disposal both
// hand the same pointer here.) Guarded by a mutex because subscriptions, like
// the rest of the events runtime, may be touched from multiple threads.
static std::mutex&                     bz_sub_live_mutex() {
    static std::mutex m;
    return m;
}
static std::unordered_set<void*>&      bz_sub_live_set() {
    static std::unordered_set<void*> s;
    return s;
}

extern "C" void* bz_event_subscribe(void* srcp, void* closure) {
    auto* s = (bz_event_source_t*)srcp;
    int64_t tok = bz_event_add(s, closure);
    auto* sub = (bz_subscription_t*)calloc(1, sizeof(bz_subscription_t));
    sub->src = s;
    sub->token = tok;
    sub->disposed = false;
    {
        std::lock_guard<std::mutex> lk(bz_sub_live_mutex());
        bz_sub_live_set().insert(sub);
    }
    return sub;
}

extern "C" void bz_subscription_dispose(void* subp) {
    if (!subp) return;
    {
        // §2.11: Dispose is idempotent. Only a live (registered) node is acted
        // on; a second call finds the pointer absent and returns without ever
        // dereferencing the freed node (no double-free, no use-after-free).
        std::lock_guard<std::mutex> lk(bz_sub_live_mutex());
        if (bz_sub_live_set().erase(subp) == 0) return;
    }
    auto* sub = (bz_subscription_t*)subp;
    sub->disposed = true;
    if (sub->src) bz_event_remove_token(sub->src, sub->token);
    sub->src = nullptr;  // null the back-pointer before freeing
    free(sub);           // fixes #372: the node was previously never freed
}

extern "C" void bz_event_source_free(void* srcp) {
    auto* s = (bz_event_source_t*)srcp;
    if (!s) return;
    free(s->handlers);
    free(s);
}

void* bz_string_split(const char* s, int64_t s_len,
                       const char* delim, int64_t delim_len) {
    void* arr = bz_array_alloc(8, 8);
    if (delim_len <= 0) {
        // Empty delimiter: return array with one element (the whole string)
        char* copy = (char*)malloc((size_t)(s_len + 1));
        memcpy(copy, s, (size_t)s_len);
        copy[s_len] = '\0';
        uint8_t* strObj = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
        memcpy(strObj, &copy, sizeof(void*));
        memcpy(strObj + 8, &s_len, sizeof(int64_t));
        strObj[16] = 1;
        bz_string_init_rc(strObj);  // #422: producer's reference, rc := 1
        bz_array_add(arr, &strObj, 8);
        return arr;
    }
    int64_t start = 0;
    for (int64_t i = 0; i <= s_len - delim_len; ) {
        if (memcmp(s + i, delim, (size_t)delim_len) == 0) {
            int64_t partLen = i - start;
            char* part = (char*)malloc((size_t)(partLen + 1));
            memcpy(part, s + start, (size_t)partLen);
            part[partLen] = '\0';
            uint8_t* strObj = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
            memcpy(strObj, &part, sizeof(void*));
            memcpy(strObj + 8, &partLen, sizeof(int64_t));
            strObj[16] = 1;
            bz_string_init_rc(strObj);  // #422: producer's reference, rc := 1
            bz_array_add(arr, &strObj, 8);
            i += delim_len;
            start = i;
        } else {
            i++;
        }
    }
    // Last segment
    int64_t partLen = s_len - start;
    char* part = (char*)malloc((size_t)(partLen + 1));
    memcpy(part, s + start, (size_t)partLen);
    part[partLen] = '\0';
    uint8_t* strObj = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(strObj, &part, sizeof(void*));
    memcpy(strObj + 8, &partLen, sizeof(int64_t));
    strObj[16] = 1;
    bz_string_init_rc(strObj);  // #422: producer's reference, rc := 1
    bz_array_add(arr, &strObj, 8);
    return arr;
}

// per §9.6 Compare(a, b, ignoreCase): ordinal comparison, optionally folding
// ASCII case. Lexicographic over min(len) bytes; equal prefixes break on length.
int32_t bz_string_compare(const char* a, int64_t a_len,
                          const char* b, int64_t b_len, int32_t ignore_case) {
    if (!a) a_len = 0;
    if (!b) b_len = 0;
    int64_t n = a_len < b_len ? a_len : b_len;
    for (int64_t i = 0; i < n; ++i) {
        int ca = (unsigned char)a[i];
        int cb = (unsigned char)b[i];
        if (ignore_case) { ca = tolower(ca); cb = tolower(cb); }
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a_len == b_len) return 0;
    return a_len < b_len ? -1 : 1;
}

// per §9.6 Split(separators: Array<char>): split on any of `nsep` single-code-
// point separators. Width-1 (byte) mode, matching bz_string_split. Each emitted
// segment is a fresh +1 bz.string; the result array is owned by the caller.
static void bz__split_emit(void* arr, const char* s, int64_t start, int64_t end) {
    int64_t partLen = end - start;
    char* part = (char*)malloc((size_t)(partLen + 1));
    memcpy(part, s + start, (size_t)partLen);
    part[partLen] = '\0';
    uint8_t* strObj = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(strObj, &part, sizeof(void*));
    memcpy(strObj + 8, &partLen, sizeof(int64_t));
    strObj[16] = 1;
    bz_string_init_rc(strObj);  // #422: producer's reference, rc := 1
    bz_array_add(arr, &strObj, 8);
}

void* bz_string_split_anychar(const char* s, int64_t s_len,
                              const int32_t* seps, int64_t nsep) {
    void* arr = bz_array_alloc(8, 8);
    // No separators (or empty set): the whole string is the single segment,
    // mirroring bz_string_split's empty-delimiter behaviour.
    if (nsep <= 0) {
        bz__split_emit(arr, s, 0, s_len);
        return arr;
    }
    int64_t start = 0;
    for (int64_t i = 0; i < s_len; ++i) {
        bool isSep = false;
        for (int64_t k = 0; k < nsep; ++k) {
            if ((int32_t)(uint8_t)s[i] == seps[k]) { isSep = true; break; }
        }
        if (isSep) {
            bz__split_emit(arr, s, start, i);
            start = i + 1;
        }
    }
    bz__split_emit(arr, s, start, s_len);  // trailing segment (possibly empty)
    return arr;
}

void bz_strbuilder_to_string(void* parts_arr, char** out_data, int64_t* out_len) {
    auto* arr = (bz_array_t*)parts_arr;
    if (!arr || arr->count == 0) {
        *out_data = (char*)calloc(1, 1);
        *out_len = 0;
        return;
    }
    // Compute total byte length (width 1 for all parts; we treat as bytes)
    int64_t total = 0;
    for (int64_t i = 0; i < arr->count; i++) {
        void* sptr = ((void**)arr->data)[i];
        if (!sptr) continue;
        auto* s = (bz_string_struct*)sptr;
        total += s->len * s->width;
    }
    char* buf = (char*)malloc((size_t)(total + 1));
    int64_t off = 0;
    for (int64_t i = 0; i < arr->count; i++) {
        void* sptr = ((void**)arr->data)[i];
        if (!sptr) continue;
        auto* s = (bz_string_struct*)sptr;
        int64_t bytes = s->len * s->width;
        memcpy(buf + off, s->data, (size_t)bytes);
        off += bytes;
    }
    buf[off] = '\0';
    *out_data = buf;
    *out_len = off;
}

// ---------------------------------------------------------------------------
// StringBuilder mutators (§3.1). Strategy: flatten the parts to one byte buffer,
// apply the operation, and reset the parts array to a single fresh +1 part. The
// builder does not own its parts (Append/Clear never release them), so resetting
// the count and overwriting slot 0 cannot double-free; old part headers are left
// as the existing tolerated leak. Width-1 (byte) mode, matching Append/ToString.

// Replace the entire parts list with one freshly-allocated +1 string holding
// `data[0..len)` (which `bz_sb_set_*` then owns by transferring it in). Resets
// the array's count to 0 and adds the single new part.
static void bz_sb_reset_to(bz_array_t* arr, char* data, int64_t len) {
    uint8_t* strObj = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(strObj, &data, sizeof(void*));
    memcpy(strObj + 8, &len, sizeof(int64_t));
    strObj[16] = 1;                                  // width = 1 (byte mode)
    bz_string_init_rc(strObj);                       // #422: producer ref, rc := 1
    arr->count = 0;                                  // drop old parts (not freed)
    bz_array_add(arr, &strObj, 8);
}

void bz_sb_insert(void* parts_arr, int64_t index, const void* value) {
    auto* arr = (bz_array_t*)parts_arr;
    if (!arr || !value) return;
    char* flat; int64_t flen;
    bz_strbuilder_to_string(parts_arr, &flat, &flen);  // current content
    if (index < 0 || index > flen) { free(flat); return; }  // no-op on OOB
    auto* v = (bz_string_struct*)value;
    int64_t vbytes = v->len * v->width;
    int64_t total = flen + vbytes;
    char* buf = (char*)malloc((size_t)(total + 1));
    memcpy(buf, flat, (size_t)index);                  // prefix
    memcpy(buf + index, v->data, (size_t)vbytes);      // inserted value
    memcpy(buf + index + vbytes, flat + index,
           (size_t)(flen - index));                    // suffix
    buf[total] = '\0';
    free(flat);
    bz_sb_reset_to(arr, buf, total);
}

void bz_sb_remove(void* parts_arr, int64_t start_index, int64_t length) {
    auto* arr = (bz_array_t*)parts_arr;
    if (!arr) return;
    char* flat; int64_t flen;
    bz_strbuilder_to_string(parts_arr, &flat, &flen);
    // §3.1 / §9.6 Remove: start and length must denote a valid range; an
    // out-of-range request is a no-op here (the SB is unchanged).
    if (length < 0 || start_index < 0 || start_index + length > flen) {
        free(flat);
        return;
    }
    int64_t total = flen - length;
    char* buf = (char*)malloc((size_t)(total + 1));
    memcpy(buf, flat, (size_t)start_index);            // prefix
    memcpy(buf + start_index, flat + start_index + length,
           (size_t)(flen - start_index - length));     // suffix
    buf[total] = '\0';
    free(flat);
    bz_sb_reset_to(arr, buf, total);
}

void bz_sb_replace(void* parts_arr, const void* old_value, const void* new_value) {
    auto* arr = (bz_array_t*)parts_arr;
    if (!arr || !old_value || !new_value) return;
    auto* ov = (bz_string_struct*)old_value;
    auto* nv = (bz_string_struct*)new_value;
    int64_t obytes = ov->len * ov->width;
    if (obytes <= 0) return;                            // empty oldValue: no-op
    char* flat; int64_t flen;
    bz_strbuilder_to_string(parts_arr, &flat, &flen);
    int64_t nbytes = nv->len * nv->width;
    // First pass: count occurrences to size the result exactly.
    int64_t occ = 0;
    for (int64_t i = 0; i + obytes <= flen;) {
        if (memcmp(flat + i, ov->data, (size_t)obytes) == 0) { occ++; i += obytes; }
        else i++;
    }
    if (occ == 0) { free(flat); return; }              // nothing to do
    int64_t total = flen + occ * (nbytes - obytes);
    char* buf = (char*)malloc((size_t)(total + 1));
    int64_t off = 0;
    for (int64_t i = 0; i < flen;) {
        if (i + obytes <= flen &&
            memcmp(flat + i, ov->data, (size_t)obytes) == 0) {
            memcpy(buf + off, nv->data, (size_t)nbytes);
            off += nbytes;
            i += obytes;
        } else {
            buf[off++] = flat[i++];
        }
    }
    buf[total] = '\0';
    free(flat);
    bz_sb_reset_to(arr, buf, total);
}

// per §32.5/§32.6: re-encode a bz_string's code points to a fresh,
// null-terminated UTF-8 buffer for the CString FFI boundary.
//
// The internal storage is raw code points at the string's width (1/2/4 bytes
// each); for width>1 those bytes contain interior zeros that truncate a C
// `char*`. Width 1 holds Latin-1 code points (0x00..0xFF) — bytes 0x80..0xFF
// are NOT valid UTF-8 either, so we re-encode every width uniformly rather than
// only width>1, which keeps the conversion correct for all inputs (width-1
// pure-ASCII strings still produce the identical bytes). The caller (the
// width-1 marshalling fast path in codegen) may still pass the raw data
// pointer for pure-ASCII strings; this helper handles the general case.
extern "C" char* bz_string_to_utf8z(const void* strp) {
    if (!strp) {
        char* empty = (char*)malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    const auto* s = (const bz_string_struct*)strp;
    int64_t n = s->len;
    if (n < 0) n = 0;
    // Worst case: every code point is U+10FFFF → 4 UTF-8 bytes; +1 for NUL.
    size_t cap = (size_t)n * 4 + 1;
    char* out = (char*)malloc(cap);
    if (!out) return nullptr;
    size_t off = 0;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t cp;
        if (s->width == 1)      cp = ((const uint8_t*)s->data)[i];
        else if (s->width == 2) cp = ((const uint16_t*)s->data)[i];
        else                    cp = ((const uint32_t*)s->data)[i];
        // UTF-8 encode one code point.
        if (cp <= 0x7F) {
            out[off++] = (char)cp;
        } else if (cp <= 0x7FF) {
            out[off++] = (char)(0xC0 | (cp >> 6));
            out[off++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out[off++] = (char)(0xE0 | (cp >> 12));
            out[off++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[off++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[off++] = (char)(0xF0 | (cp >> 18));
            out[off++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[off++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[off++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[off] = '\0';
    return out;
}

// ---------------------------------------------------------------------------
// Encoding (stdlib §3.2). The abstract `Encoding` base declares
// `GetBytes(s): owned Array<byte>` and `GetString(bytes[, index, count]): string`.
// This runtime implements the UTF-8 concrete encoding (`Encoding.Utf8`), which is
// total and lossless over every Bezel `string` (UTF-8 is the canonical byte form
// referenced throughout §3.4 / §32.5). Utf16/Utf32/Ascii are NOT implemented here
// (their concrete GetBytes/GetString semantics — endianness, surrogate handling,
// and out-of-range behaviour for Ascii — are not pinned down by §3.2); codegen
// rejects those singletons until the spec specifies them.
//
// GetBytes returns a fresh, owned `Array<byte>` (a bz_array_t* with elem_size 1):
// the caller's `owned` binding releases it via the ordinary collection-dispose
// path. GetString returns a fresh +1 managed `string` (rc := 1, exactly like
// every other heap string producer); the byte input is borrowed and untouched.

// Encoding.Utf8.GetBytes(s): walk the string's code points (width 1/2/4) and
// UTF-8-encode each into a fresh byte array. Mirrors bz_string_to_utf8z's encoder
// but emits into a bz_array_t<byte> (no trailing NUL — a byte array carries an
// explicit length, unlike the NUL-terminated FFI buffer).
extern "C" void* bz_encoding_utf8_get_bytes(const void* strp) {
    void* arr = bz_array_alloc(1, 0);  // Array<byte>: elem_size 1
    if (!strp) return arr;
    const auto* s = (const bz_string_struct*)strp;
    int64_t n = s->len;
    if (n < 0) n = 0;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t cp;
        if (s->width == 1)      cp = ((const uint8_t*)s->data)[i];
        else if (s->width == 2) cp = ((const uint16_t*)s->data)[i];
        else                    cp = ((const uint32_t*)s->data)[i];
        uint8_t b;
        if (cp <= 0x7F) {
            b = (uint8_t)cp;                          bz_array_add(arr, &b, 1);
        } else if (cp <= 0x7FF) {
            b = (uint8_t)(0xC0 | (cp >> 6));          bz_array_add(arr, &b, 1);
            b = (uint8_t)(0x80 | (cp & 0x3F));        bz_array_add(arr, &b, 1);
        } else if (cp <= 0xFFFF) {
            b = (uint8_t)(0xE0 | (cp >> 12));         bz_array_add(arr, &b, 1);
            b = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); bz_array_add(arr, &b, 1);
            b = (uint8_t)(0x80 | (cp & 0x3F));        bz_array_add(arr, &b, 1);
        } else {
            b = (uint8_t)(0xF0 | (cp >> 18));          bz_array_add(arr, &b, 1);
            b = (uint8_t)(0x80 | ((cp >> 12) & 0x3F)); bz_array_add(arr, &b, 1);
            b = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));  bz_array_add(arr, &b, 1);
            b = (uint8_t)(0x80 | (cp & 0x3F));         bz_array_add(arr, &b, 1);
        }
    }
    return arr;
}

// Encoding.Utf8.GetString(bytes, index, count): decode `count` UTF-8 bytes
// starting at `index` of the byte array into a fresh +1 managed string. The
// `index`/`count` pair models the §3.2 three-argument overload; the one-argument
// overload passes index=0 and count=Array.Length.
//
// The decoded string is stored at the NARROWEST width (1/2/4) that holds every
// code point — exactly the §4.6 adaptive-storage rule the codegen literal
// emitter (codePointsWidth: ≤0xFF→1, ≤0xFFFF→2, else 4) uses. This is required,
// not merely an optimization: the string-equality codegen compares `len*width`
// bytes via memcmp and is only correct when both operands share a width
// (LlvmBackend_String.cpp comment at emitStringEquals). Producing the same width
// the literal emitter would makes `GetString(GetBytes(s)) == s` compare correctly
// and round-trip exactly. A malformed/truncated UTF-8 sequence decodes its
// leading byte as a literal code point and advances by one (lenient, no OOB read).
extern "C" void* bz_encoding_utf8_get_string(void* arrp, int64_t index,
                                              int64_t count) {
    auto* arr = (bz_array_t*)arrp;
    const uint8_t* data = arr ? (const uint8_t*)arr->data : nullptr;
    int64_t total = arr ? arr->count : 0;
    if (index < 0) index = 0;
    if (count < 0) count = 0;
    if (index > total) index = total;
    if (index + count > total) count = total - index;
    const uint8_t* p = data + index;
    int64_t end = count;

    // Pass 1: decode into a temporary width-4 code-point buffer; track max cp.
    uint32_t* cps = (uint32_t*)malloc((size_t)(count > 0 ? count : 1) * 4);
    int64_t ncp = 0;
    uint32_t maxCp = 0;
    int64_t i = 0;
    while (i < end) {
        uint8_t b0 = p[i];
        uint32_t cp;
        int64_t adv;
        if (b0 < 0x80) {                       // 1-byte: 0xxxxxxx
            cp = b0; adv = 1;
        } else if ((b0 & 0xE0) == 0xC0 && i + 1 < end &&
                   (p[i + 1] & 0xC0) == 0x80) { // 2-byte: 110xxxxx 10xxxxxx
            cp = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(p[i + 1] & 0x3F);
            adv = 2;
        } else if ((b0 & 0xF0) == 0xE0 && i + 2 < end &&
                   (p[i + 1] & 0xC0) == 0x80 && (p[i + 2] & 0xC0) == 0x80) {
            cp = ((uint32_t)(b0 & 0x0F) << 12) |
                 ((uint32_t)(p[i + 1] & 0x3F) << 6) |
                 (uint32_t)(p[i + 2] & 0x3F);
            adv = 3;
        } else if ((b0 & 0xF8) == 0xF0 && i + 3 < end &&
                   (p[i + 1] & 0xC0) == 0x80 && (p[i + 2] & 0xC0) == 0x80 &&
                   (p[i + 3] & 0xC0) == 0x80) {
            cp = ((uint32_t)(b0 & 0x07) << 18) |
                 ((uint32_t)(p[i + 1] & 0x3F) << 12) |
                 ((uint32_t)(p[i + 2] & 0x3F) << 6) |
                 (uint32_t)(p[i + 3] & 0x3F);
            adv = 4;
        } else {
            cp = b0; adv = 1;                  // lenient: pass the byte through
        }
        cps[ncp++] = cp;
        if (cp > maxCp) maxCp = cp;
        i += adv;
    }

    // Pass 2: select the narrowest width (§4.6 adaptive storage) and pack.
    int8_t width = (maxCp <= 0xFF) ? 1 : (maxCp <= 0xFFFF ? 2 : 4);
    void* sdata = malloc((size_t)(ncp > 0 ? ncp : 1) * (size_t)width);
    if (width == 1) {
        uint8_t* d = (uint8_t*)sdata;
        for (int64_t k = 0; k < ncp; ++k) d[k] = (uint8_t)cps[k];
    } else if (width == 2) {
        uint16_t* d = (uint16_t*)sdata;
        for (int64_t k = 0; k < ncp; ++k) d[k] = (uint16_t)cps[k];
    } else {
        uint32_t* d = (uint32_t*)sdata;
        for (int64_t k = 0; k < ncp; ++k) d[k] = cps[k];
    }
    free(cps);

    // Build the bz.string header { data, len, width, rc=1 }.
    uint8_t* s = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(s, &sdata, sizeof(void*));      // data (narrowest-width buffer)
    memcpy(s + 8, &ncp, sizeof(int64_t));  // len (code-point count)
    s[16] = (uint8_t)width;                // width = 1/2/4
    bz_string_init_rc(s);                  // producer's reference, rc := 1
    return s;
}

// ---------------------------------------------------------------------------
// String reference counting (§4.6 / §28.9, issue #422 strings increment).
//
// `string` is runtime-managed: "its memory is reclaimed automatically" (§4.6).
// Strings are immutable, reference-shared LEAVES (§28.8) — a string can never
// hold an owning reference to another heap object — so acyclic reference
// counting is sound and sufficient: a value's data buffer and header are freed
// exactly when the last reference goes away, with no cycle to leak.
//
// The count is a plain (non-atomic) int64 today; this becomes an atomic
// fetch-add/sub when async (multiple threads sharing a string) lands. A single
// inline retain/release keeps the hot path cheap.
//
// SENTINEL: literal/global strings (emitted as constant LLVM globals by
// codegen, never heap-allocated) carry rc == BZ_STRING_RC_SENTINEL; retain and
// release are no-ops on them. A literal must never be free()'d.
//
// null is a no-op for both (a nullable `string?` slot may hold null).

extern "C" void bz_string_retain(void* s) {
    if (!s) return;
    int64_t rc;
    memcpy(&rc, (uint8_t*)s + BZ_STRING_RC_OFFSET, sizeof(int64_t));
    if (rc == BZ_STRING_RC_SENTINEL) return;  // static/literal — never counted
    rc += 1;
    memcpy((uint8_t*)s + BZ_STRING_RC_OFFSET, &rc, sizeof(int64_t));
}

extern "C" void bz_string_release(void* s) {
    if (!s) return;
    int64_t rc;
    memcpy(&rc, (uint8_t*)s + BZ_STRING_RC_OFFSET, sizeof(int64_t));
    if (rc == BZ_STRING_RC_SENTINEL) return;  // static/literal — never freed
    rc -= 1;
    if (rc > 0) {
        memcpy((uint8_t*)s + BZ_STRING_RC_OFFSET, &rc, sizeof(int64_t));
        return;
    }
    // Last reference gone: free the data buffer, then the header. Read the data
    // pointer at offset 0 (the layout's first field). free(NULL) is safe (the
    // empty-string case may carry a null data pointer).
    void* data;
    memcpy(&data, (uint8_t*)s, sizeof(void*));
    free(data);
    free(s);
}

// ---------------------------------------------------------------------------
// Closure-cell reference counting (§27.5 / §28.9, issue #422 closures
// increment).
//
// Function-typed values (lambda closures — capturing or not — and method
// references) are RUNTIME-MANAGED (§28.9): "Runtime management governs the
// function value itself — the heap-allocated closure object that backs the
// function value." Codegen lowers each such value to a heap cell laid out as
//   { void* fn (off 0), void* env (off 8), int64_t rc (off 16) } = 24 bytes.
// `fn` is the function/thunk pointer; `env` is null (non-capturing / static
// method reference) or points at a separately-allocated env block holding the
// capture pointers. The rc was appended AFTER env so fn (off 0) and env (off 8)
// keep their offsets: every codegen call site loads fn at offset 0 and env at
// GEP index 1, both unmoved.
//
// LEAF with respect to ownership. Per §28.9, a closure does NOT own the values
// it captures: "Capturing an owned class-typed local does NOT transfer
// ownership into the closure ... The original `using var` (or other owned
// location) retains the disposal obligation." The §28.3 capture table likewise
// classifies every closure capture as a BORROW. So a closure cell holds only
// borrows; freeing it must free ONLY the cell and its env block — never a
// captured value's storage. (The env block holds the capture pointers; freeing
// the block does not touch the pointees, which are the enclosing frame's shared
// cells / the `using var`'s object, disposed exactly once by their owner.)
// Acyclic reference counting is therefore sound: no owning edge out of a closure
// means no cycle to leak and no captured destructor to run on free.
//
// The count is a plain (non-atomic) int64 today; this becomes an atomic
// fetch-add/sub when async (closures shared across threads) lands.
//
// null is a no-op for both (a nullable `((...) -> ...)?` slot may hold null).

// Byte offset of the rc field inside the closure cell (after fn + env).
static const int64_t BZ_CLOSURE_RC_OFFSET = 16;
// Byte offset of the env pointer inside the closure cell (after fn).
static const int64_t BZ_CLOSURE_ENV_OFFSET = 8;

extern "C" void bz_closure_retain(void* c) {
    if (!c) return;
    int64_t rc;
    memcpy(&rc, (uint8_t*)c + BZ_CLOSURE_RC_OFFSET, sizeof(int64_t));
    rc += 1;
    memcpy((uint8_t*)c + BZ_CLOSURE_RC_OFFSET, &rc, sizeof(int64_t));
}

extern "C" void bz_closure_release(void* c) {
    if (!c) return;
    int64_t rc;
    memcpy(&rc, (uint8_t*)c + BZ_CLOSURE_RC_OFFSET, sizeof(int64_t));
    rc -= 1;
    if (rc > 0) {
        memcpy((uint8_t*)c + BZ_CLOSURE_RC_OFFSET, &rc, sizeof(int64_t));
        return;
    }
    // Last reference gone: free the env block (if any), then the cell. The env
    // block is uniquely owned by this cell (each lowerLambda / makeMethodThunk
    // allocates a fresh env for the one cell it builds), so freeing it here is a
    // single free, no dangler. The env's CONTENTS (capture pointers) are NOT
    // touched — they are borrows the closure does not own (§28.9). free(NULL) is
    // safe (a non-capturing closure carries a null env).
    void* env;
    memcpy(&env, (uint8_t*)c + BZ_CLOSURE_ENV_OFFSET, sizeof(void*));
    free(env);
    free(c);
}

// ---------------------------------------------------------------------------
// Boxed value-type reference counting (§8.6 / §28.9, issue #422 boxes
// increment).
//
// When a VALUE type (record/enum/primitive/nullable value) is used where
// `Object` (or an interface it implements) is expected, codegen BOXES it
// (§8.6): a heap wrapper holds the value. Per §28.9 boxed value types are
// RUNTIME-MANAGED — "exempt from manual disposal," their memory reclaimed
// automatically. Codegen lowers each box to a cell laid out as
//   { void* vtable (off 0), int64_t rc (off 8), value-bytes (off 16) }.
// The vtable word at offset 0 is the boxable type's box vtable (its first slot
// is the type descriptor, so `is`/`as`/interface dispatch recognize the box
// uniformly with a class instance). The rc sits at the FIXED offset 8 — between
// the vtable word and the payload — so ONE runtime offset serves EVERY box no
// matter the boxed value's size; codegen's payload accessors advance past TWO
// words (offset 16). boxValue initializes rc to 1 (the producer's reference).
//
// LEAF with respect to ownership. The boxed value is a VALUE type
// (record/enum/primitive/nullable value): it owns no heap sub-objects that need
// disposing (records/enums/primitives are leaves; a record's only reference
// fields are runtime-managed, §9.7). So release frees ONLY the box wrapper —
// like a string. Acyclic reference counting is therefore sound.
//
// SAFETY — never handed a class instance. A class instance coerced to `Object`
// (the #399 reference path) keeps its bare pointer: boxValue is NOT invoked, no
// box wrapper exists, and codegen never marks such a local box-release-eligible
// (its initializer has a REFERENCE static type, not a boxable value type). So
// bz_box_release is only ever called on a genuine box cell. It does NOT inspect
// the vtable or attempt to tell a box from a class instance at runtime — the
// box-vs-class distinction is resolved at the codegen producer site (the static
// type of the initializer), exactly as bz_string_release / bz_closure_release
// are only emitted for locals codegen knows hold a producer's +1 reference. A
// class instance's bytes ({ vtable, i8 disposed-flag@1, fields }) are therefore
// never read as an rc and never freed here.
//
// The count is a plain (non-atomic) int64 today; this becomes an atomic
// fetch-add/sub when async (boxes shared across threads) lands.
//
// null is a no-op for both (a null T? boxes to a null Object reference, §8.6).

// Byte offset of the rc field inside the box cell (after the vtable word).
static const int64_t BZ_BOX_RC_OFFSET = 8;

extern "C" void bz_box_retain(void* b) {
    if (!b) return;
    int64_t rc;
    memcpy(&rc, (uint8_t*)b + BZ_BOX_RC_OFFSET, sizeof(int64_t));
    rc += 1;
    memcpy((uint8_t*)b + BZ_BOX_RC_OFFSET, &rc, sizeof(int64_t));
}

extern "C" void bz_box_release(void* b) {
    if (!b) return;
    int64_t rc;
    memcpy(&rc, (uint8_t*)b + BZ_BOX_RC_OFFSET, sizeof(int64_t));
    rc -= 1;
    if (rc > 0) {
        memcpy((uint8_t*)b + BZ_BOX_RC_OFFSET, &rc, sizeof(int64_t));
        return;
    }
    // Last reference gone: free the box wrapper. The boxed value is a value type
    // (a leaf — no owned sub-objects), so nothing else is freed; the single
    // calloc'd cell is released exactly once. The box was proven non-escaping by
    // codegen, so no other reference exists — no UAF, no double-free.
    free(b);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// CG35-itable / §13: per-interface itable lookup.
// `itmap` points to a null-terminated flat array
//   [ td*I1, itab*I1, td*I2, itab*I2, ..., NULL ]
// stored in vtable[1] of every class/box vtable. Walk pairs until a td*
// matches `iface_td` and return the adjacent itab* pointer. For a
// well-typed Bezel program this always succeeds (the implementing class
// must have the interface in its itmap); returning NULL here is unreachable.
extern "C" void** bz_itable_lookup(void** itmap, void* iface_td) {
    if (!itmap) return nullptr;
    for (void** p = itmap; *p; p += 2) {
        if (p[0] == iface_td) return (void**)p[1];
    }
    return nullptr;  // unreachable for a typed program
}

// per §33.3 / §22.13 (line 3845): the runtime's interception point for an
// exception that escapes Main with no enclosing handler. Codegen emits a call to
// this from Main's top-level no-handler propagation block (in place of the old
// `ret i32 0`, which conflated abnormal termination with normal completion /
// `return 0`). It reports the failure to stderr and terminates the process with
// a non-zero exit code so the OS / shell / CI observe the abnormal state. Exit
// code 70 = EX_SOFTWARE ("internal software error", BSD sysexits.h): a clean,
// deterministic exit (not a signal); the spec pins only "not the normal-
// completion code 0", so the exact value is an implementation choice (recorded
// in the commit message).
extern "C" void bz_unhandled_exception(void) {
    fputs("Unhandled exception\n", stderr);
    exit(70);
}
