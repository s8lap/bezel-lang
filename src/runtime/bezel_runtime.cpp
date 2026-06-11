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
#include <deque>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <thread>
#include <chrono>
#include <functional>
#include <unordered_set>

// ---------------------------------------------------------------------------
// bz_array_t — dynamic array
// ---------------------------------------------------------------------------

struct bz_array_t {
    void*   data;       // heap buffer holding count*elem_size bytes
    int64_t count;
    int64_t capacity;
    // §28.2 v0.135 / issue #621: checked disposal mode flag. Set to 1 by
    // bz_array_mark_disposed (called instead of bz_array_free in checked builds)
    // so that the indexer guard can detect a use-after-dispose. The struct
    // header is NOT freed in that path (quarantine), keeping this byte valid.
    // Calloc-zeroed on alloc — 0 means live, 1 means disposed.
    int8_t  disposed;
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
        // Dispose elements in REVERSE insertion order per §28.11: ordered owning
        // collections (OwnedArray, OwnedStack, OwnedQueue) dispose last-inserted
        // first. For pointer-sized elements, elem_size = sizeof(void*).
        // bz_array_free with non-null dispose is for pointer-element arrays only
        // (the codegen ensures this).
        for (int64_t i = arr->count - 1; i >= 0; --i) {
            void* elem_ptr = ((void**)arr->data)[i];
            if (elem_ptr) dispose_elem(elem_ptr);
        }
    }
    free(arr->data);
    free(arr);
}

// §28.2 v0.135 / issue #621: checked-disposal-mode variant of bz_array_free.
// Instead of freeing the header block, it:
//  1. Runs element disposal (same as bz_array_free).
//  2. Frees the DATA buffer (contents are gone, no double-free risk).
//  3. Sets disposed = 1 on the HEADER, then leaves the header alive
//     (quarantine) so the disposed flag remains readable for subsequent
//     indexer-guard loads. This is a deliberate memory leak in checked
//     (debug) builds — consistent with the class-instance quarantine used
//     by emitDisposeAndReclaim for §28.2 compliance.
void bz_array_mark_disposed(void* arrp, void (*dispose_elem)(void*)) {
    if (!arrp) return;
    auto* arr = (bz_array_t*)arrp;
    if (arr->disposed) return;  // idempotent
    if (dispose_elem && arr->data) {
        for (int64_t i = arr->count - 1; i >= 0; --i) {
            void* elem_ptr = ((void**)arr->data)[i];
            if (elem_ptr) dispose_elem(elem_ptr);
        }
    }
    free(arr->data);
    arr->data  = nullptr;
    arr->count = 0;
    arr->disposed = 1;
    // Do NOT free(arr) — header stays alive for the disposed-flag guard.
}

// §28.2 v0.135 / issue #621: return the disposed byte for a bz_array_t*.
// Called by checked-mode indexer guards to test use-after-dispose.
// Returns 0 (live) or 1 (disposed).  Null pointer treated as non-disposed
// (a null array hits the bounds-check guard separately).
int8_t bz_array_is_disposed(void* arrp) {
    if (!arrp) return 0;
    return ((bz_array_t*)arrp)->disposed;
}

void bz_array_sort(void* arrp, int64_t elem_size,
                   int (*cmp)(const void*, const void*)) {
    auto* arr = (bz_array_t*)arrp;
    if (arr && arr->count > 1)
        qsort(arr->data, (size_t)arr->count, (size_t)elem_size, cmp);
}

// stdlib §3 / #472: Sort with a closure-bearing comparator.
// `cmpfn(closure, a, b)` wraps the user lambda `(T, T) -> int`.
// We use a thread-local shim so qsort (which takes a plain function pointer)
// can reach the closure without a global lock.
namespace {
thread_local void*  bz_sort_cl_closure = nullptr;
thread_local int  (*bz_sort_cl_fn)(void*, const void*, const void*) = nullptr;
extern "C" int bz_sort_cl_shim(const void* a, const void* b) {
    return bz_sort_cl_fn(bz_sort_cl_closure, a, b);
}
}  // namespace

void bz_array_sort_closure(void* arrp, int64_t elem_size,
                           void* closure,
                           int (*cmpfn)(void*, const void*, const void*)) {
    auto* arr = (bz_array_t*)arrp;
    if (!arr || arr->count < 2) return;
    bz_sort_cl_closure = closure;
    bz_sort_cl_fn      = cmpfn;
    qsort(arr->data, (size_t)arr->count, (size_t)elem_size, bz_sort_cl_shim);
    bz_sort_cl_closure = nullptr;
    bz_sort_cl_fn      = nullptr;
}

// stdlib §3 / #472: Shallow-copy `arr` — allocate a new bz_array_t with the
// same element bytes.  The copy does NOT deep-clone pointer-typed elements;
// for the borrowing Array<T> this is correct (Array<T> does not own its class
// elements — ownership stays with the original owner).  The returned array is
// fresh-owned by the caller; dispose with bz_array_free(result, nullptr).
void* bz_array_copy(void* arrp, int64_t elem_size) {
    auto* src = (bz_array_t*)arrp;
    if (!src) return bz_array_alloc(elem_size, 0);
    auto* dst = (bz_array_t*)bz_array_alloc(elem_size, src->count);
    if (src->count > 0 && src->data) {
        memcpy(dst->data, src->data, (size_t)(src->count * elem_size));
        dst->count = src->count;
        if (dst->capacity < src->count) dst->capacity = src->count;
    }
    return dst;
}

// #429 / §4.6 / §28.9: retain every non-null pointer-sized string element so a
// freshly copied / materialized Array<string> independently co-owns a +1 per
// element. Each slot is a bz_string*; bz_string_retain is a no-op on a
// sentinel/literal or null, so a partial/empty array is safe. This pairs with
// the per-element bz_string_release the array's later free/dispose issues — an
// insert-built array retains at Add (codegen), a copy/materialize-built array
// retains here, so both are symmetric and release exactly their +1 at free.
void bz_array_retain_strings(void* arrp) {
    if (!arrp) return;
    auto* arr = (bz_array_t*)arrp;
    if (!arr->data) return;
    for (int64_t i = 0; i < arr->count; ++i) {
        void* elem = ((void**)arr->data)[i];
        if (elem) bz_string_retain(elem);
    }
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
    // issue #622 / stdlib §4.8: insertion-order tracking for Dictionary<K,V>.
    // order[] holds bucket indices in insertion order; -1 entries are tombstones
    // (from removes). order_count is the used length (including tombstones);
    // order_cap is the allocated size. Iterated in 0..order_count order; -1
    // slots are skipped. Compacted on rehash (tombstones dropped).
    // OwnedDictionary does NOT promise insertion order (§4.9) but shares this
    // struct; we still maintain order for it (cheap, harmless).
    int64_t*    order;       // malloc'd array of bucket indices
    int64_t     order_count; // used length (including -1 tombstones)
    int64_t     order_cap;   // allocated slots in order[]
    // §28.2 v0.135 / issue #627: checked-disposal-mode quarantine flag.
    // Set by bz_dict_mark_disposed (called instead of bz_dict_free in checked
    // builds); tested by bz_dict_is_disposed for method guards.
    int8_t      disposed;
};

static uint32_t fnv1a(const char* s, int64_t len) {
    uint32_t h = 2166136261u;
    for (int64_t i = 0; i < len; i++)
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

// issue #622 / stdlib §4.8: append a bucket index to the insertion-order array.
static void dict_order_append(bz_dict_t* d, int64_t slot) {
    if (d->order_count == d->order_cap) {
        int64_t new_cap = d->order_cap < 8 ? 8 : d->order_cap * 2;
        d->order = (int64_t*)realloc(d->order, (size_t)new_cap * sizeof(int64_t));
        d->order_cap = new_cap;
    }
    d->order[d->order_count++] = slot;
}

// issue #622 / stdlib §4.8: mark a bucket index as removed in the order array
// (set its entry to -1). Linear scan — dictionaries are typically small.
static void dict_order_remove(bz_dict_t* d, int64_t slot) {
    for (int64_t i = 0; i < d->order_count; i++) {
        if (d->order[i] == slot) {
            d->order[i] = -1;
            return;
        }
    }
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

    // issue #622 / stdlib §4.8: build a translation table old_slot → new_slot
    // so we can compact and remap the order array after re-inserting all entries.
    int64_t* slot_map = (int64_t*)malloc((size_t)old_cap * sizeof(int64_t));
    for (int64_t i = 0; i < old_cap; i++) slot_map[i] = -1;

    for (int64_t i = 0; i < old_cap; i++) {
        bz_entry_t& e = old[i];
        if (!e.key || e.deleted) continue;
        // Re-insert into new buckets, record new slot index.
        int64_t pos = e.hash & (new_cap - 1);
        for (int64_t p = 0; p < new_cap; p++) {
            int64_t j = (pos + p) & (new_cap - 1);
            if (!d->buckets[j].key) {
                d->buckets[j] = e;
                d->count++;
                slot_map[i] = j;
                break;
            }
        }
    }
    free(old);

    // Compact the order array: skip tombstones (-1 entries), translate surviving
    // old bucket indices to their new positions via slot_map.
    int64_t new_oc = 0;
    for (int64_t i = 0; i < d->order_count; i++) {
        int64_t old_slot = d->order[i];
        if (old_slot < 0) continue;                  // tombstone: skip
        int64_t new_slot = (old_slot < old_cap) ? slot_map[old_slot] : -1;
        if (new_slot >= 0)
            d->order[new_oc++] = new_slot;
    }
    d->order_count = new_oc;
    free(slot_map);
}

void* bz_dict_alloc(void) {
    auto* d = (bz_dict_t*)calloc(1, sizeof(bz_dict_t));
    d->capacity = 8;
    d->buckets = (bz_entry_t*)calloc(8, sizeof(bz_entry_t));
    // issue #622: order array starts null; lazily grown on first insert.
    // order_count and order_cap are zero from calloc.
    return d;
}

void bz_dict_free(void* dp, void (*dispose_val)(void*)) {
    if (!dp) return;
    auto* d = (bz_dict_t*)dp;
    // §28.2 v0.135 / issue #627: in checked-disposal builds, bz_dict_mark_disposed
    // quarantines the header (disposed=1, buckets freed) without freeing the block.
    // A subsequent scope-end bz_dict_free must be a no-op — skip if already disposed.
    if (d->disposed) return;
    for (int64_t i = 0; i < d->capacity; i++) {
        bz_entry_t& e = d->buckets[i];
        if (e.key && !e.deleted) {
            free(e.key);
            if (dispose_val && e.val) dispose_val(e.val);
        }
    }
    free(d->buckets);
    free(d->order);  // issue #622: release the insertion-order tracking array
    free(d);
}

// §28.2 v0.135 / issue #627: checked-disposal-mode quarantine for bz_dict_t.
// Mirrors bz_array_mark_disposed: disposes values, frees the bucket/order
// arrays, marks disposed=1 on the header, but does NOT free the header block —
// keeping it alive so subsequent method-entry guards can read the disposed flag.
// Idempotent (safe to call twice). Deliberate memory leak in checked (debug)
// builds, consistent with the class-instance and array quarantine patterns.
void bz_dict_mark_disposed(void* dp, void (*dispose_val)(void*)) {
    if (!dp) return;
    auto* d = (bz_dict_t*)dp;
    if (d->disposed) return;  // idempotent
    for (int64_t i = 0; i < d->capacity; i++) {
        bz_entry_t& e = d->buckets[i];
        if (e.key && !e.deleted) {
            free(e.key);
            if (dispose_val && e.val) dispose_val(e.val);
        }
    }
    free(d->buckets);
    d->buckets = nullptr;
    d->count = 0;
    free(d->order);
    d->order = nullptr;
    d->order_count = 0;
    d->disposed = 1;
    // Do NOT free(d) — header stays alive for the disposed-flag guard.
}

// §28.2 v0.135 / issue #627: return non-zero if the dict has been disposed
// via bz_dict_mark_disposed (checked mode only).
int8_t bz_dict_is_disposed(void* dp) {
    if (!dp) return 0;
    return ((bz_dict_t*)dp)->disposed;
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
        // Key already present: update value, no order change (§4.8).
        d->buckets[idx].val = val;
        return;
    }
    // Fresh insertion: record slot in insertion order (§4.8 / issue #622).
    int64_t ins = -(idx + 1);
    d->buckets[ins].key = strndup(k, (size_t)k_len);
    d->buckets[ins].key_len = k_len;
    d->buckets[ins].val = val;
    d->buckets[ins].hash = h;
    d->buckets[ins].deleted = false;
    dict_order_append(d, ins);
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
    dict_order_remove(d, idx);  // issue #622: untrack this slot from insertion order
    free(d->buckets[idx].key);
    d->buckets[idx].key = nullptr;
    d->buckets[idx].deleted = true;
    d->count--;
    return true;
}

// issue M1 / stdlib §4.9: OwnedDictionary<string,V>.Remove(key) must dispose
// the removed owned value (exactly-once disposal), then unlink the entry.
// Mirrors bz_dict_clear's per-entry dispose pattern. The non-owning
// Dictionary.Remove still calls bz_dict_remove_str (no disposer).
bool bz_dict_remove_str_disposing(void* dp, const char* k, int64_t k_len,
                                  void (*dispose_val)(void*)) {
    auto* d = (bz_dict_t*)dp;
    uint32_t h = fnv1a(k, k_len);
    int64_t idx = dict_find(d, k, k_len, h);
    if (idx < 0) return false;
    dict_order_remove(d, idx);  // issue #622: untrack this slot from insertion order
    if (dispose_val && d->buckets[idx].val) dispose_val(d->buckets[idx].val);
    free(d->buckets[idx].key);
    d->buckets[idx].key = nullptr;
    d->buckets[idx].val = nullptr;
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
    auto** keys = (char**)malloc((size_t)(n > 0 ? n : 1) * sizeof(char*));
    auto*  lens = (int64_t*)malloc((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
    int64_t j = 0;
    if (d) {
        // issue #622 / stdlib §4.8: iterate in insertion order via order[].
        for (int64_t oi = 0; oi < d->order_count && j < n; oi++) {
            int64_t slot = d->order[oi];
            if (slot < 0) continue;  // tombstone from a remove
            bz_entry_t& e = d->buckets[slot];
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
    // issue #622: order array starts null; lazily grown on first insert.
    // order_count and order_cap are zero from calloc.
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
        // Key already present: update value, no order change (§4.8).
        d->buckets[idx].val = val;
        return;
    }
    // Fresh insertion: record slot in insertion order (§4.8 / issue #622).
    int64_t ins = -(idx + 1);
    char* kc = (char*)malloc((size_t)d->key_size);
    memcpy(kc, key, (size_t)d->key_size);
    d->buckets[ins].key = kc;
    d->buckets[ins].key_len = d->key_size;
    d->buckets[ins].val = val;
    d->buckets[ins].hash = h;
    d->buckets[ins].deleted = false;
    dict_order_append(d, ins);
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
    dict_order_remove(d, idx);  // issue #622: untrack this slot from insertion order
    free(d->buckets[idx].key);
    d->buckets[idx].key = nullptr;
    d->buckets[idx].deleted = true;
    d->count--;
    return true;
}

// issue M1 / stdlib §4.9: OwnedDictionary<K,V>.Remove(key) for generic keys —
// dispose the removed owned value (exactly-once), then unlink. Mirrors the
// string-key bz_dict_remove_str_disposing; the non-owning Dict uses
// bz_dict_remove_gen (no disposer).
bool bz_dict_remove_gen_disposing(void* dp, const void* key,
                                  void (*dispose_val)(void*)) {
    auto* d = (bz_dict_t*)dp;
    if (!d || !key || d->key_size <= 0) return false;
    uint32_t h = (uint32_t)d->hash_fn(key);
    int64_t idx = dict_find_gen(d, key, h);
    if (idx < 0) return false;
    dict_order_remove(d, idx);  // issue #622: untrack this slot from insertion order
    if (dispose_val && d->buckets[idx].val) dispose_val(d->buckets[idx].val);
    free(d->buckets[idx].key);
    d->buckets[idx].key = nullptr;
    d->buckets[idx].val = nullptr;
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
    // issue #622 / stdlib §4.8: Clear() resets insertion order (§4.8: "Clear()
    // resets the order"). Reuse the allocated order buffer but set count to 0.
    d->order_count = 0;
}

// Enumerate the generic table's keys (issue #370, §4.8 Dictionary.Keys /
// §4.10 Set iteration). Returns a malloc'd array of `n` pointers, each a
// malloc'd copy of one live key's `key_size` bytes. Mirrors bz_dict_keys_str
// (which strdup's string keys) but copies the raw key blob instead, so it
// serves int/enum/string/record keys uniformly.
// issue #622 / stdlib §4.8: iteration is now in insertion order via order[].
void bz_dict_keys_gen(void* dp, void*** keys_out, int64_t* n_out) {
    auto* d = (bz_dict_t*)dp;
    int64_t n = d ? d->count : 0;
    auto** keys = (void**)malloc((size_t)(n > 0 ? n : 1) * sizeof(void*));
    int64_t j = 0;
    if (d && d->key_size > 0) {
        // issue #622 / stdlib §4.8: iterate in insertion order via order[].
        for (int64_t oi = 0; oi < d->order_count && j < n; oi++) {
            int64_t slot = d->order[oi];
            if (slot < 0) continue;  // tombstone from a remove
            bz_entry_t& e = d->buckets[slot];
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

// issue #537 / stdlib §4.8: snapshot value arrays for `for var v in d.Values`.
// Both functions return a malloc'd void** array of `n` slot values (one per live
// entry).  The caller owns and must free() the array; the values themselves are
// owned by the dictionary and must NOT be freed by the caller.
//
// bz_dict_values_str — string-keyed table (key_size == 0).
// issue #622 / stdlib §4.8: values are returned in insertion order via order[].
void bz_dict_values_str(void* dp, void*** vals_out, int64_t* n_out) {
    auto* d = (bz_dict_t*)dp;
    int64_t n = d ? d->count : 0;
    auto** vals = (void**)malloc((size_t)(n > 0 ? n : 1) * sizeof(void*));
    int64_t j = 0;
    if (d) {
        // issue #622 / stdlib §4.8: iterate in insertion order via order[].
        for (int64_t oi = 0; oi < d->order_count && j < n; oi++) {
            int64_t slot = d->order[oi];
            if (slot < 0) continue;  // tombstone from a remove
            bz_entry_t& e = d->buckets[slot];
            if (e.key && !e.deleted)
                vals[j++] = e.val;
        }
    }
    if (vals_out) *vals_out = vals; else free(vals);
    if (n_out)    *n_out    = j;
}

// bz_dict_values_gen — generic-key table (key_size > 0). Mirrors
// bz_dict_values_str; the key_size check guards against null/uninitialized dicts.
// issue #622 / stdlib §4.8: values are returned in insertion order via order[].
void bz_dict_values_gen(void* dp, void*** vals_out, int64_t* n_out) {
    auto* d = (bz_dict_t*)dp;
    int64_t n = d ? d->count : 0;
    auto** vals = (void**)malloc((size_t)(n > 0 ? n : 1) * sizeof(void*));
    int64_t j = 0;
    if (d && d->key_size > 0) {
        // issue #622 / stdlib §4.8: iterate in insertion order via order[].
        for (int64_t oi = 0; oi < d->order_count && j < n; oi++) {
            int64_t slot = d->order[oi];
            if (slot < 0) continue;  // tombstone from a remove
            bz_entry_t& e = d->buckets[slot];
            if (e.key && !e.deleted)
                vals[j++] = e.val;
        }
    }
    if (vals_out) *vals_out = vals; else free(vals);
    if (n_out)    *n_out    = j;
}

// issue #628 / stdlib §4.9 v0.135: OwnedDictionary<K,V>.Drain() per-entry pop.
//
// bz_dict_drain_next_str — string-keyed dict (key_size == 0).
// Pops the next live entry in insertion order from the dict: removes it from
// the bucket (marks deleted) and returns its raw key bytes and value pointer.
// The caller takes ownership of the value; the key bytes are a freshly malloc'd
// copy that the caller must free(). Returns true if an entry was popped; false
// if the dict is empty (drain complete).
//
// The dict's `order[]` array is consumed from the front: we scan forward from
// the current scan position (tracked by a caller-supplied cursor, starting at 0)
// for the first live slot, pop it, and return. Because the caller advances the
// cursor each call, the scan is amortized O(1) per live entry over the full drain.
// The cursor is purely a scan hint; the caller passes a pointer to an int64_t
// that starts at 0 and is updated here.
bool bz_dict_drain_next_str(void* dp, int64_t* cursor,
                             char** key_out, int64_t* key_len_out,
                             void** val_out) {
    auto* d = (bz_dict_t*)dp;
    if (!d || d->count == 0) return false;
    // Advance cursor to the first live slot in the order array.
    while (*cursor < d->order_count) {
        int64_t slot = d->order[*cursor];
        (*cursor)++;
        if (slot < 0) continue;  // tombstone
        bz_entry_t& e = d->buckets[slot];
        if (!e.key || e.deleted) continue;  // already gone
        // Found a live entry: hand ownership of key+val to caller.
        if (key_out)     *key_out     = strndup(e.key, (size_t)e.key_len);
        if (key_len_out) *key_len_out = e.key_len;
        if (val_out)     *val_out     = e.val;
        // Remove from the bucket (mark deleted, free key storage, nil val).
        free(e.key);
        e.key     = nullptr;
        e.val     = nullptr;
        e.deleted = true;
        d->count--;
        return true;
    }
    return false;
}

// bz_dict_drain_next_gen — generic-keyed dict (key_size > 0).
// Mirrors bz_dict_drain_next_str; the key is returned as a malloc'd copy of the
// raw bytes (size = d->key_size). The caller must free() it when done.
bool bz_dict_drain_next_gen(void* dp, int64_t* cursor,
                             void** key_out, void** val_out) {
    auto* d = (bz_dict_t*)dp;
    if (!d || d->count == 0 || d->key_size <= 0) return false;
    while (*cursor < d->order_count) {
        int64_t slot = d->order[*cursor];
        (*cursor)++;
        if (slot < 0) continue;  // tombstone
        bz_entry_t& e = d->buckets[slot];
        if (!e.key || e.deleted) continue;
        // Pop: return copies of key bytes and the value pointer.
        if (key_out) {
            void* kc = malloc((size_t)d->key_size);
            memcpy(kc, e.key, (size_t)d->key_size);
            *key_out = kc;
        }
        if (val_out) *val_out = e.val;
        free(e.key);
        e.key     = nullptr;
        e.val     = nullptr;
        e.deleted = true;
        d->count--;
        return true;
    }
    return false;
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

// issue #429 / §28.9 / §4.10: release every live string element's +1.
// A Set<string> stores each element as a bz_string* in the generic-key
// table's key slot (key_size == sizeof(void*)). This function reads the
// string pointer from each key's raw bytes and calls bz_string_release on
// it BEFORE bz_dict_free frees the key copy. Call this immediately before
// bz_dict_free(d, null) — after this call the RC accounting is discharged;
// the subsequent bz_dict_free reclaims the key copies and the table itself.
// null/empty-table safe; a no-op on a non-generic table.
void bz_set_string_keys_release(void* dp) {
    if (!dp) return;
    auto* d = (bz_dict_t*)dp;
    if (d->key_size != (int64_t)sizeof(void*)) return;  // not a pointer-key set
    for (int64_t i = 0; i < d->capacity; i++) {
        bz_entry_t& e = d->buckets[i];
        if (e.key && !e.deleted) {
            void* strPtr;
            memcpy(&strPtr, e.key, sizeof(void*));
            if (strPtr) bz_string_release(strPtr);
        }
    }
}

// issue #429 / §28.9 / §4.10: like bz_dict_clear(d, null) but also releases
// each live element's string +1 before freeing the key copy. Used by
// Set<string>.Clear() so that cleared string elements are reclaimed (else
// they would leak). The table itself stays allocated and usable afterward,
// as with bz_dict_clear.
void bz_dict_clear_set_strings(void* dp) {
    if (!dp) return;
    auto* d = (bz_dict_t*)dp;
    if (d->key_size != (int64_t)sizeof(void*)) return;
    for (int64_t i = 0; i < d->capacity; i++) {
        bz_entry_t& e = d->buckets[i];
        if (e.key && !e.deleted) {
            // Release the string element (+1 discharge) before freeing key copy.
            void* strPtr;
            memcpy(&strPtr, e.key, sizeof(void*));
            if (strPtr) bz_string_release(strPtr);
            free(e.key);
            // value is a sentinel (1) — nothing to dispose
        }
    }
    memset(d->buckets, 0, (size_t)d->capacity * sizeof(bz_entry_t));
    d->count = 0;
    d->order_count = 0;  // issue #622: reset insertion-order tracking on Clear
}

// issue #429 / §28.9 / §4.10: like bz_dict_remove_gen but releases the
// removed string element's +1 before freeing the key copy. Used by
// Set<string>.Remove() so that the removed element is reclaimed (else it
// would leak). Returns true iff the element was present and removed.
bool bz_dict_remove_set_string(void* dp, const void* key) {
    auto* d = (bz_dict_t*)dp;
    if (!d || !key || d->key_size <= 0) return false;
    uint32_t h = (uint32_t)d->hash_fn(key);
    int64_t idx = dict_find_gen(d, key, h);
    if (idx < 0) return false;
    dict_order_remove(d, idx);  // issue #622: untrack this slot from insertion order
    // Release the string element's +1 before freeing the key copy.
    if (d->buckets[idx].key) {
        void* strPtr;
        memcpy(&strPtr, d->buckets[idx].key, sizeof(void*));
        if (strPtr) bz_string_release(strPtr);
    }
    free(d->buckets[idx].key);
    d->buckets[idx].key = nullptr;
    d->buckets[idx].val = nullptr;
    d->buckets[idx].deleted = true;
    d->count--;
    return true;
}

// ---------------------------------------------------------------------------
// bz_llist_t — doubly-linked list (stdlib §4.6 / §4.7)
// ---------------------------------------------------------------------------

// Node layout: prev*, next*, then elem_size bytes of element value inline.
// The list header tracks head, tail, count, and the fixed elem_size used to
// allocate nodes (so remove/free don't need a separately-passed elem_size).
struct bz_llist_node_t {
    bz_llist_node_t* prev;
    bz_llist_node_t* next;
    // element bytes follow immediately — flexible array trick via alloc
};

struct bz_llist_t {
    bz_llist_node_t* head;
    bz_llist_node_t* tail;
    int64_t          count;
    int64_t          elem_size;
};

// Allocate a node with elem_size bytes of value storage.
static bz_llist_node_t* llist_node_alloc(int64_t elem_size, const void* elem) {
    auto* n = (bz_llist_node_t*)calloc(
        1, sizeof(bz_llist_node_t) + (size_t)elem_size);
    if (elem && elem_size > 0)
        memcpy((char*)n + sizeof(bz_llist_node_t), elem, (size_t)elem_size);
    return n;
}

void* bz_llist_alloc(int64_t elem_size) {
    auto* l = (bz_llist_t*)calloc(1, sizeof(bz_llist_t));
    l->elem_size = elem_size;
    return l;
}

int64_t bz_llist_count(void* listp) {
    return listp ? ((bz_llist_t*)listp)->count : 0;
}

void* bz_llist_add_first(void* listp, const void* elem, int64_t elem_size) {
    auto* l = (bz_llist_t*)listp;
    if (!l) return nullptr;
    bz_llist_node_t* n = llist_node_alloc(elem_size, elem);
    n->next = l->head;
    n->prev = nullptr;
    if (l->head) l->head->prev = n;
    l->head = n;
    if (!l->tail) l->tail = n;
    l->count++;
    return n;
}

void* bz_llist_add_last(void* listp, const void* elem, int64_t elem_size) {
    auto* l = (bz_llist_t*)listp;
    if (!l) return nullptr;
    bz_llist_node_t* n = llist_node_alloc(elem_size, elem);
    n->prev = l->tail;
    n->next = nullptr;
    if (l->tail) l->tail->next = n;
    l->tail = n;
    if (!l->head) l->head = n;
    l->count++;
    return n;
}

void* bz_llist_add_before(void* listp, void* nodep, const void* elem,
                           int64_t elem_size) {
    auto* l = (bz_llist_t*)listp;
    auto* node = (bz_llist_node_t*)nodep;
    if (!l || !node) return nullptr;
    if (node == l->head) return bz_llist_add_first(listp, elem, elem_size);
    bz_llist_node_t* n = llist_node_alloc(elem_size, elem);
    n->next = node;
    n->prev = node->prev;
    if (node->prev) node->prev->next = n;
    node->prev = n;
    l->count++;
    return n;
}

void* bz_llist_add_after(void* listp, void* nodep, const void* elem,
                          int64_t elem_size) {
    auto* l = (bz_llist_t*)listp;
    auto* node = (bz_llist_node_t*)nodep;
    if (!l || !node) return nullptr;
    if (node == l->tail) return bz_llist_add_last(listp, elem, elem_size);
    bz_llist_node_t* n = llist_node_alloc(elem_size, elem);
    n->prev = node;
    n->next = node->next;
    if (node->next) node->next->prev = n;
    node->next = n;
    l->count++;
    return n;
}

// Internal: unlink and free `node`, calling dispose_elem if non-null.
static void llist_unlink(bz_llist_t* l, bz_llist_node_t* node,
                         void (*dispose_elem)(void*)) {
    if (node->prev) node->prev->next = node->next;
    else l->head = node->next;
    if (node->next) node->next->prev = node->prev;
    else l->tail = node->prev;
    l->count--;
    if (dispose_elem) {
        void* vp = (char*)node + sizeof(bz_llist_node_t);
        // For OwnedLinkedList<T: class>, element bytes hold a pointer; load it.
        void* obj = *(void**)vp;
        if (obj) dispose_elem(obj);
    }
    free(node);
}

void bz_llist_remove_first(void* listp, void (*dispose_elem)(void*)) {
    auto* l = (bz_llist_t*)listp;
    if (!l || !l->head) return;
    llist_unlink(l, l->head, dispose_elem);
}

void bz_llist_remove_last(void* listp, void (*dispose_elem)(void*)) {
    auto* l = (bz_llist_t*)listp;
    if (!l || !l->tail) return;
    llist_unlink(l, l->tail, dispose_elem);
}

void bz_llist_remove_node(void* listp, void* nodep,
                           void (*dispose_elem)(void*)) {
    auto* l = (bz_llist_t*)listp;
    auto* node = (bz_llist_node_t*)nodep;
    if (!l || !node) return;
    llist_unlink(l, node, dispose_elem);
}

// Find by memcmp equality on the inline element bytes.
// For class-typed elements (pointer-sized), elem points to a pointer; we
// compare the pointer value (reference identity, per §8.3 / §4.6).
void* bz_llist_find(void* listp, const void* elem, int64_t elem_size) {
    auto* l = (bz_llist_t*)listp;
    if (!l || !elem) return nullptr;
    for (bz_llist_node_t* n = l->head; n; n = n->next) {
        void* vp = (char*)n + sizeof(bz_llist_node_t);
        if (memcmp(vp, elem, (size_t)elem_size) == 0) return n;
    }
    return nullptr;
}

void* bz_llist_first_node(void* listp) {
    return listp ? ((bz_llist_t*)listp)->head : nullptr;
}

void* bz_llist_last_node(void* listp) {
    return listp ? ((bz_llist_t*)listp)->tail : nullptr;
}

void* bz_llist_node_value_ptr(void* nodep) {
    return nodep ? (char*)nodep + sizeof(bz_llist_node_t) : nullptr;
}

void* bz_llist_node_next(void* nodep) {
    return nodep ? ((bz_llist_node_t*)nodep)->next : nullptr;
}

void* bz_llist_node_prev(void* nodep) {
    return nodep ? ((bz_llist_node_t*)nodep)->prev : nullptr;
}

void bz_llist_free(void* listp, void (*dispose_elem)(void*)) {
    auto* l = (bz_llist_t*)listp;
    if (!l) return;
    // Dispose elements in REVERSE insertion order per §28.11: OwnedLinkedList
    // disposes last-inserted first (tail to head). Free nodes in the same
    // reverse pass so no separate traversal is needed.
    bz_llist_node_t* n = l->tail;
    while (n) {
        bz_llist_node_t* prv = n->prev;
        if (dispose_elem) {
            void* vp = (char*)n + sizeof(bz_llist_node_t);
            void* obj = *(void**)vp;
            if (obj) dispose_elem(obj);
        }
        free(n);
        n = prv;
    }
    free(l);
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
// Guid (stdlib §2.6) — 128-bit globally unique identifier.
// Representation: { uint64_t lo, uint64_t hi } (matches LLVM { i64, i64 }).
// NewGuid fills bytes from /dev/urandom and stamps RFC 4122 v4 version/variant
// bits.  ToString produces the canonical 36-char lowercase dashed form.
// Parse / TryParse accept exactly that form.
// ---------------------------------------------------------------------------

// bz_string_hdr: layout must match stringType() in LlvmBackend_impl.hpp.
// { ptr data, i64 len, i8 width, i64 rc } = { char*, int64_t, uint8_t, int64_t }.
// The rc field is at field-index 3 (offset 24 on LP64). BZ_GUID_STRING_RC_INIT is
// the "producer holds one reference" value (§4.6 runtime-managed lifetime).
typedef struct { char* data; int64_t len; uint8_t width; int64_t rc; } bz_string_hdr;
static const int64_t BZ_GUID_STRING_RC_INIT = 1;

void bz_guid_new(bz_guid_t* g) {
    // Fill 16 random bytes from /dev/urandom (available on all POSIX systems
    // including Linux/Fedora 43). getrandom(2) would be slightly better but
    // would require a conditional #include inside a function which is not
    // permitted; /dev/urandom is always suitable for GUID generation.
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) { (void)fread(g, 1, 16, f); fclose(f); }
    // Stamp RFC 4122 v4 version/variant bits.
    // Byte 6 (7th byte from start): version nibble = 0100 (4).
    // Byte 8 (9th byte from start): variant bits = 10xx.
    uint8_t* b = (uint8_t*)g;
    b[6] = (b[6] & 0x0F) | 0x40;   // version 4
    b[8] = (b[8] & 0x3F) | 0x80;   // variant 10xx
}

void* bz_guid_to_string(const bz_guid_t* g) {
    // Canonical form: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (36 chars).
    // The 16 bytes are laid out in the standard UUID field order.
    const uint8_t* b = (const uint8_t*)g;
    char* buf = (char*)malloc(37);  // 36 + NUL
    snprintf(buf, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0], b[1], b[2], b[3],
        b[4], b[5],
        b[6], b[7],
        b[8], b[9],
        b[10], b[11], b[12], b[13], b[14], b[15]);
    // Build a bz_string header wrapping the buffer.
    bz_string_hdr* s = (bz_string_hdr*)calloc(1, sizeof(bz_string_hdr));
    s->data  = buf;
    s->len   = 36;
    s->width = 1;
    s->rc    = BZ_GUID_STRING_RC_INIT;  // producer's reference (§4.6)
    return s;
}

// Helper: decode two lowercase hex chars at pos, return the byte value
// or -1 on non-hex input.
static int guid_parse_hex_byte(const char* s) {
    auto hd = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int hi = hd(s[0]); int lo = hd(s[1]);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

// Parse "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (exactly 36 chars, dashes at
// positions 8, 13, 18, 23).  Returns 1 on success, 0 on format error.
static int32_t guid_parse_str(const char* s, int64_t len, bz_guid_t* out) {
    if (len != 36) return 0;
    if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') return 0;
    uint8_t bytes[16];
    // group 1: 8 hex chars → bytes 0-3
    const int offsets[16][2] = {
        {0,2},{2,2},{4,2},{6,2},
        {9,2},{11,2},
        {14,2},{16,2},
        {19,2},{21,2},
        {24,2},{26,2},{28,2},{30,2},{32,2},{34,2}
    };
    for (int i = 0; i < 16; i++) {
        int v = guid_parse_hex_byte(s + offsets[i][0]);
        if (v < 0) return 0;
        bytes[i] = (uint8_t)v;
    }
    memcpy(out, bytes, 16);
    return 1;
}

int32_t bz_guid_try_parse(const void* str, bz_guid_t* out) {
    const bz_string_hdr* s = (const bz_string_hdr*)str;
    if (!s || !s->data) return 0;
    return guid_parse_str(s->data, s->len, out);
}

int32_t bz_guid_parse(const void* str, bz_guid_t* out) {
    int32_t ok = bz_guid_try_parse(str, out);
    // On failure, zero the output (codegen traps via a separate check).
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}

int32_t bz_guid_equals(const bz_guid_t* a, const bz_guid_t* b) {
    return a->lo == b->lo && a->hi == b->hi ? 1 : 0;
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

// Per §2.5: scalar property extractors from a DateTime tick value.
// All delegate through bz_datetime_ticks_to_parts for calendar accuracy.

int32_t bz_datetime_year(int64_t ticks) {
    int32_t y, mo, d, h, mi, s, ms;
    bz_datetime_ticks_to_parts(ticks, &y, &mo, &d, &h, &mi, &s, &ms);
    return y;
}
int32_t bz_datetime_month(int64_t ticks) {
    int32_t y, mo, d, h, mi, s, ms;
    bz_datetime_ticks_to_parts(ticks, &y, &mo, &d, &h, &mi, &s, &ms);
    return mo;
}
int32_t bz_datetime_day(int64_t ticks) {
    int32_t y, mo, d, h, mi, s, ms;
    bz_datetime_ticks_to_parts(ticks, &y, &mo, &d, &h, &mi, &s, &ms);
    return d;
}
int32_t bz_datetime_hour(int64_t ticks) {
    int32_t y, mo, d, h, mi, s, ms;
    bz_datetime_ticks_to_parts(ticks, &y, &mo, &d, &h, &mi, &s, &ms);
    return h;
}
int32_t bz_datetime_minute(int64_t ticks) {
    int32_t y, mo, d, h, mi, s, ms;
    bz_datetime_ticks_to_parts(ticks, &y, &mo, &d, &h, &mi, &s, &ms);
    return mi;
}
int32_t bz_datetime_second(int64_t ticks) {
    int32_t y, mo, d, h, mi, s, ms;
    bz_datetime_ticks_to_parts(ticks, &y, &mo, &d, &h, &mi, &s, &ms);
    return s;
}
int32_t bz_datetime_millisecond(int64_t ticks) {
    int32_t y, mo, d, h, mi, s, ms;
    bz_datetime_ticks_to_parts(ticks, &y, &mo, &d, &h, &mi, &s, &ms);
    return ms;
}

// DayOfWeek: 0=Sunday...6=Saturday — per §2.5 DayOfWeek enum ordering.
// Uses gmtime's tm_wday (0=Sunday, which matches the Bezel enum).
int32_t bz_datetime_day_of_week(int64_t ticks) {
    int64_t posix_sec = (ticks - POSIX_EPOCH_TICKS) / TICKS_PER_SECOND;
    time_t t = (time_t)posix_sec;
    struct tm* ti = gmtime(&t);
    return (int32_t)ti->tm_wday;  // 0=Sunday ... 6=Saturday
}

// DayOfYear: 1..366 — per §2.5; uses gmtime's tm_yday (0-based) + 1.
int32_t bz_datetime_day_of_year(int64_t ticks) {
    int64_t posix_sec = (ticks - POSIX_EPOCH_TICKS) / TICKS_PER_SECOND;
    time_t t = (time_t)posix_sec;
    struct tm* ti = gmtime(&t);
    return (int32_t)(ti->tm_yday + 1);
}

// Per §2.5: AddYears/AddMonths perform calendar-aware arithmetic.
// We decompose to components, adjust, then recompose.
int64_t bz_datetime_add_years(int64_t ticks, int32_t years) {
    int32_t y, mo, d, h, mi, s, ms;
    bz_datetime_ticks_to_parts(ticks, &y, &mo, &d, &h, &mi, &s, &ms);
    y += years;
    // Clamp Feb 29 in a non-leap year to Feb 28.
    if (mo == 2 && d == 29) {
        bool leap = (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
        if (!leap) d = 28;
    }
    return bz_datetime_parts_to_ticks(y, mo, d, h, mi, s, ms);
}

int64_t bz_datetime_add_months(int64_t ticks, int32_t months) {
    int32_t y, mo, d, h, mi, s, ms;
    bz_datetime_ticks_to_parts(ticks, &y, &mo, &d, &h, &mi, &s, &ms);
    // Convert month to 0-based for arithmetic, then back.
    int32_t total_months = (y * 12 + (mo - 1)) + months;
    y  = total_months / 12;
    mo = total_months % 12 + 1;
    if (mo <= 0) { mo += 12; y--; }
    // Clamp day to the last day of the resulting month.
    static const int32_t days_in_month[] =
        {0, 31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
    int32_t dim = (mo == 2 && leap) ? 29 : days_in_month[mo];
    if (d > dim) d = dim;
    return bz_datetime_parts_to_ticks(y, mo, d, h, mi, s, ms);
}

// ---------------------------------------------------------------------------
// Per §2.5: TimeSpan component extractors.
// Ticks are signed 100-ns units.
// ---------------------------------------------------------------------------

static const int64_t TICKS_PER_MINUTE = 600000000LL;     // 60 * 10_000_000
static const int64_t TICKS_PER_HOUR   = 36000000000LL;   // 3600 * 10_000_000
static const int64_t TICKS_PER_DAY    = 864000000000LL;  // 86400 * 10_000_000

void bz_timespan_components(int64_t ticks,
                             int32_t* days, int32_t* hours,
                             int32_t* minutes, int32_t* seconds,
                             int32_t* ms) {
    // Preserve sign: work with absolute value, then negate if negative.
    int32_t sign = (ticks < 0) ? -1 : 1;
    int64_t t = (ticks < 0) ? -ticks : ticks;
    int64_t d   = t / TICKS_PER_DAY;
    t %= TICKS_PER_DAY;
    int64_t h   = t / TICKS_PER_HOUR;
    t %= TICKS_PER_HOUR;
    int64_t min = t / TICKS_PER_MINUTE;
    t %= TICKS_PER_MINUTE;
    int64_t sec = t / TICKS_PER_SECOND;
    t %= TICKS_PER_SECOND;
    int64_t mss = t / TICKS_PER_MS;
    if (days)    *days    = (int32_t)(sign * d);
    if (hours)   *hours   = (int32_t)(sign * h);
    if (minutes) *minutes = (int32_t)(sign * min);
    if (seconds) *seconds = (int32_t)(sign * sec);
    if (ms)      *ms      = (int32_t)(sign * mss);
}

double bz_timespan_total_days(int64_t ticks) {
    return (double)ticks / (double)TICKS_PER_DAY;
}
double bz_timespan_total_hours(int64_t ticks) {
    return (double)ticks / (double)TICKS_PER_HOUR;
}
double bz_timespan_total_minutes(int64_t ticks) {
    return (double)ticks / (double)TICKS_PER_MINUTE;
}
double bz_timespan_total_seconds(int64_t ticks) {
    return (double)ticks / (double)TICKS_PER_SECOND;
}
double bz_timespan_total_milliseconds(int64_t ticks) {
    return (double)ticks / (double)TICKS_PER_MS;
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

// §33.4: build an Array<string> (bz_array_t* of bz_string*) from the process
// argv, excluding argv[0] (the executable name).  The returned array is
// freshly owned; each element's rc is initialised to 1.  Disposal:
//   bz_array_free(result, bz_string_release)
// argv[0] is excluded per §33.4: "The program's own executable path is not
// included; args[0] is the first user-supplied argument."
void* bz_args_array(int32_t argc, char** argv) {
    int32_t n = (argc > 1) ? (argc - 1) : 0;
    void* arr = bz_array_alloc(8, n);   // elem_size = sizeof(void*) = 8
    for (int32_t i = 0; i < n; ++i) {
        const char* src = argv[i + 1];  // skip argv[0]
        int64_t len = (int64_t)strlen(src);
        char* data = (char*)malloc((size_t)(len + 1));
        memcpy(data, src, (size_t)len);
        data[len] = '\0';
        uint8_t* s = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
        memcpy(s, &data, sizeof(void*));           // data ptr
        memcpy(s + 8, &len, sizeof(int64_t));      // len field
        s[16] = 1;                                  // width = 1 (UTF-8 bytes)
        bz_string_init_rc(s);                       // producer's reference, rc := 1
        bz_array_add(arr, &s, 8);                  // append the bz_string* pointer
    }
    return arr;
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
// Shared<T> / Weak<T> — domain-local shared ownership (§28.13 / stdlib §2.16)
//
// Control block layout: { void* value; long strong; long weak;
//                          void (*dispose_value)(void*); bool disposed }
// Handles (strong and weak) are both pointers to this struct; the compiler
// passes them as `ptr` and calls the functions below.  Everything is
// unsynchronised (domain-local per §28.13 / §30.14 — not sendable).
// ---------------------------------------------------------------------------

struct bz_shared_t {
    void*  value;
    long   strong;
    long   weak;
    void (*dispose_value)(void*);
    int    value_disposed;   // 1 after the value has been disposed (idempotent guard)
    // §28.2 v0.135 / issue #627: checked-disposal-mode quarantine flag.
    // Set by bz_shared_mark_disposed (called from checked-mode explicit Dispose()
    // instead of bz_shared_release). bz_shared_release becomes a no-op when this
    // is set so the control block remains alive (deliberate leak in checked
    // builds) for the disposed-flag guard in bz_shared_is_disposed.
    int8_t handle_disposed;
};

// Allocate a new control block.  strong=1, weak=0. per §28.13 constructor.
void* bz_shared_new(void* value, void (*dispose_value)(void*)) {
    bz_shared_t* cb = (bz_shared_t*)calloc(1, sizeof(bz_shared_t));
    cb->value = value;
    cb->strong = 1;
    cb->weak = 0;
    cb->dispose_value = dispose_value;
    cb->value_disposed = 0;
    return (void*)cb;
}

// Increment the strong count and return the same handle (a new alias). §28.13 Clone.
void* bz_shared_clone(void* handle) {
    bz_shared_t* cb = (bz_shared_t*)handle;
    if (!cb) return nullptr;
    cb->strong++;
    return handle;
}

// Return the underlying value pointer (borrow). §28.13 Get.
void* bz_shared_get(void* handle) {
    bz_shared_t* cb = (bz_shared_t*)handle;
    if (!cb) return nullptr;
    return cb->value;
}

// §28.2 v0.135 / issue #627: checked-disposal-mode quarantine for Shared<T>.
// Called instead of bz_shared_release for the explicit handle.Dispose() in
// checked builds. Disposes the owned value and marks handle_disposed=1, but
// does NOT free the control block — keeping it alive (deliberate leak in
// checked/debug builds) so bz_shared_is_disposed can read the flag afterward.
// Idempotent (safe to call twice). The scope-end bz_shared_release becomes a
// no-op when handle_disposed is set (see bz_shared_release below).
void bz_shared_mark_disposed(void* handle) {
    if (!handle) return;
    bz_shared_t* cb = (bz_shared_t*)handle;
    if (cb->handle_disposed) return;  // idempotent
    cb->handle_disposed = 1;
    if (cb->strong > 0) {
        cb->strong--;
        if (cb->strong == 0 && !cb->value_disposed) {
            cb->value_disposed = 1;
            if (cb->dispose_value && cb->value)
                cb->dispose_value(cb->value);
            cb->value = nullptr;
        }
    }
    // Do NOT free(cb) — block stays alive for the disposed-flag guard.
}

// §28.2 v0.135 / issue #627: return non-zero if the Shared<T> handle has been
// quarantined via bz_shared_mark_disposed (checked-disposal builds only).
int8_t bz_shared_is_disposed(void* handle) {
    if (!handle) return 0;
    return ((bz_shared_t*)handle)->handle_disposed;
}

// Mint a Weak handle (same control block, weak count +1). §28.13 Downgrade.
void* bz_shared_downgrade(void* handle) {
    bz_shared_t* cb = (bz_shared_t*)handle;
    if (!cb) return nullptr;
    cb->weak++;
    return handle;   // same block; bz_weak_t* aliases bz_shared_t*
}

// TryUpgrade: produce a new strong handle if the value is still alive. §28.13.
// Returns 1 on success (strong count was > 0), 0 if the value is gone.
int bz_weak_try_upgrade(void* weak_handle, void** out_handle) {
    bz_shared_t* cb = (bz_shared_t*)weak_handle;
    if (!cb || cb->strong == 0) {
        if (out_handle) *out_handle = nullptr;
        return 0;
    }
    cb->strong++;
    if (out_handle) *out_handle = weak_handle;
    return 1;
}

// Release one strong reference.  When the count hits 0, dispose the value;
// when both counts hit 0 free the control block. §28.13 Dispose / Dispose().
// §28.2 v0.135 / issue #627: in checked-disposal builds, bz_shared_mark_disposed
// quarantines the handle (handle_disposed=1, block NOT freed). A subsequent
// scope-end bz_shared_release call must be a no-op — skip if already quarantined.
void bz_shared_release(void* handle) {
    bz_shared_t* cb = (bz_shared_t*)handle;
    if (!cb) return;
    // Skip if already quarantined by bz_shared_mark_disposed (checked mode).
    if (cb->handle_disposed) return;
    cb->strong--;
    if (cb->strong == 0 && !cb->value_disposed) {
        cb->value_disposed = 1;
        if (cb->dispose_value && cb->value)
            cb->dispose_value(cb->value);
        cb->value = nullptr;
        // Free the block only when there are no outstanding weak handles.
        if (cb->weak == 0)
            free(cb);
    }
}

// Release one weak reference.  Free the block when both counts are 0. §28.13.
void bz_weak_release(void* weak_handle) {
    bz_shared_t* cb = (bz_shared_t*)weak_handle;
    if (!cb) return;
    cb->weak--;
    // The value was already disposed (strong == 0 got here first).
    // Free the block only when this was the last weak handle.
    // §28.2 v0.135 / issue #627: do NOT free if handle_disposed is set —
    // the block was quarantined by bz_shared_mark_disposed (checked builds)
    // and must remain alive for the scope-end bz_shared_release no-op check.
    if (cb->strong == 0 && cb->weak == 0 && !cb->handle_disposed)
        free(cb);
}


// ---------------------------------------------------------------------------
// Task scheduler — pthread-based thread pool + §30.13 serial-per-domain layer
// ---------------------------------------------------------------------------
//
// §30.13 (v0.131+): every Bezel async continuation belongs to an *execution
// domain*.  Within a domain, at most one continuation runs at any instant
// (run-to-completion semantics): the domain serializes its own work while
// pool threads can still serve different domains concurrently (reserved for
// future Domain.Spawn).
//
// Implementation sketch:
//   bz_domain_t — owns a deque of ready continuations, a mutex, and an
//     `active` flag.  When a continuation is enqueued and the domain is NOT
//     active, the domain is marked active and handed (as a unit) to a pool
//     worker.  The worker drains the deque one continuation at a time under
//     the domain mutex, re-checking under lock after each item; when the
//     deque is empty it clears `active` and releases the domain.  If the
//     domain IS already active when a continuation is enqueued, the enqueue
//     just appends — the active runner will pick it up on its next check.
//     This achieves at-most-one-running-continuation-per-domain with the pool
//     providing the parallelism budget for multi-domain programs.
//
//   g_main_domain — the single domain used by all tasks in current single-
//     domain programs.  All bz_task_alloc() tasks attach to it.
//
//   bz_task_t.domain — pointer to the owning domain (always g_main_domain
//     for now; Domain.Spawn will supply a fresh domain).
//
// Inline fast-path in bz_task_then (already-done):
//   The caller of bz_task_then is itself a continuation running inside the
//   domain's active runner.  Firing the next continuation *inline* (on the
//   same C call stack) would be correct for the memory-safety contract, but
//   it would (a) recursively deepen the C stack for every already-done await
//   chain and (b) skip the domain-queue round-trip that lets other
//   continuations that arrived concurrently interleave at the right points.
//   Routing through domain_enqueue is the correct run-to-completion
//   implementation: the current continuation returns to the domain drain loop,
//   which finds the newly-enqueued item and runs it next — a domain-level tail
//   call without stack growth.
//
// bz_task_await_sync (blocking sync-wait for async Main):
//   The main OS thread calls this to block until the top-level Task completes.
//   It parks on the task's condition variable; pool workers running domain
//   drains eventually complete the task (via bz_task_complete → task_notify),
//   which calls notify_all on the task cv, waking the main thread.  The main
//   thread is NOT a domain runner — it holds no domain mutex while waiting —
//   so this cannot deadlock.

// Forward declaration: bz_domain_t drain uses bz_task_t::Cont signature.
struct bz_domain_t;
struct bz_task_t;
static void domain_enqueue(bz_domain_t* dom,
                           void (*fn)(void*, void*), void* task, void* state);

// ---------------------------------------------------------------------------
// bz_domain_t — serial execution unit (§30.13 / #618)
// ---------------------------------------------------------------------------
//
// Lifetime via reference counting (fix #632 — UAF in domain_drain):
//
//   Every C++ holder that can dereference a bz_domain_t* acquires +1 on
//   the refcount and releases -1 when done; the last release (refs → 0)
//   deletes the domain.
//
//   Holders and their acquire / release sites (per fix #632):
//     - drain active: domain_drain acquires +1 at entry, releases -1 after
//       clearing active=false and returning — so the domain is live for the
//       entire drain loop even if a continuation calls bz_domain_spawn and
//       the child (or its SpawnCtx) completes instantly.
//     - SpawnCtx: bz_domain_spawn acquires +1 for the SpawnCtx; entry_cont
//       releases -1 after completing the join task (before deleting SpawnCtx).
//     - task->domain: each bz_task_t that stores a domain pointer holds one
//       ref (acquired when task->domain is written, released in bz_task_free).
//
//   g_main_domain is exempt from deletion: it receives a permanent +1 at
//   creation so its count never reaches 0 in normal operation.
//   bz_scheduler_shutdown deletes it directly after the pool is torn down.

struct bz_domain_t {
    struct Cont { void (*fn)(void*, void*); void* task; void* state; };

    std::deque<Cont>  queue;          // ready-to-run continuations
    std::mutex        mtx;            // guards queue and active
    bool              active = false; // true while a pool worker is draining
    // per fix #632: refcount — every holder acquires +1 and releases -1;
    // last release (refs==1→0) deletes.  Starts at 0; callers add their refs.
    std::atomic<int>  refs{0};

    // Enqueue a continuation.  If the domain is idle, mark it active and
    // return true so the caller submits a drain to the pool.  If already
    // active, just enqueue and return false — the active runner picks it up.
    bool enqueue_and_claim(Cont c) {
        std::lock_guard<std::mutex> lk(mtx);
        queue.push_back(c);
        if (!active) {
            active = true;
            return true;   // caller must submit domain_drain to pool
        }
        return false;      // active runner will pick up the new item
    }
};

static bz_domain_t* g_main_domain = nullptr;

// ---------------------------------------------------------------------------
// domain_ref / domain_release — refcount helpers (fix #632)
// ---------------------------------------------------------------------------

// Acquire a reference to a domain.  per fix #632.
static void domain_ref(bz_domain_t* dom) {
    dom->refs.fetch_add(1, std::memory_order_relaxed);
}

// Release a reference.  per fix #632.
// When the count reaches 0, delete the domain.
// g_main_domain's permanent +1 (from bz_main_domain_get) is released by
// bz_scheduler_shutdown via this same path; the domain is deleted when the
// last outstanding ref (usually the outerTask freed after shutdown) drops to 0.
static void domain_release(bz_domain_t* dom) {
    if (dom->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete dom;
}

// bz_main_domain_get — lazy-init g_main_domain with its permanent +1 ref.
// per fix #632: all lazy-init sites call this instead of bare new bz_domain_t().
// The permanent ref is released by bz_scheduler_shutdown (via domain_release),
// ensuring g_main_domain stays alive until all task refs are also released.
static bz_domain_t* bz_main_domain_get() {
    if (!g_main_domain) {
        g_main_domain = new bz_domain_t();
        domain_ref(g_main_domain);  // per fix #632: permanent ref; released at shutdown
    }
    return g_main_domain;
}

// ---------------------------------------------------------------------------
// bz_task_t
// ---------------------------------------------------------------------------

struct bz_task_t {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool is_error = false;
    char* error_msg = nullptr;
    // §30.9: the ORIGINAL exception object that faulted this task (a Bezel heap
    // object with a leading vtable word). Preserved across the task boundary so
    // an awaiting `try`/`catch` can re-raise it into the handler with the real
    // type (emitIsA needs the object, not just a message). Null when the fault
    // carries only a message (e.g. a runtime-builtin error string). The task
    // does NOT own this object's lifetime: a Bezel exception's ownership
    // transfers to whichever handler catches it (or the unhandled boundary), so
    // bz_task_free must not free it.
    void* error_obj = nullptr;
    void* result = nullptr;
    int64_t result_size = 0;
    // §30.13 (#618): owning execution domain (set at allocation; always
    // g_main_domain for current single-domain programs).
    bz_domain_t* domain = nullptr;
    // Continuation chain (registered by bz_task_then before the task completes)
    struct Cont { void (*fn)(void*, void*); void* state; };
    std::vector<Cont> continuations;
};

// ---------------------------------------------------------------------------
// BzScheduler — POSIX thread pool (workers serve domain drains)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// domain_drain — drains one domain's queue on a pool worker (§30.13 / #618)
// ---------------------------------------------------------------------------
//
// Processes continuations one-at-a-time until the queue is empty, then
// atomically clears `active` and returns.  Re-acquires the mutex between
// continuations so late-arriving items (from timer completions or concurrent
// task completions) are picked up without a new pool submission.
//
// Claim-race safety: the check for empty AND the clearing of `active` are
// ONE mutex-held critical section (no gap between them).  A concurrent
// domain_enqueue that sees active=false will atomically push its item and
// set active=true under the SAME mutex, so the item is always eventually
// drained — either by the existing drain (if it hasn't reached empty yet) or
// by the new drain the enqueue submits (if the existing drain already exited).
//
// Memory ordering: every domain_drain exit (active=false + mutex_unlock)
// happens-before any subsequent domain_enqueue claim (mutex_lock + active=true)
// on the same domain.  The mutex provides a sequentially-consistent
// happens-before edge, so a new drain on a different pool worker always sees
// all memory writes from the previous drain's continuations.
//
// Lifetime: domain_drain acquires a refcount on entry and releases on exit,
// per fix #632 (UAF: entry_cont deleted the child_domain then domain_drain
// looped back and re-locked dom->mtx on freed memory).
static void domain_drain(bz_domain_t* dom) {
    domain_ref(dom);  // per fix #632: drain holds a ref for its entire run
    for (;;) {
        bz_domain_t::Cont c;
        {
            std::lock_guard<std::mutex> lk(dom->mtx);
            if (dom->queue.empty()) {
                // Atomically clear active under the lock.  Any concurrent
                // enqueue that arrives now must acquire the lock first, will
                // see active=false, will set active=true, and will submit a
                // new drain — so no items are lost.
                dom->active = false;
                domain_release(dom);  // per fix #632: release drain ref on exit
                return;
            }
            c = dom->queue.front();
            dom->queue.pop_front();
            // c is popped under the lock while active=true.  Any concurrent
            // enqueue sees active=true and pushes to the back of the queue;
            // the next iteration of this loop will pick it up.
        }
        // Run the continuation WITHOUT holding the domain mutex so timer
        // completions, WhenAll/WhenAny callbacks, and bz_task_then calls can
        // concurrently enqueue to the domain without blocking.  The invariant
        // active=true holds for the entire duration of c.fn(), so any such
        // concurrent enqueue always appends to the queue (and never triggers
        // a second domain_drain submission).
        c.fn(c.task, c.state);
    }
}

// ---------------------------------------------------------------------------
// domain_enqueue — single path through which all continuations are queued
// ---------------------------------------------------------------------------
//
// Safety argument: no two domain_drain instances can ever be active for the
// same domain simultaneously.
//   - enqueue_and_claim atomically pushes the item AND claims the domain
//     (active=true) under dom->mtx.  It returns true (claim granted) only
//     when active was false; it returns false when active was already true.
//   - domain_drain atomically checks for an empty queue AND clears active
//     (active=false) under dom->mtx.  The only way it exits is through the
//     "queue empty ⇒ active=false" branch, which is entirely under the lock;
//     there is no window between the empty-check and the active-clear where
//     another enqueue could be missed.
//   - Because both enqueue_and_claim and the drain-exit branch of domain_drain
//     are fully serialized by dom->mtx, the transitions active false→true and
//     true→false are mutually exclusive.  At most ONE domain_drain instance
//     holds active=true at any time; the pool worker running that drain is
//     the sole serialized executor for the domain.
//
// The `dom=nullptr` path is a guard against pre-init calls (tasks that are
// allocated before bz_scheduler_init / g_main_domain is set).  In practice
// this MUST NOT occur in correct programs: every task-alloc site that runs
// in a multi-threaded context guards its own `if (!g_main_domain)` init.
// Submitting directly to the pool when dom=null BYPASSES domain serialization
// and is reserved only for the degenerate single-threaded or pre-init case.
static void domain_enqueue(bz_domain_t* dom,
                           void (*fn)(void*, void*), void* task, void* state) {
    if (!dom) {
        // Pre-init / no-domain fallback: no domain serialization available.
        // This path must not fire in a normally-initialized program — every
        // bz_task_alloc / bz_task_delay call ensures g_main_domain is set.
        // Bypass is acceptable only for single-threaded (no pool) usage.
        if (g_scheduler)
            g_scheduler->submit([fn, task, state](){ fn(task, state); });
        else
            fn(task, state);
        return;
    }
    bool need_submit = dom->enqueue_and_claim({fn, task, state});
    if (need_submit && g_scheduler)
        g_scheduler->submit([dom](){ domain_drain(dom); });
    else if (need_submit)
        domain_drain(dom);  // no pool: drain inline (single-threaded fallback)
}

// ---------------------------------------------------------------------------
// Scheduler lifecycle
// ---------------------------------------------------------------------------

void bz_scheduler_init(int num_threads) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
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
    // per fix #632: release the permanent ref on g_main_domain via domain_release
    // so the domain is deleted when ALL outstanding task refs (+ this permanent ref)
    // are gone.  The generated main calls bz_task_free(outerTask) AFTER shutdown,
    // which releases the outerTask's domain ref — that final release triggers the
    // delete.  Nulling g_main_domain first lets bz_main_domain_get re-init cleanly
    // on the next bz_scheduler_init (process-reuse in JIT test harness).
    bz_domain_t* main_dom = g_main_domain;
    g_main_domain = nullptr;
    if (main_dom) domain_release(main_dom);  // releases permanent ref; may delete
}

void* bz_task_alloc(void) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    auto* task = new bz_task_t();
    task->domain = g_main_domain;
    domain_ref(g_main_domain);  // per fix #632: task holds a ref on its domain
    return task;
}

// §30.13 (#618): schedule the initial entry-point continuation for a task (the
// state-machine Resume that starts the async method body).  Route through the
// task's domain so the initial run is also serialized within the domain.
void bz_task_schedule(void* tp, void (*fn)(void* task, void* state), void* state) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    if (!g_scheduler) bz_scheduler_init(4); // lazy init
    auto* task = (bz_task_t*)tp;
    bz_domain_t* dom = task->domain ? task->domain : g_main_domain;
    domain_enqueue(dom, fn, task, state);
}

// stdlib §6.7 / §30.10: Task.Delay(ms). Allocate a fresh (incomplete) task and
// post a worker that sleeps `ms` then completes it with no value. The task is
// genuinely incomplete at return, so an `await Task.Delay(...)` hits the suspend
// path (bz_task_then) and resumes via the domain queue when the sleep elapses
// (§30.13 / #618).  The Delay timer runs on a raw pool worker (outside any
// domain), which calls bz_task_complete -> task_notify -> domain_enqueue to
// re-queue the awaiting continuation back into its domain.
void* bz_task_delay(int64_t milliseconds) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    if (!g_scheduler) bz_scheduler_init(4);   // ensure a pool exists
    auto* task = new bz_task_t();
    task->domain = g_main_domain;
    domain_ref(g_main_domain);  // per fix #632: task holds a ref on its domain
    int64_t ms = milliseconds < 0 ? 0 : milliseconds;
    g_scheduler->submit([task, ms]() {
        if (ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        bz_task_complete(task, nullptr, 0);
    });
    return task;
}

static void task_notify(bz_task_t* task) {
    // §30.13 (#439 / #618): wake any sync-awaiter FIRST, then enqueue
    // continuations to the task's DOMAIN queue (not directly to the pool).
    //
    // Ordering is load-bearing for cross-thread safety. A continuation may FREE
    // the task (the await-resume that fired it frees an anonymous producer task —
    // e.g. `await Task.Delay(...)` — via bz_task_free). Once a continuation is
    // enqueued to the domain, a worker can run it and free `task` at any moment,
    // so task_notify must not touch `task` (its mutex / cv) AFTER the enqueue.
    // We therefore: (1) snapshot the continuation list and domain pointer, and
    // (2) notify the cv INSIDE the lock, then (3) enqueue the continuations (and
    // never touch `task` or its fields again) after releasing the lock.
    //
    // notify_all() must be called while holding task->mtx (#445 / §30.5 race fix):
    // a sync-awaiter (bz_task_await_sync) may call bz_task_free immediately on
    // waking, which destroys the condition_variable. Holding the mutex during
    // notify_all ensures the awaiter cannot re-acquire the mutex and proceed to
    // free the task until AFTER notify_all returns.
    std::vector<bz_task_t::Cont> conts;
    bz_domain_t* dom;
    {
        std::lock_guard<std::mutex> lk(task->mtx);
        conts = task->continuations;
        dom   = task->domain;
        task->cv.notify_all();
    }
    // task may be freed by a woken sync-awaiter from this point on; do not
    // touch task->mtx or task->cv again.
    for (auto& c : conts)
        domain_enqueue(dom, c.fn, task, c.state);
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

// §30.9: fault the task with the ORIGINAL exception object (a Bezel heap object).
// The awaiter re-raises this exact object into its enclosing handler so the
// catch arm's type test sees the real exception type. The task does not own the
// object (see bz_task_t::error_obj); ownership stays with the exception itself,
// transferring to the catching handler / unhandled boundary.
void bz_task_complete_error_obj(void* tp, void* error_obj) {
    auto* task = (bz_task_t*)tp;
    { std::lock_guard<std::mutex> lk(task->mtx);
      task->error_obj = error_obj;
      task->is_error = true;
      task->done = true; }
    task_notify(task);
}

// §30.9: retrieve the original exception object (valid after bz_task_is_error).
// Null when the fault carries only a message.
void* bz_task_error_obj(void* tp) {
    return ((bz_task_t*)tp)->error_obj;
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

// §30.13 / bz_task_await_sync: the main OS thread (emitted by the async-Main
// wrapper in LlvmBackend_Async.cpp) blocks here until the top-level Task
// completes.  This function is NOT a domain continuation: the main thread is
// not a domain runner, holds no domain mutex while waiting, and just parks on
// the task's condition variable.  Pool workers running domain drains eventually
// complete the task (via bz_task_complete -> task_notify), which calls
// notify_all on the task cv, waking this thread.  No deadlock is possible:
//   - The main thread holds only task->mtx during cv.wait (released atomically
//     by wait; re-acquired only for the predicate check, then released before
//     returning).
//   - Domain drains hold only dom->mtx while manipulating the deque, never
//     while running continuations.
//   - Lock ordering is always task->mtx then dom->mtx (in task_notify/
//     domain_enqueue), never reversed.
void* bz_task_await_sync(void* tp) {
    auto* task = (bz_task_t*)tp;
    std::unique_lock<std::mutex> lk(task->mtx);
    task->cv.wait(lk, [task]{ return task->done; });
    return task->is_error ? nullptr : task->result;
}

const char* bz_task_error(void* tp) {
    return ((bz_task_t*)tp)->error_msg;
}

// §30.13 (#618): bz_task_then routes the continuation through the task's domain
// queue regardless of the already-done fast path.
//
// When already_done is true the caller is executing as the domain's active
// runner (it is itself a continuation that just ran and is chaining the next
// step via bz_task_then).  Routing through domain_enqueue means: the current
// continuation returns to the domain drain loop, which finds the newly-enqueued
// item and runs it next — a domain-level tail call without stack growth.  This
// is the correct run-to-completion implementation: other continuations that
// arrived concurrently (e.g. from a parallel timer completion on a sub-task)
// can be interleaved at the right points between the already-done items.
void bz_task_then(void* tp, void (*cont)(void* task, void* state), void* state) {
    auto* task = (bz_task_t*)tp;
    bool already_done;
    bz_domain_t* dom;
    {
        std::lock_guard<std::mutex> lk(task->mtx);
        already_done = task->done;
        dom = task->domain;
        if (!already_done) task->continuations.push_back({cont, state});
    }
    if (already_done)
        domain_enqueue(dom, cont, task, state);
}

void bz_task_free(void* tp) {
    auto* task = (bz_task_t*)tp;
    free(task->result);
    free(task->error_msg);
    bz_domain_t* dom = task->domain;
    delete task;
    if (dom) domain_release(dom);  // per fix #632: release task's domain ref
}

// stdlib §6.7 / §30.10: Task.FromResult<T>(value). Allocate a fresh task and
// immediately complete it with the `result_size` bytes at `result`. The task is
// already-done at return; `await` on it takes the already-done fast path (no
// thread-pool submission). result == null / result_size == 0 produces an
// already-completed no-value task (the primitive behind Task.CompletedTask).
void* bz_task_from_result(const void* result, int64_t result_size) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    auto* task = new bz_task_t();
    task->domain = g_main_domain;
    domain_ref(g_main_domain);  // per fix #632: task holds a ref on its domain
    if (result_size > 0 && result) {
        task->result = malloc((size_t)result_size);
        memcpy(task->result, result, (size_t)result_size);
        task->result_size = result_size;
    }
    task->done = true;
    // No continuations to notify — no one registered yet; the task is complete
    // at allocation time.
    return task;
}

// stdlib §6.7 / §30.10: Task.WhenAll(tasks: Array<Task>): Task.
// Returns a new task that completes (with no value) once ALL input tasks are
// done. Uses a shared context with a refcount (number of registered
// continuations) and a completion counter. Every continuation decrements
// the refcount on exit (freeing the context when it reaches 0) and decrements
// the completion counter; the last completion (counter → 0) fires the aggregate.
// The context is heap-allocated once and shared across all continuations via a
// raw pointer (no C++ shared_ptr to stay extern "C" compatible).
//
// §30.9 / #443: fault propagation. If ANY input task is faulted (is_error),
// the aggregate completes with the FIRST observed fault (error_obj), so the
// awaiter's await.fault path re-raises the original exception. A separate
// atomic bool `any_fault` tracks whether a fault was seen; a second atomic
// `first_error_obj` (void*) records the error object of the first faulted
// task. The first faulted continuation wins via a CAS on `any_fault`
// (false → true); subsequent faults are discarded (the aggregate carries one
// representative error, per §30.9). When remaining hits 0, if any_fault is
// set the aggregate completes with error; otherwise it completes with success.
struct BzWhenAllCtx {
    bz_task_t* aggregate;
    std::atomic<int64_t> remaining;      // pending completion count
    std::atomic<int64_t> refcount;       // continuation refcount (= n at init)
    std::atomic<bool> any_fault;         // true once a faulted input is seen
    std::atomic<void*> first_error_obj;  // error_obj of the first faulted task
};

void* bz_task_when_all(void* task_arr) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    auto* arr = (bz_array_t*)task_arr;
    int64_t n = bz_array_len(arr);
    auto* aggregate = new bz_task_t();
    aggregate->domain = g_main_domain;
    domain_ref(g_main_domain);  // per fix #632: task holds a ref on its domain
    if (n == 0) {
        aggregate->done = true;
        return aggregate;
    }
    auto* ctx = new BzWhenAllCtx{aggregate, {n}, {n}, {false}, {nullptr}};
    for (int64_t i = 0; i < n; ++i) {
        void* t = nullptr;
        bz_array_get(arr, i, &t, sizeof(void*));
        bz_task_then(t,
            [](void* completedTask, void* state) {
                auto* c = (BzWhenAllCtx*)state;
                // §30.9 / #443: if this input faulted, attempt a CAS on
                // any_fault (false → true). Only the first faulted continuation
                // wins and records the error object; subsequent faults are
                // discarded. The error object is read here while the task is
                // complete and its fields are stable (no race).
                if (bz_task_is_error(completedTask)) {
                    bool expected = false;
                    if (c->any_fault.compare_exchange_strong(
                            expected, true,
                            std::memory_order_acq_rel,
                            std::memory_order_relaxed)) {
                        // We won the CAS: record the error object.
                        void* errObj = bz_task_error_obj(completedTask);
                        c->first_error_obj.store(errObj, std::memory_order_release);
                    }
                }
                // When ALL inputs are done (remaining → 0), complete the
                // aggregate: success if no fault was recorded, error otherwise.
                if (c->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    if (c->any_fault.load(std::memory_order_acquire)) {
                        void* errObj = c->first_error_obj.load(
                            std::memory_order_acquire);
                        bz_task_complete_error_obj(c->aggregate, errObj);
                    } else {
                        bz_task_complete(c->aggregate, nullptr, 0);
                    }
                }
                if (c->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    delete c;
            },
            ctx);
    }
    return aggregate;
}

// stdlib §6.7 / §30.10: Task.WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>.
// Variant of bz_task_when_all that collects each input task's result (elem_size
// bytes) into a new bz_array_t and completes the aggregate with a ptr-sized
// result (the Array<T> pointer). Fault propagation matches bz_task_when_all:
// the first faulted input's exception propagates; the result array is freed.
//
// Thread safety: each continuation writes to a distinct results[] slot (no
// aliasing), so no mutex is needed for slot stores. The acq_rel fetch_sub on
// `remaining` provides the ordering: the last decrement acquires all prior
// result stores (via the release from each preceding thread's fetch_sub), so
// the final continuation sees the complete results[] before building the array.
struct BzWhenAllResultsCtx {
    bz_task_t* aggregate;
    std::atomic<int64_t> remaining;      // pending completion count
    std::atomic<int64_t> refcount;       // continuation refcount (= n at init)
    std::atomic<bool> any_fault;         // true once a faulted input is seen
    std::atomic<void*> first_error_obj;  // error_obj of the first faulted task
    int64_t n;                           // total task count
    int64_t elem_size;                   // sizeof(T) — bytes per result element
    void** result_bufs;                  // result_bufs[i]: malloc'd copy of task i's result
};

struct BzWhenAllResultsSlot {
    BzWhenAllResultsCtx* ctx;
    int64_t idx;
};

void* bz_task_when_all_results(void* task_arr, int64_t elem_size) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    auto* arr = (bz_array_t*)task_arr;
    int64_t n = bz_array_len(arr);
    auto* aggregate = new bz_task_t();
    aggregate->domain = g_main_domain;
    domain_ref(g_main_domain);  // per fix #632: task holds a ref on its domain
    if (n == 0) {
        // Empty array: complete immediately with an empty Array<T>.
        void* emptyArr = bz_array_alloc(elem_size, 0);
        aggregate->result = malloc(sizeof(void*));
        memcpy(aggregate->result, &emptyArr, sizeof(void*));
        aggregate->result_size = (int64_t)sizeof(void*);
        aggregate->done = true;
        return aggregate;
    }
    // Allocate result buffer slots (one per task). Each slot holds a malloc'd
    // copy of the task's raw result bytes (elem_size bytes).
    void** result_bufs = (void**)calloc((size_t)n, sizeof(void*));
    auto* ctx = new BzWhenAllResultsCtx{
        aggregate, {n}, {n}, {false}, {nullptr}, n, elem_size, result_bufs};
    for (int64_t i = 0; i < n; ++i) {
        void* t = nullptr;
        bz_array_get(arr, i, &t, sizeof(void*));
        // Per-slot: allocate a BzWhenAllResultsSlot to carry (ctx, idx) into
        // the continuation. The slot is freed by the continuation itself.
        auto* slot = new BzWhenAllResultsSlot{ctx, i};
        bz_task_then(t,
            [](void* completedTask, void* state) {
                auto* slot = (BzWhenAllResultsSlot*)state;
                auto* c  = slot->ctx;
                int64_t i = slot->idx;
                delete slot;

                // §30.9 / #443: fault propagation — first faulted task wins.
                if (bz_task_is_error(completedTask)) {
                    bool expected = false;
                    if (c->any_fault.compare_exchange_strong(
                            expected, true,
                            std::memory_order_acq_rel,
                            std::memory_order_relaxed)) {
                        void* errObj = bz_task_error_obj(completedTask);
                        c->first_error_obj.store(errObj, std::memory_order_release);
                    }
                } else {
                    // Copy this task's result bytes into results[i].
                    void* resPtr = bz_task_await_sync(completedTask);
                    if (resPtr && c->elem_size > 0) {
                        void* buf = malloc((size_t)c->elem_size);
                        memcpy(buf, resPtr, (size_t)c->elem_size);
                        c->result_bufs[i] = buf;
                    }
                }

                // When ALL inputs done (remaining → 0): build the result array.
                if (c->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    if (c->any_fault.load(std::memory_order_acquire)) {
                        // Free any collected result bufs (we won't use them).
                        for (int64_t j = 0; j < c->n; ++j) free(c->result_bufs[j]);
                        free(c->result_bufs);
                        void* errObj = c->first_error_obj.load(std::memory_order_acquire);
                        bz_task_complete_error_obj(c->aggregate, errObj);
                    } else {
                        // Build Array<T> from collected result bufs.
                        void* resultArr = bz_array_alloc(c->elem_size, c->n);
                        for (int64_t j = 0; j < c->n; ++j) {
                            if (c->result_bufs[j])
                                bz_array_add(resultArr, c->result_bufs[j],
                                             c->elem_size);
                            else {
                                // Zero-result slot (task had 0-byte result).
                                void* zero = calloc(1, (size_t)c->elem_size);
                                bz_array_add(resultArr, zero, c->elem_size);
                                free(zero);
                            }
                            free(c->result_bufs[j]);
                        }
                        free(c->result_bufs);
                        // Complete the aggregate with the Array<T> pointer.
                        bz_task_complete(c->aggregate, &resultArr, sizeof(void*));
                    }
                }
                if (c->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    delete c;
            },
            slot);
    }
    return aggregate;
}

// stdlib §6.7 / §30.10: Task.WhenAny(tasks: Array<Task>): Task<Task>.
// Returns a new task that completes with a ptr-sized result (the pointer to the
// FIRST task that finishes). Uses a shared context with a once-flag and a
// refcount. Every continuation decrements the refcount on exit; the first to
// fire completes the aggregate via CAS on the once-flag.
struct BzWhenAnyCtx {
    bz_task_t* aggregate;
    std::atomic<bool> fired;         // once-flag: only the first winner fires
    std::atomic<int64_t> refcount;   // continuation refcount (= n at init)
};

void* bz_task_when_any(void* task_arr) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    auto* arr = (bz_array_t*)task_arr;
    int64_t n = bz_array_len(arr);
    auto* aggregate = new bz_task_t();
    aggregate->domain = g_main_domain;
    domain_ref(g_main_domain);  // per fix #632: task holds a ref on its domain
    if (n == 0) {
        aggregate->done = true;
        return aggregate;
    }
    auto* ctx = new BzWhenAnyCtx{aggregate, {false}, {n}};
    for (int64_t i = 0; i < n; ++i) {
        void* t = nullptr;
        bz_array_get(arr, i, &t, sizeof(void*));
        bz_task_then(t,
            [](void* completedTask, void* state) {
                auto* c = (BzWhenAnyCtx*)state;
                bool expected = false;
                if (c->fired.compare_exchange_strong(expected, true,
                        std::memory_order_acq_rel))
                    bz_task_complete(c->aggregate, &completedTask, sizeof(void*));
                if (c->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    delete c;
            },
            ctx);
    }
    return aggregate;
}

// stdlib §6.7 / §30.10: Task.FireAndForget(owned task). Ownership of `task`
// transfers; the caller must not free or reference it again. For already-
// complete tasks, free them immediately (inline) without going through the
// scheduler — this avoids a race where the scheduler is shut down before the
// continuation runs. For incomplete tasks (still running on a worker), register
// a continuation via bz_task_then so the memory is freed when the task completes
// (safe: the continuation runs on the pool before or after shutdown drains the
// queue, and the task struct remains live until then).
void  bz_task_fire_and_forget(void* tp) {
    if (!tp) return;
    auto* task = (bz_task_t*)tp;
    // Check if already done under the lock (same as bz_task_is_done).
    bool already_done;
    { std::lock_guard<std::mutex> lk(task->mtx); already_done = task->done; }
    if (already_done) {
        // Fast path: task is complete; free immediately without the scheduler.
        bz_task_free(tp);
    } else {
        // Slow path: task still running; register a continuation so the worker
        // frees it when it eventually completes.
        bz_task_then(tp,
            [](void* completedTask, void* /*state*/) {
                bz_task_free(completedTask);
            },
            nullptr);
    }
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
// Element hash trampoline (Union/Intersect/Except membership dict): maps an
// element's raw bytes to a uint64 hash value for the generic dict backing.
typedef uint64_t (*bz_hash_fn)(const void* elem);
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
    // stdlib §5 Set-like lazy operators (added milestone):
    BZ_LAZY_UNION,     // §5 Set-like: unique elements from `up` ∪ `down`
    BZ_LAZY_INTERSECT, // §5 Set-like: elements from `up` that are in `down`
    BZ_LAZY_EXCEPT,    // §5 Set-like: elements from `up` not in `down`
    // stdlib §5 Ordering secondary key:
    BZ_LAZY_THENBY,    // §5 Ordering: composite (primary, secondary) sort
    // stdlib §5 Lazy Reverse:
    BZ_LAZY_REVERSE,   // §5 Ordering: materialize then yield backwards
    // per §4.6/§4.7 + §5 fix #450:
    BZ_LAZY_LLIST,     // source = bz_llist_t*, walk nodes head-to-tail
};

struct bz_lazy_t {
    void*   vtable;     // shared bz_lazy_vtable
    int32_t kind;
    int64_t esz;        // element size in bytes (output size for SELECT)
    // Source: either an upstream lazy enumerator (`up`) or an array (`arr`).
    bz_lazy_t* up;      // upstream enumerator (null for ARRAY)
    bz_lazy_t* down;    // second source (CONCAT/ZIP/UNION/INTERSECT/EXCEPT)
    void*   arr;        // bz_array_t* (ARRAY kind)
    int64_t idx;        // ARRAY: current index; TAKE/SKIP: count consumed
    int64_t n;          // TAKE/SKIP threshold
    // Closure + trampoline (WHERE/SELECT/ORDERBY).
    void*   closure;
    bz_pred_fn pred;
    bz_sel_fn  sel;
    bz_key_fn  keyfn;
    bz_eq_fn   eqfn;    // DISTINCT/UNION/INTERSECT/EXCEPT: element equality
    bz_hash_fn hashfn;  // UNION/INTERSECT/EXCEPT: element hash (membership dict)
    bz_flatsel_fn flatsel;  // SELECTMANY: element -> inner lazy enumerator
    bz_comb_fn comb;        // ZIP: (a, b) -> out
    int32_t descending; // ORDERBY/THENBY primary direction
    int64_t up_esz;     // ZIP: byte stride of the `up`/left element
    int64_t down_esz;   // ZIP: byte stride of the `down`/right element
    // Scratch storage for the current element bytes (Current returns cur).
    void*   cur;        // malloc'd, esz bytes
    // DISTINCT: a dynamic array of seen elements.
    void*   seen;       // bz_array_t* of element bytes (DISTINCT) OR
                        // bz_dict_t* (UNION/INTERSECT/EXCEPT: elements seen so far)
    // ORDERBY/THENBY/REVERSE: materialized + sorted snapshot, iterated by index.
    // UNION/INTERSECT/EXCEPT: pre-built dict of `down` elements (membership test).
    void*   sorted;     // bz_array_t* (ORDERBY/THENBY/REVERSE) or bz_dict_t*
    int32_t skipped;    // SKIP/SKIPWHILE: have we already dropped the prefix?
    int32_t materialized;
    int32_t phase;      // CONCAT: 0 = draining `up`, 1 = draining `down`
                        // UNION: 0 = draining `up` (unique), 1 = from `other_keys`
    int32_t stopped;    // TAKEWHILE: predicate has failed; sequence ended
    bz_lazy_t* inner;   // SELECTMANY: current inner enumerator (null = none)
    // #422 / §28.9 / §28.3: when the operator's closure was produced INLINE (a
    // lambda literal — a +1 transient this enumerator owns), codegen sets
    // own_closure := 1 so bz_lazy_dispose releases it; a closure arriving via a
    // variable/field is a BORROW (own_closure stays 0 from calloc) and is never
    // freed here. Mirrors the GetOrAdd factoryIsInlineLambda guard.
    int32_t own_closure;
    // THENBY secondary sort key:
    void*      closure2;   // secondary key closure (THENBY)
    bz_key_fn  keyfn2;    // secondary key trampoline (THENBY)
    int32_t    descending2; // secondary descending flag (THENBY)
    int32_t    own_closure2; // secondary closure ownership (THENBY)
    // UNION phase-1 iteration: pointer to the `down`-side key array snapshot.
    // Built from the `sorted` (other_dict) when `up` is exhausted.
    void**  other_keys;   // malloc'd array of element pointers for UNION phase 1
    int64_t other_n;      // count of other_keys
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
    // LLIST uses `inner` as a bz_llist_node_t* borrow (not an owned enumerator).
    if (e->inner && e->kind != BZ_LAZY_LLIST) bz_lazy_dispose(e->inner);  // SELECTMANY
    if (e->cur) free(e->cur);
    // DISTINCT uses `seen` as a bz_array_t*; UNION/INTERSECT/EXCEPT use it as a
    // bz_dict_t* (membership dict). Both are heap-allocated and must be freed.
    if (e->seen) {
        if (e->kind == BZ_LAZY_UNION || e->kind == BZ_LAZY_INTERSECT ||
            e->kind == BZ_LAZY_EXCEPT)
            bz_dict_free(e->seen, nullptr);  // seen dict
        else
            bz_array_free(e->seen, nullptr);
    }
    // `sorted` is bz_array_t* for ORDERBY/THENBY/REVERSE/SET, bz_dict_t* for
    // UNION/INTERSECT/EXCEPT (the `other` membership dict).
    if (e->sorted) {
        if (e->kind == BZ_LAZY_UNION || e->kind == BZ_LAZY_INTERSECT ||
            e->kind == BZ_LAZY_EXCEPT)
            bz_dict_free(e->sorted, nullptr);  // other dict
        else
            bz_array_free(e->sorted, nullptr);
    }
    // UNION phase-1 other_keys snapshot (if built).
    if (e->other_keys) {
        for (int64_t i = 0; i < e->other_n; i++) free(e->other_keys[i]);
        free(e->other_keys);
    }
    // #422 / §28.9 / §28.3: release the operator's closure ONLY when it was an
    // inline lambda we own (own_closure set by codegen). The closure BORROWS its
    // captures, so this frees only the cell + env, never a capture — sound, no
    // double-dispose. A borrowed (variable-form) closure has own_closure == 0 and
    // is left untouched. Dispose runs exactly once per enumerator (cascaded from
    // the for-loop / ToArray drain), so this is a single release, no double-free.
    if (e->own_closure && e->closure) bz_closure_release(e->closure);
    // THENBY secondary closure release (same ownership convention).
    if (e->own_closure2 && e->closure2) bz_closure_release(e->closure2);
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

// stdlib §5 Ordering: ThenBy secondary-closure ownership. Mirrors
// bz_lazy_own_closure for the secondary sort key closure stored in closure2.
// Called by codegen immediately after bz_lazy_thenby when the secondary key
// argument was an inline lambda literal (own_closure2 := 1 → released at
// bz_lazy_dispose). Returns the enumerator for inline threading. Null-safe.
extern "C" void* bz_lazy_own_closure2(void* lazy) {
    if (lazy) ((bz_lazy_t*)lazy)->own_closure2 = 1;
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

// per §4.6/§4.7 + §5 fix #450: wrap a bz_llist_t* as a lazy source that walks
// nodes head-to-tail. `arr` holds the bz_llist_t*; `inner` is reused (distinct
// kind, no SELECTMANY overlap) to store the current bz_llist_node_t*.
// On first MoveNext `inner` is null → initialized to head. Does not own the list.
extern "C" void* bz_lazy_from_llist(void* llist, int64_t esz) {
    auto* e = bz_lazy_new(BZ_LAZY_LLIST, esz);
    e->arr = llist;     // bz_llist_t*
    e->inner = nullptr; // current node, null = not yet started
    e->materialized = 0;// 0 = not yet advanced to head
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

// stdlib §5 Set-like (lazy): Union/Intersect/Except with deduplication.
// `down` is the `other` IEnumerable<T> lazy source, pre-built by the caller.
// `hashfn` and `eqfn` are element-wise hash/equality trampolines (matching the
// generic-dict contract). At construction time, `down` is drained into `sorted`
// (the `other` membership dict) and disposed; `seen` is the dedup dict for `up`.
//
// Union:     yield unique elements from `up`, then elements from `down` not yet
//            seen. Phase 0 = draining `up` (insert into seen, yield if new).
//            Phase 1 = iterate other_keys snapshot and yield those not in seen.
// Intersect: yield unique elements from `up` that appear in the `other` dict.
// Except:    yield unique elements from `up` that do NOT appear in the `other` dict.
//
// Each needs a unique `seen` dict to prevent repeats on the self side. `sorted`
// holds the pre-built `other` dict (Intersect/Except) or the full other-dict
// (Union phase 1). `other_keys` / `other_n` are used by Union phase-1 iteration.
static void bz_lazy_setop_build_other(bz_lazy_t* e, bz_lazy_t* down) {
    // Materialize `down` into the `other` membership dict (`sorted`).
    e->sorted = bz_dict_alloc_generic(e->esz,
        (uint64_t(*)(const void*))e->hashfn,
        (bool(*)(const void*, const void*))e->eqfn);
    // Drain `down` into the other dict.
    const void* p;
    while ((p = bz_lazy_up_next(down)) != nullptr)
        bz_dict_set_gen(e->sorted, p, (void*)1);
    bz_lazy_dispose(down);
    // Build the `seen` dedup dict (same hash/eq).
    e->seen = bz_dict_alloc_generic(e->esz,
        (uint64_t(*)(const void*))e->hashfn,
        (bool(*)(const void*, const void*))e->eqfn);
}

extern "C" void* bz_lazy_union(void* up, void* down, void* hashfn, void* eqfn) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_UNION, u->esz);
    e->up = u;
    e->hashfn = (bz_hash_fn)hashfn;
    e->eqfn   = (bz_eq_fn)eqfn;
    bz_lazy_setop_build_other(e, (bz_lazy_t*)down);
    e->phase = 0;
    return e;
}

extern "C" void* bz_lazy_intersect(void* up, void* down, void* hashfn, void* eqfn) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_INTERSECT, u->esz);
    e->up = u;
    e->hashfn = (bz_hash_fn)hashfn;
    e->eqfn   = (bz_eq_fn)eqfn;
    bz_lazy_setop_build_other(e, (bz_lazy_t*)down);
    return e;
}

extern "C" void* bz_lazy_except(void* up, void* down, void* hashfn, void* eqfn) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_EXCEPT, u->esz);
    e->up = u;
    e->hashfn = (bz_hash_fn)hashfn;
    e->eqfn   = (bz_eq_fn)eqfn;
    bz_lazy_setop_build_other(e, (bz_lazy_t*)down);
    return e;
}

// stdlib §5 Ordering: ThenBy / ThenByDescending — secondary sort key applied
// to the already-sorted output of an upstream OrderBy. On first MoveNext,
// materializes all upstream elements and stable-sorts by (primary_key, secondary_key)
// composite, accessing the primary key by reaching into the upstream ORDERBY node.
extern "C" void* bz_lazy_thenby(void* up, void* closure2, void* keyfn2,
                                int32_t descending2) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_THENBY, u->esz);
    e->up = u;
    // Copy the primary key from the upstream ORDERBY/THENBY node so the
    // composite sort can apply it without re-visiting the chain structure.
    e->closure   = u->closure;    // primary closure (BORROW, not owned)
    e->keyfn     = u->keyfn;      // primary key trampoline
    e->descending = u->descending; // primary direction
    // Secondary key:
    e->closure2   = closure2;
    e->keyfn2     = (bz_key_fn)keyfn2;
    e->descending2 = descending2;
    return e;
}

// stdlib §5 Ordering: Reverse — materialize the upstream sequence, then yield
// elements backwards. Per §5: "Reverse(): IEnumerable<T>".
extern "C" void* bz_lazy_reverse(void* up) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_REVERSE, u->esz);
    e->up = u;
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
    case BZ_LAZY_UNION: {
        // §5 Set-like: yield unique elements from `up`, then unique elements from
        // `down` (those not already yielded from `up`).
        // Phase 0: drain `up`, yielding elements not yet in `seen` dict.
        if (e->phase == 0) {
            const void* p;
            while ((p = bz_lazy_up_next(e->up)) != nullptr) {
                if (!bz_dict_contains_gen(e->seen, p)) {
                    bz_dict_set_gen(e->seen, p, (void*)1);
                    memcpy(e->cur, p, (size_t)e->esz);
                    return 1;
                }
            }
            // `up` exhausted. Build the other_keys snapshot from `sorted` (other dict).
            bz_dict_keys_gen(e->sorted, &e->other_keys, &e->other_n);
            e->idx = 0;
            e->phase = 1;
        }
        // Phase 1: iterate other_keys snapshot, yield elements not in `seen`.
        while (e->idx < e->other_n) {
            const void* k = e->other_keys[e->idx++];
            if (!bz_dict_contains_gen(e->seen, k)) {
                bz_dict_set_gen(e->seen, k, (void*)1);
                memcpy(e->cur, k, (size_t)e->esz);
                return 1;
            }
        }
        return 0;
    }
    case BZ_LAZY_INTERSECT: {
        // §5 Set-like: yield unique elements from `up` that appear in `other` dict.
        const void* p;
        while ((p = bz_lazy_up_next(e->up)) != nullptr) {
            if (bz_dict_contains_gen(e->sorted, p) &&
                !bz_dict_contains_gen(e->seen, p)) {
                bz_dict_set_gen(e->seen, p, (void*)1);
                memcpy(e->cur, p, (size_t)e->esz);
                return 1;
            }
        }
        return 0;
    }
    case BZ_LAZY_EXCEPT: {
        // §5 Set-like: yield unique elements from `up` that are NOT in `other` dict.
        const void* p;
        while ((p = bz_lazy_up_next(e->up)) != nullptr) {
            if (!bz_dict_contains_gen(e->sorted, p) &&
                !bz_dict_contains_gen(e->seen, p)) {
                bz_dict_set_gen(e->seen, p, (void*)1);
                memcpy(e->cur, p, (size_t)e->esz);
                return 1;
            }
        }
        return 0;
    }
    case BZ_LAZY_THENBY: {
        // §5 Ordering: composite (primary, secondary) sort. On first MoveNext,
        // drain upstream, sort by (primary_key, secondary_key) stably, then
        // iterate from index 0.
        if (!e->materialized) {
            e->sorted = bz_array_alloc(e->esz, 8);
            const void* p;
            while ((p = bz_lazy_up_next(e->up)) != nullptr)
                bz_array_add(e->sorted, p, e->esz);
            auto* arr = (bz_array_t*)e->sorted;
            int64_t cnt = arr->count;
            void* tmp = malloc((size_t)e->esz);
            // Stable insertion sort by (primary_key, secondary_key) composite.
            for (int64_t i = 1; i < cnt; ++i) {
                memcpy(tmp, (char*)arr->data + i * e->esz, (size_t)e->esz);
                int64_t pk_i = e->keyfn  ? e->keyfn(e->closure, tmp)   : 0;
                int64_t sk_i = e->keyfn2 ? e->keyfn2(e->closure2, tmp) : 0;
                int64_t j = i - 1;
                while (j >= 0) {
                    void* jptr = (char*)arr->data + j * e->esz;
                    int64_t pk_j = e->keyfn  ? e->keyfn(e->closure, jptr)   : 0;
                    int64_t sk_j = e->keyfn2 ? e->keyfn2(e->closure2, jptr) : 0;
                    // Compare primary first, then secondary on ties.
                    int shift;
                    if (pk_j != pk_i) {
                        shift = e->descending ? (pk_j < pk_i) : (pk_j > pk_i);
                    } else {
                        shift = e->descending2 ? (sk_j < sk_i) : (sk_j > sk_i);
                    }
                    if (!shift) break;
                    memcpy((char*)arr->data + (j + 1) * e->esz, jptr, (size_t)e->esz);
                    --j;
                }
                memcpy((char*)arr->data + (j + 1) * e->esz, tmp, (size_t)e->esz);
            }
            free(tmp);
            e->idx = 0;
            e->materialized = 1;
        }
        auto* arr = (bz_array_t*)e->sorted;
        if (e->idx >= arr->count) return 0;
        memcpy(e->cur, bz_array_ptr_at(e->sorted, e->idx, e->esz), (size_t)e->esz);
        e->idx++;
        return 1;
    }
    case BZ_LAZY_REVERSE: {
        // §5 Ordering: materialize upstream, then yield in reverse order.
        if (!e->materialized) {
            e->sorted = bz_array_alloc(e->esz, 8);
            const void* p;
            while ((p = bz_lazy_up_next(e->up)) != nullptr)
                bz_array_add(e->sorted, p, e->esz);
            auto* arr = (bz_array_t*)e->sorted;
            e->idx = arr->count - 1;  // start from last element
            e->materialized = 1;
        }
        auto* arr = (bz_array_t*)e->sorted;
        if (e->idx < 0) return 0;
        memcpy(e->cur, bz_array_ptr_at(e->sorted, e->idx, e->esz), (size_t)e->esz);
        e->idx--;
        return 1;
    }
    case BZ_LAZY_LLIST: {
        // per §4.6/§4.7 + §5 fix #450: walk bz_llist_t nodes head-to-tail.
        // `arr` = bz_llist_t*; `inner` = current node (null = not yet started).
        // On the first call, initialize `inner` to the list's head.
        if (!e->materialized) {
            auto* l = (bz_llist_t*)e->arr;
            e->inner = l ? (bz_lazy_t*)l->head : nullptr;
            e->materialized = 1;
        }
        if (!e->inner) return 0;
        auto* node = (bz_llist_node_t*)e->inner;
        const void* vp = (const char*)node + sizeof(bz_llist_node_t);
        memcpy(e->cur, vp, (size_t)e->esz);
        e->inner = (bz_lazy_t*)node->next;
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

// stdlib §5 ToSet: materialize a lazy enumerator into a fresh owned Set<T>.
// Set<T> is dict-backed (bz_dict_t); all elements use the generic-key path
// (hash/eq trampolines). Consumes (drains + disposes) the pipeline.
// `hashfn` and `eqfn` are the element-wise hash/equality trampolines.
extern "C" void* bz_lazy_to_set(void* lazy, int64_t esz, void* hashfn, void* eqfn) {
    auto* e = (bz_lazy_t*)lazy;
    void* set = bz_dict_alloc_generic(esz,
        (uint64_t(*)(const void*))hashfn,
        (bool(*)(const void*, const void*))eqfn);
    void** vt = (void**)e->vtable;
    typedef int32_t (*mn_t)(void*);
    typedef void*   (*cur_t)(void*);
    while (((mn_t)vt[2])(e))
        bz_dict_set_gen(set, ((cur_t)vt[3])(e), (void*)1);
    bz_lazy_dispose(e);
    return set;
}

// stdlib §5 ToDictionary<TKey>(key: (T)->TKey): materialize into a fresh
// Dictionary<TKey, T>. Each element T is stored as value; the key is extracted
// by the `keyfn` trampoline. Uses the generic-key dict path.
// `key_esz` = byte size of TKey; `val_esz` = byte size of T (same as esz).
// `hashfn` and `eqfn` are key-wise trampolines.
// The element T bytes are copied into a fresh heap block and stored as the value.
extern "C" void* bz_lazy_to_dict(void* lazy, int64_t val_esz, int64_t key_esz,
                                 void* keyfn, void* closure,
                                 void* hashfn, void* eqfn) {
    auto* e = (bz_lazy_t*)lazy;
    // The dict is keyed by TKey (key_esz bytes), value is a heap pointer to
    // a T-value copy. This matches the Dictionary<TKey,T> runtime layout
    // (string-key dicts store values inline; generic-key dicts store a void* val).
    void* dict = bz_dict_alloc_generic(key_esz,
        (uint64_t(*)(const void*))hashfn,
        (bool(*)(const void*, const void*))eqfn);
    void** vt = (void**)e->vtable;
    typedef int32_t (*mn_t)(void*);
    typedef void*   (*cur_t)(void*);
    typedef int64_t (*kfn_t)(void* cl, const void* elem);
    // We need to store the key in a local buffer (key_esz may be ≤8 for
    // primitive types — just use a stack buffer of max 8 bytes for the
    // common case, or alloca for larger keys). Since key_esz is always a
    // primitive or record and ≤ sizeof(void*) in most practical cases, a
    // fixed 64-byte scratch covers all key types.
    char key_buf[64] = {};
    while (((mn_t)vt[2])(e)) {
        void* elem = ((cur_t)vt[3])(e);
        // Extract key into key_buf (the keyfn trampoline returns int64 for the
        // common single-int-key path — ToDictionary's key is a scalar TKey).
        int64_t k = ((kfn_t)keyfn)(closure, elem);
        memcpy(key_buf, &k, (size_t)(key_esz < 8 ? key_esz : 8));
        // Value: heap copy of the T element bytes.
        void* val = malloc((size_t)val_esz);
        memcpy(val, elem, (size_t)val_esz);
        bz_dict_set_gen(dict, key_buf, val);
    }
    bz_lazy_dispose(e);
    return dict;
}

// stdlib §5 FirstOrNull / LastOrNull / SingleOrNull: eager terminals returning
// a nullable T (via a {hasValue:i8, value:T} tagged struct for value types, or
// a null pointer sentinel for class types). Since these lower into LLVM IR
// inline (not via a C call), we provide runtime helpers only for the
// lazy-source path where we need to drain the enumerator.
//
// For the lazy-source path, the codegen calls:
//   bz_lazy_first_or_null(lazy, esz, out) → 1 if found (copies into *out), 0 if empty
//   bz_lazy_last_or_null(lazy, esz, out)  → 1 if found (copies into *out), 0 if empty
//   bz_lazy_single_or_null(lazy, esz, out)→ 1 if exactly one (copies into *out),
//                                           0 if empty or more than one
// These drain and dispose the enumerator.
extern "C" int32_t bz_lazy_first_or_null(void* lazy, int64_t esz, void* out) {
    auto* e = (bz_lazy_t*)lazy;
    void** vt = (void**)e->vtable;
    typedef int32_t (*mn_t)(void*);
    typedef void*   (*cur_t)(void*);
    int32_t found = 0;
    if (((mn_t)vt[2])(e)) {
        memcpy(out, ((cur_t)vt[3])(e), (size_t)esz);
        found = 1;
    }
    bz_lazy_dispose(e);
    return found;
}

extern "C" int32_t bz_lazy_last_or_null(void* lazy, int64_t esz, void* out) {
    auto* e = (bz_lazy_t*)lazy;
    void** vt = (void**)e->vtable;
    typedef int32_t (*mn_t)(void*);
    typedef void*   (*cur_t)(void*);
    int32_t found = 0;
    while (((mn_t)vt[2])(e)) {
        memcpy(out, ((cur_t)vt[3])(e), (size_t)esz);
        found = 1;
    }
    bz_lazy_dispose(e);
    return found;
}

extern "C" int32_t bz_lazy_single_or_null(void* lazy, int64_t esz, void* out) {
    auto* e = (bz_lazy_t*)lazy;
    void** vt = (void**)e->vtable;
    typedef int32_t (*mn_t)(void*);
    typedef void*   (*cur_t)(void*);
    int32_t found = 0;
    if (((mn_t)vt[2])(e)) {
        memcpy(out, ((cur_t)vt[3])(e), (size_t)esz);
        found = 1;
        if (((mn_t)vt[2])(e)) found = 2;  // more than one: signal failure
    }
    bz_lazy_dispose(e);
    return (found == 1) ? 1 : 0;
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

// §24.1 / §22.13 (#477 B6): a STATIC event's backing EventSource is a static
// owned field with PROGRAM-EXIT disposal. Track the static sources in a global
// list and free them (handlers + struct) via an atexit handler installed once,
// so the program-exit-disposal obligation is met across every exit path
// (normal return, early return, or an unhandled-exception interceptor that
// still drains atexit). Allocated once per static event at program start.
static std::vector<bz_event_source_t*>* g_static_event_sources = nullptr;

static void bz_free_static_event_sources(void) {
    if (!g_static_event_sources) return;
    for (bz_event_source_t* s : *g_static_event_sources) {
        if (!s) continue;
        free(s->handlers);
        free(s);
    }
    delete g_static_event_sources;
    g_static_event_sources = nullptr;
}

extern "C" void* bz_static_event_source_new(void) {
    auto* s = (bz_event_source_t*)calloc(1, sizeof(bz_event_source_t));
    s->next_token = 1;
    if (!g_static_event_sources) {
        g_static_event_sources = new std::vector<bz_event_source_t*>();
        atexit(bz_free_static_event_sources);
    }
    g_static_event_sources->push_back(s);
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

// ---------------------------------------------------------------------------
// §4.6 / #535: Width-aware (code-point-level) string search/slice helpers.
//
// The internal bz.string representation stores code points at 1/2/4 bytes each
// depending on the maximum code point value in the string (§4.6 adaptive-width
// storage). Byte-level search primitives (memmem, memcmp) operating on the raw
// buffer are WRONG for width>1: passing `len` (code-point count) as a byte
// size reads only the first `len` out of `len*width` actual data bytes.
//
// These helpers accept full bz_string* pointers, read each operand's width
// field, and compare code points by value regardless of storage width. All
// index/length arguments and return values are CODE-POINT counts/indices, not
// byte offsets, matching §9.6's documented semantics.
//
// Helper: read one code point from a string's data buffer at code-point index i.
static inline uint32_t bz_str_cp(const bz_string_struct* s, int64_t i) {
    if (s->width == 1) return ((const uint8_t*)s->data)[i];
    if (s->width == 2) return ((const uint16_t*)s->data)[i];
    return ((const uint32_t*)s->data)[i];
}

// Helper: build a fresh +1 bz_string from a sub-range [start, start+count) of
// source string `s`. The result uses the NARROWEST width that fits all its code
// points (§4.6 adaptive-width storage). This allows comparing a substring of
// an ASCII segment extracted from a wide string against a width=1 literal using
// emitStringEquals's memcmp path (which only works for equal-width operands).
static void* bz_str_slice_cp(const bz_string_struct* s, int64_t start, int64_t count) {
    if (count < 0) count = 0;
    // Scan to find the maximum code point value in the range.
    uint32_t maxCp = 0;
    for (int64_t i = 0; i < count; ++i) {
        uint32_t cp = bz_str_cp(s, start + i);
        if (cp > maxCp) maxCp = cp;
    }
    // Select narrowest width (§4.6 adaptive storage).
    int8_t w = (maxCp <= 0xFF) ? 1 : (maxCp <= 0xFFFF ? 2 : 4);
    size_t byte_count = (size_t)count * (size_t)w;
    // Allocate data buffer (with one width-sized terminator slot).
    void* data = calloc(byte_count + (size_t)w, 1);
    if (w == 1) {
        uint8_t* d = (uint8_t*)data;
        for (int64_t i = 0; i < count; ++i) d[i] = (uint8_t)bz_str_cp(s, start + i);
    } else if (w == 2) {
        uint16_t* d = (uint16_t*)data;
        for (int64_t i = 0; i < count; ++i) d[i] = (uint16_t)bz_str_cp(s, start + i);
    } else {
        uint32_t* d = (uint32_t*)data;
        for (int64_t i = 0; i < count; ++i) d[i] = bz_str_cp(s, start + i);
    }
    uint8_t* strObj = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(strObj, &data, sizeof(void*));
    memcpy(strObj + 8, &count, sizeof(int64_t));
    strObj[16] = (uint8_t)w;
    bz_string_init_rc(strObj);  // #422: producer's reference, rc := 1
    return strObj;
}

// Helper: compare two code-point sequences for equality.
// Returns 1 if they have the same length and the same code point at every
// position, 0 otherwise. Width of each may differ.
static int bz_str_cp_seq_eq(const bz_string_struct* a, int64_t a_start,
                             const bz_string_struct* b, int64_t b_start,
                             int64_t len) {
    for (int64_t i = 0; i < len; ++i) {
        if (bz_str_cp(a, a_start + i) != bz_str_cp(b, b_start + i)) return 0;
    }
    return 1;
}

// per §9.6 / #535: StartsWith(prefix: string): bool — code-point comparison.
// Returns 1 if every code point of `prefix` matches the corresponding leading
// code point of `self`, 0 otherwise. Width mismatch is handled via bz_str_cp.
extern "C" int32_t bz_str_startswith(const void* self_p, const void* prefix_p) {
    if (!self_p || !prefix_p) return 0;
    const auto* s = (const bz_string_struct*)self_p;
    const auto* p = (const bz_string_struct*)prefix_p;
    if (p->len > s->len) return 0;
    return bz_str_cp_seq_eq(s, 0, p, 0, p->len);
}

// per §9.6 / #535: EndsWith(suffix: string): bool — code-point comparison.
extern "C" int32_t bz_str_endswith(const void* self_p, const void* suffix_p) {
    if (!self_p || !suffix_p) return 0;
    const auto* s = (const bz_string_struct*)self_p;
    const auto* suf = (const bz_string_struct*)suffix_p;
    if (suf->len > s->len) return 0;
    int64_t offset = s->len - suf->len;
    return bz_str_cp_seq_eq(s, offset, suf, 0, suf->len);
}

// per §9.6 / #535: Contains(sub: string): bool — code-point sliding window.
// Returns 1 if `sub` appears as a consecutive code-point subsequence anywhere
// in `self`; 0 otherwise.
extern "C" int32_t bz_str_contains(const void* self_p, const void* sub_p) {
    if (!self_p || !sub_p) return 0;
    const auto* s = (const bz_string_struct*)self_p;
    const auto* n = (const bz_string_struct*)sub_p;
    if (n->len == 0) return 1;   // empty needle is always contained (§9.6)
    if (n->len > s->len) return 0;
    int64_t limit = s->len - n->len;
    for (int64_t i = 0; i <= limit; ++i) {
        if (bz_str_cp_seq_eq(s, i, n, 0, n->len)) return 1;
    }
    return 0;
}

// per §9.6 / #535: IndexOf(sub: string): int — first code-point occurrence.
// Returns the code-point index of the first occurrence of `sub` in `self`,
// or -1 if not found. An empty needle returns 0 (per §9.6 "appears at 0").
extern "C" int32_t bz_str_indexof(const void* self_p, const void* sub_p) {
    if (!self_p || !sub_p) return -1;
    const auto* s = (const bz_string_struct*)self_p;
    const auto* n = (const bz_string_struct*)sub_p;
    if (n->len == 0) return 0;
    if (n->len > s->len) return -1;
    int64_t limit = s->len - n->len;
    for (int64_t i = 0; i <= limit; ++i) {
        if (bz_str_cp_seq_eq(s, i, n, 0, n->len)) return (int32_t)i;
    }
    return -1;
}

// per §9.6 / #535: LastIndexOf(sub: string): int — last code-point occurrence.
// Returns the code-point index of the LAST occurrence of `sub` in `self`,
// or -1 if not found.
extern "C" int32_t bz_str_lastindexof(const void* self_p, const void* sub_p) {
    if (!self_p || !sub_p) return -1;
    const auto* s = (const bz_string_struct*)self_p;
    const auto* n = (const bz_string_struct*)sub_p;
    if (n->len == 0) return (int32_t)s->len;  // convention: last+1 position
    if (n->len > s->len) return -1;
    int64_t limit = s->len - n->len;
    for (int64_t i = limit; i >= 0; --i) {
        if (bz_str_cp_seq_eq(s, i, n, 0, n->len)) return (int32_t)i;
    }
    return -1;
}

// per §9.6 / #535: Substring(start, count): owned string — code-point slice.
// Returns a fresh +1 string of `count` code points starting at code-point
// index `start`. Clamps `count` to the available tail if needed.
// Preserves the source string's width (§4.6 per-string storage width).
extern "C" void* bz_str_substring(const void* self_p, int64_t start, int64_t count) {
    if (!self_p) {
        // Return empty string.
        uint8_t* e = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
        void* edata = calloc(1, 1);
        memcpy(e, &edata, sizeof(void*));
        int64_t z = 0;
        memcpy(e + 8, &z, sizeof(int64_t));
        e[16] = 1;
        bz_string_init_rc(e);
        return e;
    }
    const auto* s = (const bz_string_struct*)self_p;
    if (start < 0) start = 0;
    if (start > s->len) start = s->len;
    int64_t avail = s->len - start;
    if (count < 0 || count > avail) count = avail;
    return bz_str_slice_cp(s, start, count);
}

// per §9.6 / #535: Replace(old, new): owned string — code-point-aware.
// Replaces every non-overlapping occurrence of `old_p` in `self_p` with
// `new_p`. The result may have a DIFFERENT width from the source (e.g.
// replacing ASCII content with non-ASCII widens; the output uses the narrowest
// width that fits all resulting code points). For simplicity, the result is
// built into a code-point buffer at width 4, then repacked at narrowest width.
extern "C" void* bz_str_replace(const void* self_p,
                                 const void* old_p,
                                 const void* new_p) {
    if (!self_p) {
        // Return empty string (same as bz_str_substring empty case).
        uint8_t* e = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
        void* edata = calloc(1, 1);
        memcpy(e, &edata, sizeof(void*));
        int64_t z = 0;
        memcpy(e + 8, &z, sizeof(int64_t));
        e[16] = 1;
        bz_string_init_rc(e);
        return e;
    }
    const auto* s   = (const bz_string_struct*)self_p;
    const auto* old = (const bz_string_struct*)old_p;
    const auto* nw  = (const bz_string_struct*)new_p;

    // Empty needle: return a copy of self (§9.6: no-op on empty old).
    if (!old || old->len == 0) {
        return bz_str_slice_cp(s, 0, s->len);
    }

    // Collect result code points into a width-4 scratch buffer.
    // Worst-case: each source CP replaced by nw->len CPs.
    int64_t cap = (s->len + 1) * (nw ? nw->len + 1 : 1) + 1;
    uint32_t* out = (uint32_t*)malloc((size_t)cap * sizeof(uint32_t));
    int64_t olen = 0;
    uint32_t maxCp = 0;

    int64_t i = 0;
    while (i <= s->len - old->len) {
        if (bz_str_cp_seq_eq(s, i, old, 0, old->len)) {
            // Emit replacement.
            if (nw) {
                for (int64_t k = 0; k < nw->len; ++k) {
                    uint32_t cp = bz_str_cp(nw, k);
                    if (cp > maxCp) maxCp = cp;
                    out[olen++] = cp;
                }
            }
            i += old->len;
        } else {
            uint32_t cp = bz_str_cp(s, i);
            if (cp > maxCp) maxCp = cp;
            out[olen++] = cp;
            ++i;
        }
    }
    // Emit remaining tail.
    while (i < s->len) {
        uint32_t cp = bz_str_cp(s, i);
        if (cp > maxCp) maxCp = cp;
        out[olen++] = cp;
        ++i;
    }

    // Pack into narrowest width (§4.6 adaptive storage).
    int8_t w = (maxCp <= 0xFF) ? 1 : (maxCp <= 0xFFFF ? 2 : 4);
    void* data = calloc((size_t)(olen > 0 ? olen : 1) * (size_t)w + (size_t)w, 1);
    if (w == 1) {
        uint8_t* d = (uint8_t*)data;
        for (int64_t k = 0; k < olen; ++k) d[k] = (uint8_t)out[k];
    } else if (w == 2) {
        uint16_t* d = (uint16_t*)data;
        for (int64_t k = 0; k < olen; ++k) d[k] = (uint16_t)out[k];
    } else {
        uint32_t* d = (uint32_t*)data;
        for (int64_t k = 0; k < olen; ++k) d[k] = out[k];
    }
    free(out);

    uint8_t* strObj = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(strObj, &data, sizeof(void*));
    memcpy(strObj + 8, &olen, sizeof(int64_t));
    strObj[16] = (uint8_t)w;
    bz_string_init_rc(strObj);  // #422: producer's reference, rc := 1
    return strObj;
}

// per §9.6 / #535: Split(separator: char) — code-point-aware single separator.
// Splits `self_p` at every code point that equals `sep` (a Unicode scalar).
// Returns an owned bz_array_t* of bz.string*; each segment preserves the
// source string's width. Supersedes bz_string_split_anychar for the char
// overload (which passed raw byte data + code-point count, wrong for width>1).
extern "C" void* bz_str_split_cp(const void* self_p, int32_t sep) {
    void* arr = bz_array_alloc(8, 8);
    if (!self_p) {
        // Return array with one empty-string element.
        void* e = bz_str_substring(nullptr, 0, 0);
        bz_array_add(arr, &e, 8);
        return arr;
    }
    const auto* s = (const bz_string_struct*)self_p;
    int64_t start = 0;
    for (int64_t i = 0; i < s->len; ++i) {
        if ((int32_t)bz_str_cp(s, i) == sep) {
            void* seg = bz_str_slice_cp(s, start, i - start);
            bz_array_add(arr, &seg, 8);
            start = i + 1;
        }
    }
    // Trailing segment (possibly empty).
    void* seg = bz_str_slice_cp(s, start, s->len - start);
    bz_array_add(arr, &seg, 8);
    return arr;
}

// per §9.6 / #535: Split(separator: string) — code-point-aware string delimiter.
// Splits `self_p` at every non-overlapping occurrence of `delim_p`.
// Returns an owned bz_array_t* of bz.string*; each segment preserves the
// source string's width.
extern "C" void* bz_str_split_str(const void* self_p, const void* delim_p) {
    void* arr = bz_array_alloc(8, 8);
    if (!self_p) {
        void* e = bz_str_substring(nullptr, 0, 0);
        bz_array_add(arr, &e, 8);
        return arr;
    }
    const auto* s = (const bz_string_struct*)self_p;
    if (!delim_p) {
        void* seg = bz_str_slice_cp(s, 0, s->len);
        bz_array_add(arr, &seg, 8);
        return arr;
    }
    const auto* d = (const bz_string_struct*)delim_p;
    if (d->len == 0) {
        // Empty delimiter: return array with one element (the whole string).
        void* seg = bz_str_slice_cp(s, 0, s->len);
        bz_array_add(arr, &seg, 8);
        return arr;
    }
    int64_t start = 0;
    int64_t limit = s->len - d->len;
    for (int64_t i = 0; i <= limit; ) {
        if (bz_str_cp_seq_eq(s, i, d, 0, d->len)) {
            void* seg = bz_str_slice_cp(s, start, i - start);
            bz_array_add(arr, &seg, 8);
            start = i + d->len;
            i = start;
        } else {
            ++i;
        }
    }
    // Trailing segment.
    void* seg = bz_str_slice_cp(s, start, s->len - start);
    bz_array_add(arr, &seg, 8);
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
// §28.9 / #439: the count is ATOMIC now that async lands. Async continuations
// resume on any thread-pool thread (§30.13), and a runtime-managed string may be
// captured into an async frame and released on a different thread than the one
// that retained it, so retain/release must be a data-race-free atomic
// read-modify-write. The cheapest sound choice (correctness-first per §28.9) is
// a single __atomic_add_fetch / __atomic_sub_fetch with acq_rel ordering on the
// rc word: the increment publishes the new reference; the final decrement
// synchronizes-with all prior releases so the freeing thread observes every
// writer's stores before it frees. The rc field sits at an 8-byte-aligned offset
// in a malloc/calloc'd header (max-aligned base), so the reinterpreted int64* is
// suitably aligned for a lock-free atomic op. The SENTINEL check is a relaxed
// load (a literal's rc is a compile-time constant, never mutated concurrently).
//
// SENTINEL: literal/global strings (emitted as constant LLVM globals by
// codegen, never heap-allocated) carry rc == BZ_STRING_RC_SENTINEL; retain and
// release are no-ops on them. A literal must never be free()'d.
//
// null is a no-op for both (a nullable `string?` slot may hold null).

extern "C" void bz_string_retain(void* s) {
    if (!s) return;
    int64_t* rcp = (int64_t*)((uint8_t*)s + BZ_STRING_RC_OFFSET);
    if (__atomic_load_n(rcp, __ATOMIC_RELAXED) == BZ_STRING_RC_SENTINEL)
        return;                                // static/literal — never counted
    __atomic_add_fetch(rcp, 1, __ATOMIC_ACQ_REL);
}

extern "C" void bz_string_release(void* s) {
    if (!s) return;
    int64_t* rcp = (int64_t*)((uint8_t*)s + BZ_STRING_RC_OFFSET);
    if (__atomic_load_n(rcp, __ATOMIC_RELAXED) == BZ_STRING_RC_SENTINEL)
        return;                                // static/literal — never freed
    if (__atomic_sub_fetch(rcp, 1, __ATOMIC_ACQ_REL) > 0)
        return;
    // Last reference gone: free the data buffer, then the header. Read the data
    // pointer at offset 0 (the layout's first field). free(NULL) is safe (the
    // empty-string case may carry a null data pointer).
    void* data;
    memcpy(&data, (uint8_t*)s, sizeof(void*));
    free(data);
    free(s);
}

// §8.6 / §12.7 (#417): is the Object-typed value `obj` a string? A string in an
// Object slot is a bare bz_string* (word 0 = char* data); a class / boxed value
// / built-in exception has a vtable pointer at word 0. `vtable_table` is the
// null-terminated array codegen emits listing every vtable/box-vtable global.
// `obj` is a string iff it is non-null and its first word is NOT one of those
// vtables. (Runtime-internal vtables — IEnumerator state machines — are never
// stored in a plain Object slot, so omitting them from the table is sound.)
extern "C" int32_t bz_obj_is_string(void* obj, void** vtable_table) {
    if (!obj) return 0;
    void* word0;
    memcpy(&word0, obj, sizeof(void*));  // obj's first word (vtable or data ptr)
    if (vtable_table) {
        for (void** p = vtable_table; *p != nullptr; ++p)
            if (*p == word0) return 0;   // a known vtable → not a string
    }
    return 1;                            // not a vtable → a string
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
// §28.9 / #439: the count is ATOMIC now that async lands — a closure may be
// captured into an async frame (or passed to a worker that runs it) and released
// on a different thread than the one that retained it (§30.13). A single
// __atomic_add_fetch / __atomic_sub_fetch (acq_rel) on the 8-byte-aligned rc
// word is the cheapest sound read-modify-write; the final decrement
// synchronizes-with prior releases before the env block + cell are freed.
//
// null is a no-op for both (a nullable `((...) -> ...)?` slot may hold null).

// Byte offset of the rc field inside the closure cell (after fn + env).
static const int64_t BZ_CLOSURE_RC_OFFSET = 16;
// Byte offset of the env pointer inside the closure cell (after fn).
static const int64_t BZ_CLOSURE_ENV_OFFSET = 8;

extern "C" void bz_closure_retain(void* c) {
    if (!c) return;
    int64_t* rcp = (int64_t*)((uint8_t*)c + BZ_CLOSURE_RC_OFFSET);
    __atomic_add_fetch(rcp, 1, __ATOMIC_ACQ_REL);
}

extern "C" void bz_closure_release(void* c) {
    if (!c) return;
    int64_t* rcp = (int64_t*)((uint8_t*)c + BZ_CLOSURE_RC_OFFSET);
    if (__atomic_sub_fetch(rcp, 1, __ATOMIC_ACQ_REL) > 0)
        return;
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
// §28.9 / #439: the count is ATOMIC now that async lands — a boxed value may be
// captured into an async frame and released on a different thread than the one
// that retained it (§30.13). A single __atomic_add_fetch / __atomic_sub_fetch
// (acq_rel) on the 8-byte-aligned rc word is the cheapest sound read-modify-
// write; the final decrement synchronizes-with prior releases before the wrapper
// is freed.
//
// null is a no-op for both (a null T? boxes to a null Object reference, §8.6).

// Byte offset of the rc field inside the box cell (after the vtable word).
static const int64_t BZ_BOX_RC_OFFSET = 8;

extern "C" void bz_box_retain(void* b) {
    if (!b) return;
    int64_t* rcp = (int64_t*)((uint8_t*)b + BZ_BOX_RC_OFFSET);
    __atomic_add_fetch(rcp, 1, __ATOMIC_ACQ_REL);
}

extern "C" void bz_box_release(void* b) {
    if (!b) return;
    int64_t* rcp = (int64_t*)((uint8_t*)b + BZ_BOX_RC_OFFSET);
    if (__atomic_sub_fetch(rcp, 1, __ATOMIC_ACQ_REL) > 0)
        return;
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

// fix #492 / §31.1 / §2.6: the single process-wide in-flight exception pointer.
// Codegen emits ExternalLinkage references to "bz_exc" in every compiled module
// (library and main); the linker merges them all to this one definition.
// Null = no exception in flight; non-null = heap-allocated exception object.
// The symbol name is "bz_exc" (valid C identifier, extern "C" suppresses
// C++ name mangling). The previous per-module name "bz$exc" was InternalLinkage
// (BSS) — the $ was invisible externally and is dropped now that the symbol is
// external.
extern "C" void* bz_exc = nullptr;

// ---------------------------------------------------------------------------
// CancellationToken / CancellationTokenSource — stdlib §6.1 / spec §30.8.
//
// Layout:
//   bz_cancel_state_t  — the shared cancellation flag cell.  Allocated by the
//                        source; borrowed (by pointer) by every derived token.
//                        Kept alive by a simple reference count so a token read
//                        after source disposal is safe (flag remains readable;
//                        the spec requires the source to outlive tokens but
//                        defensive runtime code should not crash on violation).
//   bz_cancel_source_t — the CancellationTokenSource object.  Owns one
//                        bz_cancel_state_t (the initial rc is 1 for the source's
//                        reference; bz_cts_token adds one more for each token
//                        handle handed out).
//
// CancellationToken handle = void* == bz_cancel_state_t*.
// CancellationToken.None  = null pointer.
// ---------------------------------------------------------------------------

struct bz_cancel_state_t {
    int32_t cancelled;   // 0 = live, 1 = cancelled
    int32_t rc;          // reference count (source + outstanding tokens)
};

struct bz_cancel_source_t {
    bz_cancel_state_t* state;
    bool disposed;
};

// Internal helpers — not exported.
static void bz_cancel_state_retain(bz_cancel_state_t* s) {
    if (s) s->rc++;
}
static void bz_cancel_state_release(bz_cancel_state_t* s) {
    if (!s) return;
    if (--s->rc <= 0) free(s);
}

extern "C" void* bz_cts_alloc(void) {
    auto* src = (bz_cancel_source_t*)calloc(1, sizeof(bz_cancel_source_t));
    src->state = (bz_cancel_state_t*)calloc(1, sizeof(bz_cancel_state_t));
    src->state->cancelled = 0;
    src->state->rc = 1;  // source holds one reference
    src->disposed = false;
    return src;
}

extern "C" void* bz_cts_token(void* srcp) {
    auto* src = (bz_cancel_source_t*)srcp;
    if (!src || !src->state) return nullptr;
    bz_cancel_state_retain(src->state);  // token borrows a reference
    return src->state;                   // CancellationToken handle
}

extern "C" int32_t bz_cts_is_cancelled(void* srcp) {
    auto* src = (bz_cancel_source_t*)srcp;
    if (!src || !src->state) return 0;
    return src->state->cancelled;
}

extern "C" void bz_cts_cancel(void* srcp) {
    auto* src = (bz_cancel_source_t*)srcp;
    if (!src || !src->state) return;
    src->state->cancelled = 1;  // idempotent: already 1 → no-op
}

extern "C" void bz_cts_dispose(void* srcp) {
    auto* src = (bz_cancel_source_t*)srcp;
    if (!src) return;
    if (src->disposed) return;  // idempotent (§28.2)
    src->disposed = true;
    // Per stdlib §6.1 destructor note: signals cancellation to any tokens
    // still observing.  Outstanding token handles retain the state cell via
    // the rc, so reads after dispose stay safe.
    if (src->state) {
        src->state->cancelled = 1;
        bz_cancel_state_release(src->state);  // drop the source's reference
        src->state = nullptr;
    }
    free(src);
}

extern "C" int32_t bz_ctoken_is_cancelled(void* token) {
    auto* s = (bz_cancel_state_t*)token;
    if (!s) return 0;  // CancellationToken.None never cancels
    return s->cancelled;
}

// Shared OperationCanceledException vtable for ThrowIfCancellationRequested.
// We use the same machinery as the built-in exception types in
// LlvmBackend_Setup.cpp: each built-in exception has a static vtable global
// (builtinVtable_["OperationCanceledException"]) and the global exception slot
// stores the object pointer.  We cannot access that vtable from the pure-C
// runtime, so instead we use a sentinel approach: a process-global
// "cancellation exception" object is allocated once and reused for every
// ThrowIfCancellationRequested.  The object layout matches the 16-byte
// built-in exception format (vtable_slot, message_slot) that LlvmBackend_Setup
// allocates for every `new OperationCanceledException()`.  We leave the vtable
// slot null here — the codegen catch machinery matches by the global exception
// slot being non-null AND by type descriptor, which it retrieves from the
// object's vtable_slot.  A null vtable would break type-based catch dispatch.
//
// Therefore we take a different approach: bz_ctoken_throw_if_cancelled sets
// a thread-local "cancellation requested" flag and returns 1; the codegen emits
// a call to bz_ctoken_materialize_exception (below) to construct the real
// OperationCanceledException object before branching to the unwind path.  This
// two-step matches the pattern used by other synthetic throws (e.g. the
// index-out-of-range guard in the array indexer path).
//
// Actually, the simpler approach that mirrors existing code is:
// bz_ctoken_throw_if_cancelled simply returns a bool; the codegen wraps it in
// a conditional block that, on true, executes `throw new OperationCanceledException()`.
// This is the cleanest design — no special materialize call needed.

extern "C" int32_t bz_ctoken_throw_if_cancelled(void* token) {
    auto* s = (bz_cancel_state_t*)token;
    if (!s) return 0;   // None — never cancels
    return s->cancelled ? 1 : 0;
}

// stdlib §6.7 / spec §30.8: task helpers for CancellationToken-aware Delay.
//
// bz_task_make_oce(oce_vtable): allocate a 16-byte OperationCanceledException
// object (format: {void* vtable, void* msg}; calloc zeros msg). The vtable
// pointer is supplied by codegen so the runtime does not need to know the LLVM
// vtable global's address.
static void* bz_task_make_oce(void* oce_vtable) {
    void* obj = calloc(1, 16);
    memcpy(obj, &oce_vtable, sizeof(void*));  // slot 0 = vtable
    return obj;
}

// stdlib §6.7 (line 2868) / spec §30.8:
// Task.Delay(milliseconds: int, token: CancellationToken): Task.
//
// Token-aware Delay. If the token is already cancelled at call time, returns an
// immediately-faulted task (OperationCanceledException). Otherwise, starts the
// timer worker but also polls the token at the WAKE point: if the sleep elaps
// and the token is still not cancelled, complete normally; if the token was
// cancelled during the sleep, complete with OCE. Mid-sleep polling (shorter
// sleep intervals) is deferred — only the pre-delay and post-delay checks are
// done here. oce_vtable is the vtable pointer for OperationCanceledException,
// supplied by codegen so the runtime can construct the exception object.
void* bz_task_delay_cancelable(int64_t milliseconds, void* token, void* oce_vtable) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    auto* s = (bz_cancel_state_t*)token;
    auto* task = new bz_task_t();
    task->domain = g_main_domain;
    domain_ref(g_main_domain);  // per fix #632: task holds a ref on its domain
    // Pre-check: if already cancelled, fault immediately.
    if (s && s->cancelled) {
        void* excObj = bz_task_make_oce(oce_vtable);
        { std::lock_guard<std::mutex> lk(task->mtx);
          task->error_obj = excObj;
          task->is_error = true;
          task->done = true; }
        return task;
    }
    // Not cancelled: schedule the delay worker.
    if (!g_scheduler) bz_scheduler_init(4);
    int64_t ms = milliseconds < 0 ? 0 : milliseconds;
    g_scheduler->submit([task, ms, s, oce_vtable]() {
        if (ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        // Post-sleep check: if token was cancelled while sleeping, fault.
        if (s && s->cancelled) {
            void* excObj = bz_task_make_oce(oce_vtable);
            bz_task_complete_error_obj(task, excObj);
        } else {
            bz_task_complete(task, nullptr, 0);
        }
    });
    return task;
}

// ---------------------------------------------------------------------------
// §30.14/§30.15 (#600 phase c): Domain.Spawn and Channel<T> runtime.
//
// Design decisions (recorded per CLAUDE.md):
//   D-c1: bz_domain_spawn allocates a FRESH bz_domain_t for the spawned
//         domain (serial run-to-completion inside the child; independent of
//         the caller's domain — §30.13 isolation). The entry function and
//         argument are passed as type-erased void* and transferred (owned);
//         the result (if any) is a void*-sized heap pointer for class results
//         or a value-in-malloc for value/string results. This mirrors the
//         bz_task_complete convention already used for Task<T>.
//   D-c2: bz_channel_t is a mutex-protected deque of void*-sized items plus
//         a FIFO of pending-receiver tasks (at most one outstanding per §30.15
//         MPSC contract — a second concurrent Receive simply enqueues another
//         pending receiver task). No condvars: receivers are async.
//   D-c3: bz_channel_sender_t is a trivial ref-count wrapper around the shared
//         channel so multiple senders (via NewSender()) see the same channel.
//         Last sender release closes the channel and drains pending receivers
//         with ChannelClosedException.
//   D-c4: DomainFaultedException and ChannelClosedException are raised as
//         16-byte built-in exception objects (same layout as all builtin
//         exceptions: [vtable*, message_string*]). Their vtables are injected
//         at codegen setup time via builtinExceptionHierarchy; the runtime
//         allocates them exactly like OperationCanceledException in
//         bz_ctoken_throw_if_cancelled.
//   D-c5: For bz_domain_spawn, the entry and its args are plain void* values;
//         codegen lowers the static-function reference as a function pointer
//         and moves the argument into a heap allocation (or passes it as a
//         pointer-sized value inline). The spawned domain calls the entry via
//         the trampoline protocol: entry_fn(arg_ptr) -> result_ptr.
//         For a void entry (no return), the join OwnedTask is completed with
//         no result; for an owned-result entry the result pointer IS the result.
//   D-c6: Element encoding in channel items: class types pass void* (object
//         pointer, ownership transferred); value types (int/long/double/bool)
//         are bit-cast to uint64_t and stored inline; string items are stored
//         as void* with a +1 reference counted (bz_string_retain on send,
//         bz_string_release on Dispose drain -- same as Array<string> per
//         §28.11 / §28.9). The dispose_elem callback resolves the strategy:
//         null = value/string-ref (no deep disposal), non-null = class
//         (vtable Dispose + free) -- mirrors OwnedArray<T> element policy.
//   D-c7: The spawned domain uses the same BzScheduler thread pool as the main
//         domain. The pool must be started (bz_scheduler_init) before spawn;
//         bz_domain_spawn lazy-inits the pool if needed (same as bz_task_delay).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// bz_channel_t -- MPSC channel (§30.15)
// ---------------------------------------------------------------------------

struct bz_pending_receiver_t {
    bz_task_t*     task;   // the Task that will receive the item
    bz_domain_t*   domain; // the domain to route the completion through
};

struct bz_channel_t {
    std::mutex                        mtx;
    std::deque<void*>                 items;   // buffered items (void*-sized)
    std::deque<bz_pending_receiver_t> pending; // pending receivers (FIFO)
    void (*dispose_elem)(void*); // null for value/string; non-null for class
    int64_t  sender_count;
    bool     closed;
    bool     disposed;
};

// Forward declaration
static void channel_close_and_drain_pending(bz_channel_t* ch,
                                            void* cce_vtable);

extern "C"
void* bz_channel_new(void (*dispose_elem)(void*)) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    auto* ch = new bz_channel_t();
    ch->dispose_elem   = dispose_elem;
    ch->sender_count   = 0;
    ch->closed         = false;
    ch->disposed       = false;
    return ch;
}

// bz_channel_sender_t -- a minted sender handle (§30.15 NewSender).
// Each handle increments the channel's sender_count; releasing the last one
// closes the channel.
struct bz_channel_sender_t {
    bz_channel_t* ch;
};

extern "C"
void* bz_channel_new_sender(void* chp) {
    auto* ch = (bz_channel_t*)chp;
    {   std::lock_guard<std::mutex> lk(ch->mtx);
        ch->sender_count++;
    }
    auto* tx = new bz_channel_sender_t{ch};
    return tx;
}

// bz_channel_send: append item to channel; if a pending receiver exists,
// complete it immediately with the item (via the receiver's domain queue).
// Per §30.15: Send transfers ownership of the item.
extern "C"
void bz_channel_send(void* chp, void* item) {
    auto* ch = (bz_channel_t*)chp;
    bz_pending_receiver_t recv{nullptr, nullptr};
    {   std::lock_guard<std::mutex> lk(ch->mtx);
        if (ch->closed || ch->disposed) {
            // Should not happen in a correct program (sema enforces no send
            // after close). Safe fallback: dispose the item and return.
            if (item && ch->dispose_elem)
                ch->dispose_elem(item);
            return;
        }
        if (!ch->pending.empty()) {
            recv = ch->pending.front();
            ch->pending.pop_front();
            // Don't push item to items -- deliver directly via the task below.
        } else {
            ch->items.push_back(item);
        }
    }
    if (recv.task) {
        // Complete the pending-receiver task with the item (ptr-sized result).
        // Route through the receiver's domain so the continuation is serialized.
        // bz_task_complete stores result_size bytes; for a void* item we store 8.
        bz_task_complete(recv.task, &item, sizeof(void*));
    }
}

// bz_channel_sender_send: convenience wrapper used by codegen for ChannelSender.Send.
extern "C"
void bz_channel_sender_send(void* txp, void* item) {
    auto* tx = (bz_channel_sender_t*)txp;
    bz_channel_send(tx->ch, item);
}

// bz_channel_receive: return a task that will complete with the next item.
// If items are available, the task is already-done (fast path). If the channel
// is empty + closed, the task faults with ChannelClosedException.
// If empty + open, the task is parked in the pending-receiver FIFO.
//
// The codegen-supplied cce_vtable is the ChannelClosedException vtable global.
extern "C"
void* bz_channel_receive(void* chp, void* cce_vtable) {
    auto* ch = (bz_channel_t*)chp;
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    auto* task = new bz_task_t();
    task->domain = g_main_domain;
    domain_ref(g_main_domain);  // per fix #632: task holds a ref on its domain

    void* item = nullptr;
    bool  have_item = false;
    bool  is_closed_empty = false;

    {   std::lock_guard<std::mutex> lk(ch->mtx);
        if (!ch->items.empty()) {
            item = ch->items.front();
            ch->items.pop_front();
            have_item = true;
        } else if (ch->closed || ch->disposed) {
            is_closed_empty = true;
        } else {
            // Park in pending-receiver FIFO.
            ch->pending.push_back({task, task->domain});
        }
    }

    if (have_item) {
        // Fast path: already-done (item available).
        bz_task_complete(task, &item, sizeof(void*));
    } else if (is_closed_empty) {
        // Fault: ChannelClosedException.
        void* excObj = nullptr;
        if (cce_vtable) {
            excObj = calloc(1, 16);
            memcpy(excObj, &cce_vtable, sizeof(void*));
        }
        bz_task_complete_error_obj(task, excObj);
    }
    // else: task is parked; bz_channel_send or sender_release will complete it.
    return task;
}

// bz_channel_try_receive: non-blocking receive. If an item is available,
// stores it in *out_item and returns 1. Otherwise returns 0.
extern "C"
int32_t bz_channel_try_receive(void* chp, void** out_item) {
    auto* ch = (bz_channel_t*)chp;
    std::lock_guard<std::mutex> lk(ch->mtx);
    if (ch->items.empty()) return 0;
    *out_item = ch->items.front();
    ch->items.pop_front();
    return 1;
}

// bz_channel_count: number of buffered (not-yet-received) items.
extern "C"
int64_t bz_channel_count(void* chp) {
    auto* ch = (bz_channel_t*)chp;
    std::lock_guard<std::mutex> lk(ch->mtx);
    return (int64_t)ch->items.size();
}

// bz_channel_is_closed: true after the last sender has released.
extern "C"
int32_t bz_channel_is_closed(void* chp) {
    auto* ch = (bz_channel_t*)chp;
    std::lock_guard<std::mutex> lk(ch->mtx);
    return ch->closed ? 1 : 0;
}

// Close the channel and fault any pending receivers with ChannelClosedException.
// Called under external locking (the sender_count reaches 0).
// cce_vtable may be null (best-effort, no exception object).
static void channel_close_and_drain_pending(bz_channel_t* ch,
                                            void* cce_vtable) {
    std::deque<bz_pending_receiver_t> pending;
    {   std::lock_guard<std::mutex> lk(ch->mtx);
        ch->closed = true;
        pending.swap(ch->pending);
    }
    for (auto& r : pending) {
        void* excObj = nullptr;
        if (cce_vtable) {
            excObj = calloc(1, 16);
            memcpy(excObj, &cce_vtable, sizeof(void*));
        }
        bz_task_complete_error_obj(r.task, excObj);
    }
}

// bz_channel_sender_release: release one sender handle. If this was the last
// sender, close the channel. cce_vtable is the ChannelClosedException vtable.
extern "C"
void bz_channel_sender_release(void* txp, void* cce_vtable) {
    if (!txp) return;
    auto* tx = (bz_channel_sender_t*)txp;
    bz_channel_t* ch = tx->ch;
    delete tx;

    bool last_sender = false;
    {   std::lock_guard<std::mutex> lk(ch->mtx);
        ch->sender_count--;
        if (ch->sender_count <= 0)
            last_sender = true;
    }
    if (last_sender)
        channel_close_and_drain_pending(ch, cce_vtable);
}

// bz_channel_dispose: §30.15 §28.11 dispose -- close the channel and dispose
// any buffered (undelivered) items in REVERSE insertion order (§28.11 owned
// column; per §28.11 v0.135 ordered-disposal-of-owned row). Then free the
// channel object. cce_vtable is passed to close pending receivers.
extern "C"
void bz_channel_dispose(void* chp, void* cce_vtable) {
    if (!chp) return;
    auto* ch = (bz_channel_t*)chp;
    // Drain pending receivers with closed error.
    channel_close_and_drain_pending(ch, cce_vtable);
    // Dispose undelivered items in REVERSE insertion order (§28.11).
    {   std::lock_guard<std::mutex> lk(ch->mtx);
        ch->disposed = true;
        if (ch->dispose_elem) {
            // Reverse the deque items for disposal.
            std::vector<void*> rev(ch->items.rbegin(), ch->items.rend());
            ch->items.clear();
            for (void* item : rev)
                if (item) ch->dispose_elem(item);
        } else {
            ch->items.clear();
        }
    }
    delete ch;
}

// ---------------------------------------------------------------------------
// §30.14 (#600 phase c): Domain.Spawn
//
// bz_domain_spawn(entry_fn, arg_ptr, dfe_vtable) -- spawn a new domain that
// runs entry_fn(arg_ptr) and returns an already-allocated join task.
//
// entry_fn signature: void* (*)(void*) -- takes the arg pointer, returns
//   the result pointer (or null for void entry).  The codegen wraps the
//   actual Bezel entry method in a static trampoline that matches this ABI.
//
// arg_ptr: pointer to the (owned) argument value (pointer for class types;
//   address of a stack/heap copy for value types; null for no-arg entry).
//   Ownership is transferred to the spawned domain.
//
// dfe_vtable: DomainFaultedException vtable (injected by codegen; null if
//   the exception vtable is not available).
//
// Returns: a bz_task_t* join task (OwnedTask<R> or OwnedTask) that completes
//   when the spawned domain finishes (or faults).
// ---------------------------------------------------------------------------

extern "C"
void* bz_domain_spawn(void* (*entry_fn)(void*), void* arg_ptr,
                      void* dfe_vtable) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    if (!g_scheduler) bz_scheduler_init(4);

    // Allocate the join task -- the caller awaits this.
    auto* join_task = new bz_task_t();
    // The join task belongs to the CALLER's domain (main or current domain).
    join_task->domain = g_main_domain;
    domain_ref(g_main_domain);  // per fix #632: join_task holds a ref on its domain

    // Allocate a FRESH domain for the spawned entry (§30.13 isolation).
    // per fix #632: start with refs=0; domain_drain and SpawnCtx each add their own.
    auto* child_domain = new bz_domain_t();

    struct SpawnCtx {
        bz_domain_t*   child_domain;
        bz_task_t*     join_task;
        void* (*entry_fn)(void*);
        void*          arg_ptr;
        void*          dfe_vtable;
    };
    auto* ctx = new SpawnCtx{child_domain, join_task, entry_fn, arg_ptr, dfe_vtable};
    domain_ref(child_domain);  // per fix #632: SpawnCtx holds a ref on child_domain

    // Schedule the entry as a continuation on the child domain.
    auto entry_cont = [](void* /*task_unused*/, void* state) {
        auto* sc = (SpawnCtx*)state;

        // Per D-c5: call entry; detect fault via bz_exc.
        // Save and clear bz_exc so a fault in the caller domain doesn't
        // confuse the fault check.
        extern void* bz_exc;
        void* prev_exc = bz_exc;
        bz_exc = nullptr;
        void* result = sc->entry_fn(sc->arg_ptr);
        void* fault_exc = bz_exc;
        bz_exc = prev_exc;

        if (fault_exc) {
            // The entry faulted. Per §30.14: wrap in DomainFaultedException.
            // Copy message from the original exception (offset 8 = message string*).
            void* dfe_obj = nullptr;
            if (sc->dfe_vtable) {
                void* msg_ptr = nullptr;
                if (fault_exc) {
                    // Read message string pointer from the exception's slot 8.
                    memcpy(&msg_ptr, (char*)fault_exc + 8, sizeof(void*));
                    if (msg_ptr) bz_string_retain(msg_ptr);
                }
                dfe_obj = calloc(1, 16);
                memcpy(dfe_obj, &sc->dfe_vtable, sizeof(void*));
                if (msg_ptr) memcpy((char*)dfe_obj + 8, &msg_ptr, sizeof(void*));
                // Dispose the original fault exception. Dynamic dispatch via
                // vtable slot 2 (Dispose), then free.
                void** fault_vtbl = nullptr;
                memcpy(&fault_vtbl, fault_exc, sizeof(void*));
                if (fault_vtbl && fault_vtbl[2]) {
                    typedef void (*DispFn)(void*);
                    ((DispFn)fault_vtbl[2])(fault_exc);
                }
                free(fault_exc);
            }
            bz_task_complete_error_obj(sc->join_task, dfe_obj);
        } else {
            // Normal completion. result is the ptr-sized result (class ptr or
            // null for void entry).
            bz_task_complete(sc->join_task, &result, sizeof(void*));
        }

        // per fix #632: release the SpawnCtx's ref on the child domain.
        // domain_drain still holds its own drain-ref (acquired at domain_drain
        // entry), so the domain remains live until the drain loop exits.
        bz_domain_t* dom = sc->child_domain;
        delete sc;
        domain_release(dom);  // per fix #632: release SpawnCtx ref
    };

    domain_enqueue(child_domain, entry_cont, nullptr, ctx);
    return join_task;
}
// ============================================================
// BinarySerializer / BinaryDeserializer runtime  (§2.15 / §9.8)
// ============================================================
// All extern "C" function signatures match bezel_runtime.h exactly.
// Internal types use the bz_array_t / bz_string_struct layouts defined
// earlier in this file.

// --- writer state -----------------------------------------------------------

struct bz_binser_t {
    uint8_t* buf;
    int64_t  used;   // bytes written so far
    int64_t  cap;    // allocated capacity
};

static void bz_binser_ensure(bz_binser_t* s, int64_t need) {
    if (s->used + need <= s->cap) return;
    int64_t newcap = s->cap == 0 ? 256 : s->cap;
    while (newcap < s->used + need) newcap *= 2;
    s->buf = static_cast<uint8_t*>(realloc(s->buf, (size_t)newcap));
    s->cap = newcap;
}

static void bz_binser_write_byte_impl(bz_binser_t* s, uint8_t b) {
    bz_binser_ensure(s, 1);
    s->buf[s->used++] = b;
}

static void bz_binser_write_le32_impl(bz_binser_t* s, uint32_t v) {
    bz_binser_ensure(s, 4);
    s->buf[s->used+0] = (uint8_t)(v);
    s->buf[s->used+1] = (uint8_t)(v >> 8);
    s->buf[s->used+2] = (uint8_t)(v >> 16);
    s->buf[s->used+3] = (uint8_t)(v >> 24);
    s->used += 4;
}

static void bz_binser_write_le64_impl(bz_binser_t* s, uint64_t v) {
    bz_binser_ensure(s, 8);
    for (int i = 0; i < 8; ++i)
        s->buf[s->used+i] = (uint8_t)(v >> (8*i));
    s->used += 8;
}

// Write LE32 length + raw UTF-8 bytes (no NUL), per §2.15 wire format.
static void bz_binser_write_utf8_bytes(bz_binser_t* s, const char* data, int64_t byte_len) {
    bz_binser_write_le32_impl(s, (uint32_t)byte_len);
    if (byte_len > 0) {
        bz_binser_ensure(s, byte_len);
        memcpy(s->buf + s->used, data, (size_t)byte_len);
        s->used += byte_len;
    }
}

extern "C" {

void* bz_binser_alloc(void) {
    auto* s = static_cast<bz_binser_t*>(malloc(sizeof(bz_binser_t)));
    s->buf  = nullptr;
    s->used = 0;
    s->cap  = 0;
    return s;
}

void bz_binser_free(void* serp) {
    if (!serp) return;
    auto* s = static_cast<bz_binser_t*>(serp);
    free(s->buf);
    free(s);
}

// Transfer accumulated bytes as a fresh OwnedBuffer<byte> (bz_array_t* with
// elem_size=1).  The serializer's internal buffer is cleared after the copy
// so further writes start fresh.  Caller owns the returned array.
void* bz_binser_to_bytes(void* serp) {
    auto* s = static_cast<bz_binser_t*>(serp);
    int64_t len = s->used;
    // Allocate a bz_array_t and bulk-copy the bytes directly.
    void* arr = bz_array_alloc(1, len > 0 ? len : 1);
    if (len > 0) {
        auto* a = static_cast<bz_array_t*>(arr);
        memcpy(a->data, s->buf, (size_t)len);
        a->count = len;
    }
    // Clear internal buffer so the serializer can be reused.
    s->used = 0;
    return arr;
}

// ISerializer method implementations ----------------------------------------

void bz_binser_write_int(void* serp, int64_t v) {
    bz_binser_write_le64_impl(static_cast<bz_binser_t*>(serp), (uint64_t)v);
}

void bz_binser_write_uint(void* serp, uint64_t v) {
    bz_binser_write_le64_impl(static_cast<bz_binser_t*>(serp), v);
}

void bz_binser_write_float(void* serp, double v) {
    uint64_t bits;
    memcpy(&bits, &v, 8);
    bz_binser_write_le64_impl(static_cast<bz_binser_t*>(serp), bits);
}

// WriteDecimal: Bezel decimal wire rep is implementation-defined in §2.15;
// we use 8-byte IEEE 754 LE (same as WriteFloat) for now.
void bz_binser_write_decimal(void* serp, void* /*val*/) {
    // val is a Bezel decimal value; we receive it as a double via LLVM ABI.
    // Treat as 0.0 stub (decimal synthesis deferred).
    double zero = 0.0;
    bz_binser_write_float(serp, zero);
}

void bz_binser_write_bool(void* serp, int8_t v) {
    bz_binser_write_byte_impl(static_cast<bz_binser_t*>(serp), v ? 1 : 0);
}

void bz_binser_write_string(void* serp, void* strp) {
    auto* s = static_cast<bz_binser_t*>(serp);
    if (!strp) {
        bz_binser_write_le32_impl(s, 0);
        return;
    }
    // bz_string_struct layout: {void* data, int64_t len, int8_t width}
    // For width==1, data points to uint8_t byte array (UTF-8 / Latin-1).
    // For width==2, data points to uint16_t (UTF-16 LE).
    // We re-encode to UTF-8 bytes for the wire format.
    // Simplification: for width==1 we can write data directly (it's already
    // byte-width; Bezel strings with width==1 are latin-1 or ASCII subset;
    // non-ASCII wide chars use width==2 or width==4 so the byte content is
    // valid as-is for our LE32-length-prefixed raw-bytes encoding).
    const auto* str = static_cast<const bz_string_struct*>(strp);
    if (str->width == 1) {
        bz_binser_write_utf8_bytes(s,
            static_cast<const char*>(str->data), str->len);
    } else {
        // width==2 or width==4: use bz_string_to_utf8z to get a UTF-8 copy.
        char* utf8 = bz_string_to_utf8z(strp);
        int64_t n = utf8 ? (int64_t)strlen(utf8) : 0;
        bz_binser_write_utf8_bytes(s, utf8 ? utf8 : "", n);
        free(utf8);
    }
}

void bz_binser_write_char(void* serp, int32_t codepoint) {
    // Encode Unicode scalar as UTF-8, then write as LE32-length-prefixed bytes.
    char buf[4];
    int n = 0;
    uint32_t cp = (uint32_t)codepoint;
    if      (cp < 0x80)     { buf[n++] = (char)cp; }
    else if (cp < 0x800)    { buf[n++] = (char)(0xC0|(cp>>6));  buf[n++] = (char)(0x80|(cp&0x3F)); }
    else if (cp < 0x10000)  { buf[n++] = (char)(0xE0|(cp>>12)); buf[n++] = (char)(0x80|((cp>>6)&0x3F));  buf[n++] = (char)(0x80|(cp&0x3F)); }
    else                    { buf[n++] = (char)(0xF0|(cp>>18)); buf[n++] = (char)(0x80|((cp>>12)&0x3F)); buf[n++] = (char)(0x80|((cp>>6)&0x3F)); buf[n++] = (char)(0x80|(cp&0x3F)); }
    bz_binser_write_utf8_bytes(static_cast<bz_binser_t*>(serp), buf, n);
}

void bz_binser_write_null(void* serp) {
    bz_binser_write_byte_impl(static_cast<bz_binser_t*>(serp), 0);
}

void bz_binser_begin_object(void* serp, int32_t field_count) {
    bz_binser_write_le32_impl(static_cast<bz_binser_t*>(serp), (uint32_t)field_count);
}

// write_field_name: name is a bz_string* (void*)
void bz_binser_write_field_name(void* serp, void* name) {
    bz_binser_write_string(serp, name);
}

void bz_binser_end_object(void* /*serp*/) { /* no trailer in binary format */ }

void bz_binser_begin_array(void* serp, int32_t element_count) {
    bz_binser_write_le32_impl(static_cast<bz_binser_t*>(serp), (uint32_t)element_count);
}

void bz_binser_end_array(void* /*serp*/) { /* no trailer */ }

} // extern "C" (writer half)

// --- reader state -----------------------------------------------------------

struct bz_binder_t {
    const uint8_t* data;
    int64_t        len;
    int64_t        pos;
    int8_t         ignore_unknown;
    int32_t        fields_remaining;
    bool           in_object;
};

static int bz_binder_read_byte_impl(bz_binder_t* d, uint8_t* out) {
    if (d->pos >= d->len) return 0;
    *out = d->data[d->pos++];
    return 1;
}

static int bz_binder_read_le32_impl(bz_binder_t* d, uint32_t* out) {
    if (d->pos + 4 > d->len) return 0;
    *out = ((uint32_t)d->data[d->pos+0])
         | ((uint32_t)d->data[d->pos+1] <<  8)
         | ((uint32_t)d->data[d->pos+2] << 16)
         | ((uint32_t)d->data[d->pos+3] << 24);
    d->pos += 4;
    return 1;
}

static int bz_binder_read_le64_impl(bz_binder_t* d, uint64_t* out) {
    if (d->pos + 8 > d->len) return 0;
    *out = 0;
    for (int i = 0; i < 8; ++i)
        *out |= ((uint64_t)d->data[d->pos+i]) << (8*i);
    d->pos += 8;
    return 1;
}

// Read a LE32-prefixed UTF-8 byte string and return a heap-allocated
// bz_string_struct* (width=1, caller must free via bz_string_release or
// direct free of both the struct and its data buffer).
// Returns nullptr on truncated data.
static void* bz_binder_read_bz_string(bz_binder_t* d) {
    uint32_t byte_len = 0;
    if (!bz_binder_read_le32_impl(d, &byte_len)) return nullptr;
    if (d->pos + (int64_t)byte_len > d->len)     return nullptr;
    // Allocate data buffer (NUL-terminate for bz_string_to_utf8z compat).
    char* buf = static_cast<char*>(malloc((size_t)byte_len + 1));
    memcpy(buf, d->data + d->pos, byte_len);
    buf[byte_len] = '\0';
    d->pos += byte_len;
    // Allocate a full BZ_STRING_SIZEOF (32-byte) object with rc initialized so
    // that bz_string_release works correctly (#550: deserialized field names are
    // passed to bz_string_release by generated code; using a smaller struct
    // would leave rc at offset 24 uninitialized, corrupting the heap).
    uint8_t* strObj = static_cast<uint8_t*>(calloc(1, (size_t)BZ_STRING_SIZEOF));
    int64_t str_len64 = (int64_t)byte_len;
    memcpy(strObj,      &buf,        sizeof(void*));    // data
    memcpy(strObj + 8,  &str_len64,  sizeof(int64_t)); // len
    strObj[16] = 1;                                     // width = 1
    bz_string_init_rc(strObj);  // #422: rc := 1 (producer's reference)
    return strObj;
}

extern "C" {

void* bz_binder_alloc(const uint8_t* data, int64_t len, int8_t ignore_unknown) {
    auto* d = static_cast<bz_binder_t*>(malloc(sizeof(bz_binder_t)));
    d->data             = data;
    d->len              = len;
    d->pos              = 0;
    d->ignore_unknown   = ignore_unknown;
    d->fields_remaining = 0;
    d->in_object        = false;
    return d;
}

void bz_binder_free(void* derp) {
    free(derp);
}

int64_t bz_binder_read_int(void* derp) {
    uint64_t v = 0;
    bz_binder_read_le64_impl(static_cast<bz_binder_t*>(derp), &v);
    return (int64_t)v;
}

uint64_t bz_binder_read_uint(void* derp) {
    uint64_t v = 0;
    bz_binder_read_le64_impl(static_cast<bz_binder_t*>(derp), &v);
    return v;
}

double bz_binder_read_float(void* derp) {
    uint64_t bits = 0;
    bz_binder_read_le64_impl(static_cast<bz_binder_t*>(derp), &bits);
    double v;
    memcpy(&v, &bits, 8);
    return v;
}

void* bz_binder_read_decimal(void* derp) {
    // Stub: read 8 bytes (to advance position) and return nullptr.
    uint64_t bits = 0;
    bz_binder_read_le64_impl(static_cast<bz_binder_t*>(derp), &bits);
    return nullptr;
}

int8_t bz_binder_read_bool(void* derp) {
    uint8_t b = 0;
    bz_binder_read_byte_impl(static_cast<bz_binder_t*>(derp), &b);
    return b ? 1 : 0;
}

// Returns a newly-allocated bz_string_struct* (caller owns).
void* bz_binder_read_string(void* derp) {
    return bz_binder_read_bz_string(static_cast<bz_binder_t*>(derp));
}

int32_t bz_binder_read_char(void* derp) {
    auto* str = static_cast<bz_string_struct*>(
        bz_binder_read_bz_string(static_cast<bz_binder_t*>(derp)));
    if (!str || str->len == 0) {
        if (str) { free(str->data); free(str); }
        return 0;
    }
    const uint8_t* u = static_cast<const uint8_t*>(str->data);
    uint32_t cp;
    if      (u[0] < 0x80) { cp = u[0]; }
    else if (u[0] < 0xE0) { cp = ((u[0]&0x1F)<<6)  | (u[1]&0x3F); }
    else if (u[0] < 0xF0) { cp = ((u[0]&0x0F)<<12) | ((u[1]&0x3F)<<6) | (u[2]&0x3F); }
    else                  { cp = ((u[0]&0x07)<<18)  | ((u[1]&0x3F)<<12) | ((u[2]&0x3F)<<6) | (u[3]&0x3F); }
    free(str->data);
    free(str);
    return (int32_t)cp;
}

int8_t bz_binder_try_read_null(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);
    if (d->pos >= d->len) return 0;
    if (d->data[d->pos] == 0) { d->pos++; return 1; }
    return 0;
}

int32_t bz_binder_begin_object(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);
    uint32_t cnt = 0;
    if (!bz_binder_read_le32_impl(d, &cnt)) return -1;
    // Note: fields_remaining is NOT set here.  The synthesized loop in
    // emitRecordDeserialize uses the returned count directly; nested records
    // call bz_binder_begin_object recursively which would clobber a flat
    // fields_remaining field.  Reading is driven entirely by the loop counter
    // held in LLVM IR, not by the binder state (§9.8 / issue #550).
    d->in_object = true;
    return (int32_t)cnt;
}

// Returns a newly-allocated bz_string* for the field name.
// No fields_remaining guard: the synthesized deserialization loop in
// emitRecordDeserialize calls this exactly fieldCount times, where fieldCount
// is the value returned by bz_binder_begin_object for THIS object level.
// Nesting works correctly because each emitRecordDeserialize invocation uses
// its own fieldCount local (stored in an LLVM alloca), not shared state here.
void* bz_binder_read_field_name(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);
    return bz_binder_read_bz_string(d);
}

// skip_value: advance past one value at current position.
// The binary format has no type tags, so this is a best-effort skip:
// we read a LE32 "length" and skip that many bytes (works for all
// string-prefixed and object/array values; for int/float the caller
// would need to know the type — synthesis never generates unknown fields).
void bz_binder_skip_value(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);
    if (d->pos >= d->len) return;
    uint32_t maybe_len = 0;
    if (!bz_binder_read_le32_impl(d, &maybe_len)) return;
    if (d->pos + (int64_t)maybe_len <= d->len)
        d->pos += (int64_t)maybe_len;
}

void bz_binder_end_object(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);
    d->in_object = false;
    d->fields_remaining = 0;
}

int32_t bz_binder_begin_array(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);
    uint32_t cnt = 0;
    if (!bz_binder_read_le32_impl(d, &cnt)) return -1;
    return (int32_t)cnt;
}

int8_t bz_binder_has_more_elements(void* /*derp*/) {
    // Count-prefixed format: caller drives the loop; this is a placeholder.
    return 0;
}

void bz_binder_end_array(void* /*derp*/) { /* no trailer */ }

// bz_ser_throw: build a minimal SerializationException-like exception object
// and store it in the global bz_exc slot.  `ser_exn_vtable` is the vtable*
// emitted by the codegen for the SerializationException class (nullptr in tests
// that don't use vtable-dispatch).  `msg` is a NUL-terminated C string.
void bz_ser_throw(const char* msg, void* ser_exn_vtable) {
    // Build a bz_string_struct for the message.
    size_t msg_len = msg ? strlen(msg) : 0;
    char* buf = static_cast<char*>(malloc(msg_len + 1));
    if (msg_len > 0) memcpy(buf, msg, msg_len);
    buf[msg_len] = '\0';
    auto* str = static_cast<bz_string_struct*>(malloc(sizeof(bz_string_struct)));
    str->data  = buf;
    str->len   = (int64_t)msg_len;
    str->width = 1;

    // An exception object layout is {vtable*, message bz_string*}.
    // Allocate a 2-pointer object: [vtable*, str*].
    void** exc_obj = static_cast<void**>(malloc(2 * sizeof(void*)));
    exc_obj[0] = ser_exn_vtable;
    exc_obj[1] = str;
    extern void* bz_exc;
    bz_exc = exc_obj;
}

// OwnedBuffer<byte> helpers --------------------------------------------------

// Allocate an OwnedBuffer<byte> containing a copy of `data[0..len)`.
// Returns bz_array_t* (elem_size 1).  Caller owns the result.
void* bz_owned_buffer_byte_alloc(const uint8_t* data, int64_t len) {
    void* arr = bz_array_alloc(1, len > 0 ? len : 1);
    if (data && len > 0) {
        auto* a = static_cast<bz_array_t*>(arr);
        memcpy(a->data, data, (size_t)len);
        a->count = len;
    }
    return arr;
}

// Return a pointer+length pair from an OwnedBuffer<byte> (bz_array_t* with
// elem_size 1).  The span borrows from the buffer; the buffer must outlive it.
void bz_owned_buffer_get_span(void* bufp,
                               const uint8_t** data_out,
                               int64_t* len_out) {
    if (!bufp) { *data_out = nullptr; *len_out = 0; return; }
    auto* arr = static_cast<bz_array_t*>(bufp);
    *data_out = static_cast<const uint8_t*>(arr->data);
    *len_out  = arr->count;
}

} // extern "C" (binary serialization)
