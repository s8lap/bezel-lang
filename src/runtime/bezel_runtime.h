// bezel_runtime.h — C runtime support for the Bezel standard library.
// All functions use the 'bz_' prefix and are declared 'extern "C"' so
// Bezel's LLVM IR can call them without name-mangling issues.
//
// The codegen in LlvmBackend.cpp declares these via declareC() and emits
// direct calls in the generated IR. The runtime is compiled as a static
// library (libbezel_runtime.a) and linked into both standalone binaries
// (via Driver.cpp) and the JIT test harness (via DynamicLibrarySearchGenerator).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// bz_array_t — type-erased dynamic array used by Array<T> and OwningArray<T>.
// Elements are stored as raw bytes; callers supply the element size.
// ---------------------------------------------------------------------------

// stdlib §4.4/§4.5 (Array<T>/OwningArray<T>): allocate a new array capable
// of holding `initial_cap` elements of `elem_size` bytes each. Returns a
// pointer to the internal bz_array_t struct (opaque to Bezel code, which
// treats it as a plain `ptr`).
void* bz_array_alloc(int64_t elem_size, int64_t initial_cap);

// stdlib §4.4/§4.5: return the current element count (0 for a null arr).
int64_t bz_array_len(void* arr);

// stdlib §4.4/§4.5 (#741): the array-backed collection's allocated capacity.
int64_t bz_array_capacity(void* arr);

// stdlib §9.6 (#741): 1 iff every code point of the string is < 128.
int32_t bz_string_is_ascii(const void* s);

// §4.14: allocate with n pre-filled (zero-initialized) slots; count == capacity == n.
// Used by `new OwningBuffer<T>(n)`.
void* bz_array_alloc_count_n(int64_t elem_size, int64_t n);

// stdlib §4.4/§4.5 (Array<T>.Add / OwningArray<T>.Add): append one element
// (elem_size bytes) to the end. Grows capacity as needed.
void bz_array_add(void* arr, const void* elem, int64_t elem_size);

// stdlib §4.4/§4.5 (Array<T>/OwningArray<T> indexer getter): copy the
// element at `idx` into `out` (elem_size bytes). No bounds check.
void bz_array_get(void* arr, int64_t idx, void* out, int64_t elem_size);

// stdlib §4.4/§4.5 (Array<T> indexer setter): overwrite the element at `idx`
// with the elem_size bytes at `elem`. No bounds check.
void bz_array_set(void* arr, int64_t idx, const void* elem, int64_t elem_size);

// issue #388 / stdlib §4.5: the OwningArray<T> indexer setter — dispose the
// (non-null) class-typed element currently at `idx` via `dispose_elem`, then
// overwrite the slot with the elem_size bytes at `elem`. The element is treated
// as a pointer-sized object reference (owning collections constrain
// `where T: class`). `dispose_elem` is null-safe per element. The slot is read
// BEFORE the store so the displaced owner is disposed exactly once, then the new
// owner replaces it. Used by OwningArray<T>'s `set(owned)` indexer (§4.5).
void bz_array_set_disposing(void* arr, int64_t idx, const void* elem,
                            int64_t elem_size, void (*dispose_elem)(void*));

// stdlib §4.4/§4.5 (Array<T>.RemoveAt): remove the element at `idx`,
// shifting subsequent elements down. No bounds check.
void bz_array_remove_at(void* arr, int64_t idx, int64_t elem_size);

// fix #642 / stdlib §4.5: PopAt(i) — copy element at idx into *out (transferring
// ownership to the caller) then remove the slot WITHOUT disposal.
void bz_array_pop_at(void* arr, int64_t idx, void* out, int64_t elem_size);

// stdlib §4.5: insert one element (elem_size bytes) at `idx`, shifting the
// existing element at `idx` and all subsequent elements up by one. Grows
// capacity as needed. `idx` is clamped to [0, count] (idx == count appends).
// Used by OwningArray<T>.Insert(index, item) and Array<T>.Insert.
void bz_array_insert_at(void* arr, int64_t idx, const void* elem,
                        int64_t elem_size);

// stdlib §4.4/§4.5 (Array<T>.Clear / OwningArray<T>.Clear): set length to 0.
// Does NOT call dispose_elem; caller is responsible (the borrowing Array<T>
// has nothing to dispose; the owning OwningArray<T> path is the
// clear_disposing overload below).
void bz_array_clear(void* arr);

// stdlib §4.5: dispose every (non-null) class-typed element, then set length to
// 0. Used by the OwningArray<T>.Clear()/Dispose() element cascade where the
// element type is a class. (The borrowing-family `Array<T>.DisposeElements()`
// that formerly shared this helper was retired with axiom A8, §28.11 v0.213,
// #2081.) `dispose_elem` is null-safe per element. Treats each
// element as a pointer-sized object reference (owning collections constrain
// `where T: class`, so elements are always object pointers). The slots are
// nulled as they are disposed so a subsequent Dispose finds nothing to redo.
void bz_array_clear_disposing(void* arr, void (*dispose_elem)(void*));

// stdlib §4.5: dispose the (non-null) class-typed element at `idx`, then remove
// it (shifting subsequent elements down). Used by OwningArray<T>.RemoveAt(i).
// No-op if idx is out of range. Disposing happens before the shift so the slot
// leaves the live range afterward (no double-dispose on a later Dispose).
void bz_array_remove_at_disposing(void* arr, int64_t idx, int64_t elem_size,
                                  void (*dispose_elem)(void*));

// Free the array and, if `dispose_elem` is non-null, call it for each element
// passing a pointer to the element's storage.
void bz_array_free(void* arr, void (*dispose_elem)(void*));

// #2608 / §28.11 / stdlib §4.3: record an OWNING array-backed collection's
// element disposer in the array header at construction. Codegen emits this only
// for a concrete owning collection (`new OwningArray<T>()` and its Stack/Queue
// siblings) whose element policy yields a disposer thunk; a borrowing collection
// is never tagged and keeps the calloc-zeroed NULL.
void bz_array_set_elem_disposer(void* arr, void (*dispose_elem)(void*));

// #2608: free an array whose element ownership is NOT statically known at the
// free site — an `owned` field typed by a read-only interface shared by the
// borrowing and owning hierarchies (stdlib §4.3), whose dynamic backing may be
// `Array<T>` (borrowing) or `OwningArray<T>` (owning). Uses the header disposer
// recorded at construction; an untagged array degenerates exactly to
// `bz_array_free(arr, nullptr)`, so this can never over-dispose.
void bz_array_free_dynamic(void* arr);

// §28.2 v0.135 / issue #621: checked-disposal-mode quarantine. Marks the
// array as disposed (for the indexer guard) and disposes elements, but does
// NOT free the header block — mirrors class-instance quarantine in the codegen.
void bz_array_mark_disposed(void* arr, void (*dispose_elem)(void*));

// §28.2 v0.135 / issue #621: return non-zero if the array has been disposed
// via bz_array_mark_disposed (checked mode only).
int8_t bz_array_is_disposed(void* arr);

// Sort in place. `cmp(a, b)` receives pointers to two elements and must return
// negative/zero/positive analogous to strcmp.
void bz_array_sort(void* arr, int64_t elem_size,
                   int (*cmp)(const void*, const void*));

// Sort in place with a closure-aware comparator.  `cmpfn(closure, a, b)`
// receives the opaque closure pointer and two element pointers; returns
// negative/zero/positive.  Used for Array<T>.Sort(comparer: (T,T)->int)
// and the derived Sorted(comparer) copy-form (stdlib §3 / #472).
void bz_array_sort_closure(void* arr, int64_t elem_size,
                           void* closure,
                           int (*cmpfn)(void*, const void*, const void*));

// §29.12 / #4631: record an in-place repositioning the CALLER performed, so a
// live enumerator is invalidated. The in-place `Reverse()` (stdlib §4.4) is
// lowered as an inlined swap loop in codegen rather than as a call into the
// runtime, so it has no other site at which to bump the modification epoch.
// No-op on an array of 0 or 1 elements (nothing repositions), matching
// bz_array_sort.
void bz_array_note_repositioned(void* arr);

// Return a shallow copy of `arr` (same element bytes, no deep clone).
// The caller owns the returned array and must free it with
// bz_array_free(result, /*disposer=*/null).  Used by Array<T>.Reversed()
// and Array<T>.Sorted[...] (stdlib §3 / #472).
void* bz_array_copy(void* arr, int64_t elem_size);

// fix #682 / §15.6: return a fresh Array<T> containing copies of elements in
// the half-open range [start, end).  `end` is exclusive.  Precondition: the
// caller has already bounds-checked start/end against Count (raises
// IndexOutOfRangeException via codegen emitGuardThrow before calling here).
// The caller owns the returned array and must free it with
// bz_array_free(result, /*disposer=*/null).
void* bz_array_slice(void* arr, int64_t start, int64_t end, int64_t elem_size);

// #429 / §4.6 / §28.9: bz_string_retain every non-null pointer-sized element of
// `arr` (each slot holds a bz_string*).  Codegen calls this on a freshly copied
// or materialized Array<string> (bz_array_copy / bz_lazy_to_array result) so the
// new array independently co-owns a +1 per element — matching the per-element
// bz_string_release the array's free/dispose later issues.  Only valid for an
// array whose elements are pointer-sized strings.  retain is a no-op on a
// sentinel/literal or null, so partial/empty arrays are safe.
void bz_array_retain_strings(void* arr);

// Return a pointer directly into the array's internal buffer at element `idx`.
// Only valid until the next mutation; used for zero-copy access in codegen.
void* bz_array_ptr_at(void* arr, int64_t idx, int64_t elem_size);

// §29.12 / issue #1232: return the current modification epoch of the array.
// Codegen captures this at for-in loop start; checks it each iteration.
// Mismatch → codegen throws InvalidOperationException (per §29.12).
int64_t bz_array_get_mod(void* arr);

// Ensure capacity for at least `cap` elements without changing the length.
void bz_array_reserve(void* arr, int64_t elem_size, int64_t cap);

// §33.4: build an Array<string> (bz_array_t* of bz_string*) from the process
// argv, excluding argv[0] (the executable name).  Called by the C-entry shim
// for Main(args: Array<string>) forms.  Dispose with
//   bz_array_free(result, bz_string_release).
void* bz_args_array(int32_t argc, char** argv);

// ---------------------------------------------------------------------------
// bz_llist_t — doubly-linked list used by LinkedList<T> and OwningLinkedList<T>.
// stdlib §4.6 / §4.7.  Node layout: { prev*, next*, value[elem_size] }.
// List layout: { head*, tail*, count }.  All pointers are opaque to Bezel code.
// Elements are stored inline in each node (same elem_size model as bz_array_t).
// For OwningLinkedList<T> pass a non-null dispose_elem; for LinkedList<T> pass null.
// ---------------------------------------------------------------------------

// Allocate an empty list. elem_size = bytes per element value stored inline.
void* bz_llist_alloc(int64_t elem_size);

// Return the number of elements in the list.
int64_t bz_llist_count(void* list);

// §29.12 / issue #1232: return the current modification epoch of the linked list.
// Codegen captures this at for-in loop start; checks it each iteration.
// Mismatch → codegen throws InvalidOperationException (per §29.12).
int64_t bz_llist_get_mod(void* list);

// Prepend a new node (copies elem_size bytes from `elem`). Returns the new node*.
void* bz_llist_add_first(void* list, const void* elem, int64_t elem_size);

// Append a new node. Returns the new node*.
void* bz_llist_add_last(void* list, const void* elem, int64_t elem_size);

// Insert a new node before `node`. Returns the new node*.
void* bz_llist_add_before(void* list, void* node, const void* elem, int64_t elem_size);

// Insert a new node after `node`. Returns the new node*.
void* bz_llist_add_after(void* list, void* node, const void* elem, int64_t elem_size);

// Remove and free the first node. If dispose_elem is non-null, calls it on the element first.
void bz_llist_remove_first(void* list, void (*dispose_elem)(void*));

// Remove and free the last node. If dispose_elem is non-null, calls it on the element first.
void bz_llist_remove_last(void* list, void (*dispose_elem)(void*));

// fix #642 / stdlib §4.7: copy first/last element into *out, then unlink WITHOUT
// disposal (ownership transfers to the caller).
void bz_llist_pop_first(void* list, void* out, int64_t elem_size);
void bz_llist_pop_last(void* list, void* out, int64_t elem_size);

// Remove and free a specific node (unlinks from list). Calls dispose_elem if non-null.
void bz_llist_remove_node(void* list, void* node, void (*dispose_elem)(void*));

// Find the first node whose element bytes equal `elem` (using memcmp). Returns null if not found.
void* bz_llist_find(void* list, const void* elem, int64_t elem_size);

// Return the head node (or null if empty).
void* bz_llist_first_node(void* list);

// Return the tail node (or null if empty).
void* bz_llist_last_node(void* list);

// Return a pointer to the element bytes stored inline in `node`.
void* bz_llist_node_value_ptr(void* node);

// Return the next node (or null if at tail).
void* bz_llist_node_next(void* node);

// Return the previous node (or null if at head).
void* bz_llist_node_prev(void* node);

// Free all nodes (calling dispose_elem on each element if non-null) and the list struct.
void bz_llist_free(void* list, void (*dispose_elem)(void*));

// ---------------------------------------------------------------------------
// bz_dict_t — open-addressing hash table used by Dictionary<K,V>.
// Two variants: string keys (most common) and generic keys (via fn pointers).
// Values are stored as raw `void*` (the Bezel object pointer or boxed scalar).
// ---------------------------------------------------------------------------

// String-key dictionary (keys are UTF-8 byte slices; copied internally).
void*   bz_dict_alloc(void);
void    bz_dict_free(void* d, void (*dispose_val)(void*));

// §28.2 v0.135 / issue #627: checked-disposal-mode quarantine. Disposes values,
// frees buckets/order arrays, sets disposed=1 on the header, does NOT free the
// header (so the disposed flag remains readable for method guards).
void    bz_dict_mark_disposed(void* d, void (*dispose_val)(void*));

// §28.2 v0.135 / issue #627: return non-zero if the dict has been disposed
// via bz_dict_mark_disposed (checked mode only).
int8_t  bz_dict_is_disposed(void* d);

// fix #4499 (spec §4.6 / §8.3, v0.306): every string-key entry point takes the
// key as (raw data, CODE-POINT count, storage width). The width is load-bearing
// — a string's byte size is `len * width`, and the pre-fix ABI carried only
// (data, len) and used `len` as a byte count, truncating every width>1 key to
// its first `len/width` code points so distinct keys collided into one entry.
bool    bz_dict_try_get_str(void* d, const char* k, int64_t k_len, int8_t k_w,
                            void** out);
void    bz_dict_set_str(void* d, const char* k, int64_t k_len, int8_t k_w,
                        void* val);

// issue #388 / stdlib §4.9: the OwningDictionary<string,V> indexer setter —
// if the key is already present, dispose the value currently stored under it via
// `dispose_val` (null-safe) before storing the new (owned) value; on a fresh key
// it just inserts. Used by OwningDictionary<K,V>'s `set(owned)` indexer.
void    bz_dict_set_str_disposing(void* d, const char* k, int64_t k_len,
                                  int8_t k_w, void* val,
                                  void (*dispose_val)(void*));
bool    bz_dict_remove_str(void* d, const char* k, int64_t k_len, int8_t k_w);
// issue M1 / stdlib §4.9: OwningDictionary<string,V>.Remove — disposes the
// removed value via dispose_val before unlinking (null-safe: skip if null).
bool    bz_dict_remove_str_disposing(void* d, const char* k, int64_t k_len,
                                     int8_t k_w, void (*dispose_val)(void*));
bool    bz_dict_contains_str(void* d, const char* k, int64_t k_len, int8_t k_w);
int64_t bz_dict_count(void* d);

// §29.12 / issue #1232: return the current modification epoch of the dict/set.
// Codegen captures this at for-in loop start; checks it each iteration.
// Mismatch → codegen throws InvalidOperationException (per §29.12).
int64_t bz_dict_get_mod(void* d);

// fix #3026 (spec §28.2 / §28.9 / §8.1, stdlib §4.1 / §4.6-§4.9, v0.240):
// dispose a heap-collection pointer whose concrete backing is unknown at the
// call site (a polymorphic collection-interface receiver —
// IReadOnlyCollection<T> / ICollection<T> / IOwningCollection<T>). Reads the
// kind tag (see BZ_RUNTIME_KIND_* in bezel_runtime_internal.hpp) the alloc
// fn stamped and dispatches to bz_array_free / bz_llist_free / bz_dict_free
// accordingly. `dispose_elem` is the static-element-type disposer the caller
// computed (null for borrowing, the class-element disposer for
// IOwningCollection<T>); see the helper's definition for the sentinel layout
// and probe order.
void     bz_runtime_dispose(void* p, void (*dispose_elem)(void*));
// Diagnostic accessor for the kind tag (returns BZ_RUNTIME_KIND_NONE on null
// or on an untagged pointer). Used by codegen / tests; never by emitted
// code.
int64_t  bz_runtime_kind(void* p);

// Enumerate all keys as (char* data, int64_t code-point len, int8_t width)
// triples. The caller must free the returned arrays with free(). `*keys_out`,
// `*lens_out` and `*widths_out` are malloc'd parallel arrays; each `keys[i]` is
// its own malloc'd `lens[i] * widths[i]`-byte copy (fix #4499 — the width is
// what lets a caller rebuild the key at its true storage width).
void bz_dict_keys_str(void* d, char*** keys_out, int64_t** lens_out,
                      int8_t** widths_out, int64_t* n_out);

// Generic-key variant: key_size bytes per key; hash and eq work on raw bytes.
void* bz_dict_alloc_generic(int64_t key_size,
                             uint64_t (*hash)(const void* key),
                             bool     (*eq)(const void* a, const void* b));
bool  bz_dict_try_get_gen(void* d, const void* key, void** out);
void  bz_dict_set_gen(void* d, const void* key, void* val);

// issue #388 / stdlib §4.9: generic-key counterpart to bz_dict_set_str_disposing
// — dispose the value currently stored under `key` (null-safe) before storing
// the new (owned) value; on a fresh key it just inserts.
void  bz_dict_set_gen_disposing(void* d, const void* key, void* val,
                                void (*dispose_val)(void*));
bool  bz_dict_remove_gen(void* d, const void* key);
// issue M1 / stdlib §4.9: OwningDictionary<K,V>.Remove for generic keys —
// disposes the removed value via dispose_val before unlinking (null-safe).
bool  bz_dict_remove_gen_disposing(void* d, const void* key,
                                   void (*dispose_val)(void*));
bool  bz_dict_contains_gen(void* d, const void* key);

// fix #643 / stdlib §4.9: PopValue(key) — retrieve the stored value pointer
// (written into *out) then remove the entry WITHOUT disposing the value
// (ownership transfers to the caller).  Returns false when key is absent.
bool  bz_dict_pop_value_str(void* d, const char* k, int64_t k_len, int8_t k_w,
                            void** out);
bool  bz_dict_pop_value_gen(void* d, const void* key, void** out);

// stdlib §4.10 / §4.8: empty a hash table in place (keeps it allocated). Frees
// each key copy and, if `dispose_val` is non-null, disposes each value; zeroes
// all buckets and resets count. Used by Set<T>.Clear() and the generic-table
// Dictionary.Clear(). Dict-backed Set values are sentinels → pass null.
void  bz_dict_clear(void* d, void (*dispose_val)(void*));

// Enumerate all generic-key entries (issue #370). `*keys_out` is a malloc'd
// array of `n` pointers; each points to a malloc'd copy of the key's raw bytes
// (key_size bytes). The caller frees each key pointer and the array with
// free(). Used by Dictionary<K,V>.Keys for non-string keys and by iteration
// over a dict-backed Set<T> (§4.8 / §4.10). For a STRING key/element the bytes
// are the string-object pointer (key_size == sizeof(void*)); for a record they
// are the record's value bytes.
void bz_dict_keys_gen(void* d, void*** keys_out, int64_t* n_out);

// issue #537 / stdlib §4.8: enumerate Dictionary<K,V> values for
// `for var v in d.Values`.  Mirrors bz_dict_keys_str / bz_dict_keys_gen but
// snapshots values rather than keys.
//
// bz_dict_values_str — for string-keyed dictionaries (key_size == 0). Returns
// a malloc'd array of `n` void* slot values (one per live entry). The caller
// frees the array with free(); the values themselves are NOT freed here (they
// are owned by the dictionary; the loop body borrows them).
void bz_dict_values_str(void* d, void*** vals_out, int64_t* n_out);
//
// bz_dict_values_gen — for generic-key dictionaries (key_size > 0). Returns
// the same shape: a malloc'd void** array of `n` slot values.
void bz_dict_values_gen(void* d, void*** vals_out, int64_t* n_out);

// fix #1426 / stdlib §4.17–§4.18: SortedDictionary/SortedSet ordered-key
// accessors.  O(n) scans over the hash-table representation.
//
// bz_dict_min_key_gen / bz_dict_max_key_gen — return a malloc'd copy of the
// least/greatest key bytes from a generic-key dict.  NULL if empty.
// cmp(a, b): a, b are direct pointers to key bytes (NOT void** of void*).
void* bz_dict_min_key_gen(void* d, int (*cmp)(const void* a, const void* b));
void* bz_dict_max_key_gen(void* d, int (*cmp)(const void* a, const void* b));
//
// bz_dict_min_key_str / bz_dict_max_key_str — least/greatest key in a string-key
// dict.  Fills *out_data (a malloc'd `*out_len * *out_w`-byte copy), *out_len
// (code points) and *out_w (storage width, fix #4499).  False if empty.
bool bz_dict_min_key_str(void* d, char** out_data, int64_t* out_len,
                         int8_t* out_w);
bool bz_dict_max_key_str(void* d, char** out_data, int64_t* out_len,
                         int8_t* out_w);
//
// Sorted-snapshot helpers: reorder snapshot arrays in ascending key order.
// O(n log n); used for SortedDictionary/SortedSet iteration (§4.17/§4.18).
//
// bz_sort_kv_gen: sort keys[] and values[] together.
// cmp(a, b): a, b = direct pointers to key bytes.
void bz_sort_kv_gen(void** keys, void** values, int64_t n,
                    int (*cmp)(const void* a, const void* b));
//
// bz_sort_kv_str: sort char** keys + int64_t* lens + int8_t* widths +
// void** values together by lexicographic string comparison (§9.6). The width
// spine is permuted with the others (fix #4499); the COMPARISON itself is still
// byte-wise strcmp and is #4493's to make code-point-aware.
void bz_sort_kv_str(char** keys, int64_t* lens, int8_t* widths, void** values,
                    int64_t n);
//
// bz_sort_keys_gen: sort a void** key-bytes-pointer array in ascending order.
void bz_sort_keys_gen(void** keys, int64_t n,
                      int (*cmp)(const void* a, const void* b));
//
// bz_sort_keys_str: sort char** + int64_t* + int8_t* arrays in ascending order.
void bz_sort_keys_str(char** keys, int64_t* lens, int8_t* widths, int64_t n);

//
// #628 / stdlib §4.9 v0.135: ownership-transferring drain iteration.
//
// bz_dict_drain_next_str — string-keyed dict (key_size == 0).
// Pops the next live entry from the dict in insertion order. Returns true if
// an entry was popped; false when the dict is empty (drain complete). The
// caller owns the popped value (val_out) and the malloc'd key copy (key_out);
// it must free(key_out) or adopt the bytes, and dispose the value.
// `cursor` is a caller-supplied int64_t scan position (start at 0).
bool bz_dict_drain_next_str(void* d, int64_t* cursor,
                             char** key_out, int64_t* key_len_out,
                             int8_t* key_w_out, void** val_out);
//
// bz_dict_drain_next_gen — generic-keyed dict (key_size > 0).
// Same as bz_dict_drain_next_str but the key is returned as a malloc'd copy
// of the raw key bytes (size = dict->key_size). The caller must free(key_out).
bool bz_dict_drain_next_gen(void* d, int64_t* cursor,
                             void** key_out, void** val_out);

// issue #639 / stdlib §4.5/§4.7/§4.11 v0.135: ordered owning collection drain.
//
// bz_array_drain_next — pops index 0 (front-to-back); for OwningArray<T>.Drain()
// (front-to-back) and OwningQueue<T>.Drain() (FIFO). Returns true if an element
// was popped into *val_out; false when the array is empty.
bool bz_array_drain_next(void* arr, void** val_out);
//
// bz_array_drain_next_lifo — pops the last element (LIFO); for OwningStack<T>.Drain()
// which pops top-first. Returns true if an element was popped; false when empty.
bool bz_array_drain_next_lifo(void* arr, void** val_out);
//
// bz_llist_drain_next — pops the head node (front-to-back); for OwningLinkedList<T>.Drain()
// which iterates head-to-tail. Returns true if a node was popped; false when empty.
bool bz_llist_drain_next(void* list, void** val_out);

// issue #389 / stdlib §4.10: fold a key snapshot of `other` into/against a
// dict-backed Set `self` (generic-key table). `keys` is a malloc'd array of `n`
// pointers, each a malloc'd copy of one element's `self->key_size` bytes — the
// same shape bz_dict_keys_gen produces, or that bz_set_collect_enumerator
// builds from a non-Set IEnumerable<T> (#326). `op` selects the operation:
//   0 UnionWith     — set every key into self (idempotent by content)
//   1 ExceptWith    — remove every key from self
//   2 IntersectWith — keep only self keys present in the snapshot (mutate-free:
//                     a temp membership set is built from the snapshot, then
//                     self's own keys are snapshotted and the absent ones removed)
//   3 IsSubsetOf    — true iff every self element is in the snapshot
//   4 IsSupersetOf  — true iff every snapshot element is in self
// The function FREES each `keys[i]` and `keys` before returning. The return is
// 1/0 for the query ops (3,4) and 0 for the mutators (0,1,2). The dict-backed
// Set holds value-semantic elements (records/primitives/enums/strings), so no
// element disposal is performed — the removed keys are just freed.
int32_t bz_set_algebra_gen(void* self, void** keys, int64_t n, int32_t op);

// issue #429 / §28.9 / §4.10: reference-counting helpers for Set<string>.
// A Set<string> stores each element as a bz_string* in the generic-key table's
// key slot (key_size == sizeof(void*)). These helpers manage the per-element
// +1 that the set co-owns:
//   bz_set_string_keys_release — release every live element's +1 by reading
//     the string pointer from each key slot. Call BEFORE bz_dict_free.
//   bz_dict_clear_set_strings  — like bz_dict_clear(d, null) but also releases
//     each key's string +1 before freeing the key copy.
//   bz_dict_remove_set_string  — like bz_dict_remove_gen but releases the
//     string element's +1 before freeing the key copy.
void bz_set_string_keys_release(void* d);
void bz_dict_clear_set_strings(void* d);
bool bz_dict_remove_set_string(void* d, const void* key);

// fix #4091 / §28.11 / §28.9: free each live element of a dict-backed
// `OwningSet<T>` whose T is itself a BORROWING built-in collection (Array /
// Stack / Queue / LinkedList / FrozenArray / Dictionary / SortedDictionary).
// The set stores each element as a void* in the generic-key table's key slot
// (key_size == sizeof(void*)); this helper calls `dispose_coll` (a thunk
// built by codegen that invokes the matching runtime free fn — bz_array_free,
// bz_llist_free, or bz_dict_free — with NULL inner disposer, per #4054) on
// each non-null live key, then the subsequent bz_dict_free(d, null) reclaims
// the key copies and the table itself. null/empty-table safe; a no-op on a
// non-generic table (key_size != sizeof(void*), e.g. a primitive-element Set).
void bz_set_coll_keys_dispose(void* d, void (*dispose_coll)(void*));

// issue #389 / stdlib §5: wrap a dict-backed Set<T> (§4.10) as a lazy LINQ
// source (Set is IEnumerable<T> via ICollection<T>, §4.3). Snapshots the set's
// element keys (bz_dict_keys_gen) into an owned buffer iterated by index; the
// snapshot is freed when the enumerator is disposed. `elem_size` is the stored
// element byte count (the set's key_size). Mirrors bz_lazy_from_array.
void* bz_lazy_from_set(void* set, int64_t elem_size);

// ---------------------------------------------------------------------------
// Random number generator
// ---------------------------------------------------------------------------

void*   bz_random_alloc(int64_t seed);
void    bz_random_free(void* r);
int32_t bz_random_next_int(void* r, int32_t lo, int32_t hi_exclusive);
double  bz_random_next_double(void* r);
void    bz_random_next_bytes(void* r, uint8_t* buf, int64_t len);
// per stdlib §2.7 / #1359: NextInt64 (renamed from NextLong) — full 64-bit.
int64_t bz_random_next_long(void* r);
// per stdlib §2.7 / #1359: NextBoolean — 1 bit from PRNG.
int32_t bz_random_next_bool(void* r);

// ---------------------------------------------------------------------------
// decimal (§4.4 / stdlib §9.3) — 128-bit high-precision decimal type.
//
// Layout: .NET-compatible four-word struct (16 bytes, same as System.Decimal):
//   { uint32_t lo32, uint32_t mid32, uint32_t hi32, uint32_t flags }
// where lo32/mid32/hi32 are the low/middle/high 32 bits of the 96-bit
// unsigned integer mantissa, and flags encodes:
//   bit 31        = sign (1 = negative)
//   bits 16..23   = scale (0..28, decimal point position from the right)
//   all other bits = 0
//
// All functions take a pointer to bz_decimal_t for ABI clarity (16-byte
// struct passing is architecture-specific; pointer avoids the ambiguity).
// Return values are written via an out-pointer too.  The caller allocates
// stack storage (entry alloca) and passes its address.
//
// Errors (per fix #521 / §6.4):
//   bz_decimal_div and bz_decimal_mod: divisor-zero precondition is checked
//     INLINE by the codegen (DivideByZeroException via emitGuardThrow).
//   bz_decimal_add/sub/mul/div/mod: accept ove_vtable (OverflowException vtable);
//     on overflow store the exception object in bz_exc and return.
//     Callers call emitPendingCheck() after each call.
//   bz_decimal_from_double: same — ove_vtable for out-of-range.
//   bz_decimal_parse / bz_decimal_parse_cstr: ove_vtable for out-of-range,
//     fe_vtable (FormatException vtable) for malformed input.
// ---------------------------------------------------------------------------

typedef struct { uint32_t lo32; uint32_t mid32; uint32_t hi32; uint32_t flags; }
    bz_decimal_t;

// Arithmetic — result written to *out.
// per fix #521 / §6.4: ove_vtable is the OverflowException vtable (may be
// null at compile-time; emitPendingCheck() is called by codegen after each op).
void bz_decimal_add(const bz_decimal_t* a, const bz_decimal_t* b,
                    bz_decimal_t* out, void* ove_vtable);
void bz_decimal_sub(const bz_decimal_t* a, const bz_decimal_t* b,
                    bz_decimal_t* out, void* ove_vtable);
void bz_decimal_mul(const bz_decimal_t* a, const bz_decimal_t* b,
                    bz_decimal_t* out, void* ove_vtable);
void bz_decimal_div(const bz_decimal_t* a, const bz_decimal_t* b,
                    bz_decimal_t* out, void* ove_vtable);
void bz_decimal_mod(const bz_decimal_t* a, const bz_decimal_t* b,
                    bz_decimal_t* out, void* ove_vtable);
void bz_decimal_neg(const bz_decimal_t* a, bz_decimal_t* out);

// Comparison — returns negative/zero/positive like memcmp.
int32_t bz_decimal_cmp(const bz_decimal_t* a, const bz_decimal_t* b);

// fix #685 / §4.4 / §8.3: content-based hash consistent with bz_decimal_cmp
// equality (1.0m and 1.00m compare equal → must hash identically). Normalizes
// by stripping trailing zeros from the mantissa+scale, then folds the canonical
// (sign, mantissa) into a 32-bit value.  Always returns 0 for any zero value.
int32_t bz_decimal_hash(const bz_decimal_t* a);

// Conversions — from integer (per §5.1 implicit widening table).
void bz_decimal_from_int64(int64_t v, bz_decimal_t* out);
void bz_decimal_from_int32(int32_t v, bz_decimal_t* out);

// Conversions — from/to double (explicit only, per §5.2).
// per fix #521: ove_vtable for out-of-range values.
void   bz_decimal_from_double(double v, bz_decimal_t* out, void* ove_vtable);
double bz_decimal_to_double(const bz_decimal_t* a);

// Conversions — to integer (explicit only, per §5.2).
// §5.2: truncates toward zero; raises OverflowException (via ove_vtable) when
// the truncated integer value does not fit in the target type's range.
int32_t bz_decimal_to_int32(const bz_decimal_t* a, void* ove_vtable);
int64_t bz_decimal_to_int64(const bz_decimal_t* a, void* ove_vtable);
// #4785 / §5.2: unsigned mirror of bz_decimal_to_int64.  The codegen
// `ulong(decimal)` arm at LlvmBackend_Exprs2.cpp:9825-9833 routes ulong
// targets here; rejecting negative decimals and values > uint64.MaxValue.
uint64_t bz_decimal_to_uint64(const bz_decimal_t* a, void* ove_vtable);

// Parse from string (stdlib §9.3 Decimal.Parse / TryParse).
// per fix #521: bz_decimal_parse raises FormatException (fe_vtable) for
// malformed input and OverflowException (ove_vtable) for out-of-range.
// bz_decimal_try_parse returns 0/1 with no exception, and writes
// default(decimal) to *out on failure (stdlib §2.3).
// per fix #2273: these two enforce the §9.3 grammar — "an optional leading sign
// (+/-) followed by decimal digits with at most one `.`" — which admits NO
// exponent, and they read the string's full length (a `string` is not
// NUL-terminated and is not bounded by any fixed buffer here).
void    bz_decimal_parse(const void* str, bz_decimal_t* out,
                         void* ove_vtable, void* fe_vtable);
int32_t bz_decimal_try_parse(const void* str, bz_decimal_t* out);

// ToString (stdlib §9.3 Decimal.ToString()) — returns a new bz_string* (rc=1).
void* bz_decimal_to_string(const bz_decimal_t* a);

// Round(digits: int) — result written to *out.
void bz_decimal_round(const bz_decimal_t* a, int32_t digits, bz_decimal_t* out);

// Literal parse at compile-time (called by runtime init for MinValue/MaxValue).
// Takes a null-terminated C string.
// per fix #521: ove_vtable/fe_vtable may be nullptr for compile-time use
// (literals are lexer-validated; overflow cannot occur for known constants).
// per fix #2273: this is the LITERAL grammar (language spec §18.1 / Appendix A)
// and therefore admits scientific notation (`1.23e2m`) — unlike
// bz_decimal_parse above, which implements the narrower stdlib §9.3 grammar.
// The compile-time literal fold and bz_decimal_from_double ("%.15g" emits
// exponent form) both depend on that admission.
void bz_decimal_parse_cstr(const char* s, bz_decimal_t* out,
                            void* ove_vtable, void* fe_vtable);

// ---------------------------------------------------------------------------
// Guid (stdlib §2.6) — 128-bit globally unique identifier.
// Layout: two uint64_t fields (lo, hi) — a 16-byte value type. All functions
// take pointers to bz_guid_t so the value can be passed without LLVM ABI
// pain. ToString returns a heap-allocated bz_string (rc := 1, caller owns).
// ---------------------------------------------------------------------------

typedef struct { uint64_t lo; uint64_t hi; } bz_guid_t;

// Fill *g with a random v4 UUID (RFC 4122).
void bz_guid_new(bz_guid_t* g);

// Return a heap-allocated bz_string* (rc := 1) with the canonical 36-char
// lowercase dashed form "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx".
void* bz_guid_to_string(const bz_guid_t* g);

// v0.172 (stdlib §2.18): Guid "N" — 32 lowercase hex digits, no hyphens.
void* bz_guid_to_string_n(const bz_guid_t* g);

// v0.197 (#1989, stdlib §2.6/§2.18): Guid.ToString(format) format overload —
// "D" (dashed) / "N" (no hyphens); returns nullptr for any other specifier
// (caller raises FormatException). `fmt_p` is a bz_string*.
void* bz_guid_to_string_fmt(const bz_guid_t* g, const void* fmt_p);

// v0.172 (#812/#1339, stdlib §2.18): floating shortest-round-trip renderers —
// the value used by no-format ToString() and the "R" specifier.
void* bz_double_to_string_r(double v);
void* bz_float_to_string_r(float v);
// v0.172 (stdlib §2.18): integer "B"/"Bn" binary rendering; `value` is the
// operand's natural-width bit pattern zero-extended to 64 bits.
void* bz_ulong_to_binary(uint64_t value, int32_t min_digits);
// v0.172 (stdlib §2.18): TimeSpan "c" constant format [-][d.]hh:mm:ss[.fffffff].
void* bz_timespan_to_string_c(int64_t ticks);

// Parse the canonical 36-char dashed form from a bz_string*. Returns 1 on
// success (writes *out), 0 on failure (leaves *out unchanged).
int32_t bz_guid_try_parse(const void* str, bz_guid_t* out);

// Parse the canonical form; abort/unreachable on failure (caller guarantees
// well-formed input — used by Guid.Parse where failure is an exception).
// Returns 1 on success, 0 on parse failure (codegen emits a trap on 0).
int32_t bz_guid_parse(const void* str, bz_guid_t* out);

// Bytewise equality: returns 1 if *a == *b, 0 otherwise.
int32_t bz_guid_equals(const bz_guid_t* a, const bz_guid_t* b);
// fix #4126 / stdlib §2.6 / §8.3 (v0.295): content hash consistent with
// bz_guid_equals (equal Guids hash identically).
int32_t bz_guid_hash(const bz_guid_t* g);
// per stdlib §2.6 / #1357: CompareTo — 16-byte lexicographic (canonical byte order).
// Returns negative/0/positive like memcmp.
int32_t bz_guid_compare(const bz_guid_t* a, const bz_guid_t* b);
// per stdlib §2.6 / #1357: FromByteArray — copy exactly 16 bytes into *out.
void bz_guid_from_bytes(const uint8_t* bytes, bz_guid_t* out);

// ---------------------------------------------------------------------------
// DateTime / TimeSpan (100-ns ticks since 0001-01-01 00:00:00 UTC,
// matching the Bezel spec §2.5 which follows .NET DateTime semantics).
//
// DateTime is a value type stored as a scalar int64 tick count.
// TimeSpan is likewise a scalar int64 tick count (signed duration).
// ---------------------------------------------------------------------------

int64_t bz_datetime_now_ticks(void);
int64_t bz_datetime_utcnow_ticks(void);

// Decompose ticks into calendar/clock components.
void bz_datetime_ticks_to_parts(int64_t ticks,
                                int32_t* year, int32_t* month,  int32_t* day,
                                int32_t* hour, int32_t* minute, int32_t* second,
                                int32_t* ms);

// Build ticks from calendar/clock components (no validation).
int64_t bz_datetime_parts_to_ticks(int32_t year, int32_t month,  int32_t day,
                                    int32_t hour, int32_t minute, int32_t second,
                                    int32_t ms);

// Per §2.5: computed scalar properties extracted from a DateTime tick value.
int32_t bz_datetime_year(int64_t ticks);
int32_t bz_datetime_month(int64_t ticks);
int32_t bz_datetime_day(int64_t ticks);
int32_t bz_datetime_hour(int64_t ticks);
int32_t bz_datetime_minute(int64_t ticks);
int32_t bz_datetime_second(int64_t ticks);
int32_t bz_datetime_millisecond(int64_t ticks);
// DayOfWeek: 0=Sunday, 1=Monday, ..., 6=Saturday (per §2.5 DayOfWeek enum)
int32_t bz_datetime_day_of_week(int64_t ticks);
// DayOfYear: 1..366
int32_t bz_datetime_day_of_year(int64_t ticks);

// Per §2.5: Add* instance methods on DateTime (return new tick value).
// AddYears/AddMonths perform calendar-aware arithmetic; others are tick math.
int64_t bz_datetime_add_years(int64_t ticks, int32_t years);
int64_t bz_datetime_add_months(int64_t ticks, int32_t months);

// per stdlib §2.5 / #1357: Unix-time interop.
int64_t bz_datetime_to_unix_seconds(int64_t ticks);
int64_t bz_datetime_from_unix_seconds(int64_t seconds);

// ---------------------------------------------------------------------------
// Per §2.5: TimeSpan component properties (all computed from a signed i64
// tick count). Total* return double; component Days/Hours/Minutes/Seconds/
// Milliseconds return int (the partial component, not the total).
// ---------------------------------------------------------------------------

// Decompose a TimeSpan tick count into (days, hours, minutes, seconds, ms).
// All are signed; e.g. a negative TimeSpan gives negative components.
void bz_timespan_components(int64_t ticks,
                             int32_t* days, int32_t* hours,
                             int32_t* minutes, int32_t* seconds,
                             int32_t* ms);

// Total* properties (double): e.g. TotalMinutes = ticks / TICKS_PER_MINUTE.
double bz_timespan_total_days(int64_t ticks);
double bz_timespan_total_hours(int64_t ticks);
double bz_timespan_total_minutes(int64_t ticks);
double bz_timespan_total_seconds(int64_t ticks);
double bz_timespan_total_milliseconds(int64_t ticks);

// ---------------------------------------------------------------------------
// per stdlib §2.5 / #1357: TimeOnly — value type stored as i32 ms from
// midnight (0..86399999). Helpers for string conversion and parsing.
// ---------------------------------------------------------------------------

// Format ms_from_midnight as "HH:mm:ss" → fresh heap bz_string* (rc=1).
void* bz_timeonly_to_string(int32_t ms_from_midnight);
// Parse "HH:mm:ss" or "HH:mm:ss.fff" from a bz_string*. Returns 1 on success
// (writes *out_ms in [0..86399999]), 0 on failure.
int32_t bz_timeonly_try_parse(const void* str, int32_t* out_ms);

// ---------------------------------------------------------------------------
// File I/O — wraps POSIX read/write. All path parameters are NUL-terminated
// UTF-8. Returns 0 on success, a negative errno value on failure.
// Caller owns returned heap buffers (free with free()).
// ---------------------------------------------------------------------------

// Read entire file into a malloc'd byte buffer.
int32_t bz_file_read_all_bytes(const char* path, uint8_t** out, int64_t* len);

// Read entire file as UTF-8 text (NUL-terminated). *out is malloc'd.
int32_t bz_file_read_all_text(const char* path, char** out, int64_t* byte_len);

// Read file as lines — returns bz_array_t* of bz_string_struct* (heap-alloc'd).
// Each line string is malloc'd; the array itself is a bz_array_t of pointers.
// Caller disposes via bz_array_free(result, free) or manages lifetime.
void* bz_file_read_all_lines(const char* path);

// Write bytes to file (creates or truncates).
int32_t bz_file_write_all_bytes(const char* path, const uint8_t* data, int64_t len);

// Write UTF-8 text to file (creates or truncates).
int32_t bz_file_write_all_text(const char* path, const char* data, int64_t len);

// Append UTF-8 text to file (creates if absent).
int32_t bz_file_append_all_text(const char* path, const char* data, int64_t len);

bool    bz_file_exists(const char* path);
bool    bz_file_delete(const char* path);
int32_t bz_file_copy(const char* src, const char* dst, bool overwrite);
int32_t bz_file_move(const char* src, const char* dst);

// Higher-level File I/O helpers that work directly with bz_array_t* (per §7.3).
// These are the codegen entry-points for File.ReadAllBytes/WriteAllBytes/WriteAllLines.

// Read file into a freshly-allocated bz_array_t* of uint8_t (elem_size=1).
// Returns the array on success (count == file bytes), or null on I/O error.
void*   bz_file_read_all_bytes_arr(const char* path);

// Write file from a bz_array_t* of uint8_t (elem_size=1). Returns 0 or -errno.
int32_t bz_file_write_all_bytes_arr(const char* path, void* arr);

// Write file from a bz_array_t* of bz_string* (Array<string>), joining with '\n'.
// Returns 0 on success, -errno on I/O failure.
int32_t bz_file_write_all_lines(const char* path, void* arr);

bool    bz_dir_exists(const char* path);
int32_t bz_dir_create(const char* path);   // creates all intermediate dirs
int32_t bz_dir_delete(const char* path, bool recursive);

// Returns malloc'd array of malloc'd strings (NUL-terminated paths).
// *out is a char** of length *count_out. Caller must free each string and the array.
void* bz_dir_get_files(const char* path, int64_t* count_out);
void* bz_dir_get_dirs(const char* path,  int64_t* count_out);

// Path utilities — return malloc'd NUL-terminated strings. Caller must free.
char* bz_path_combine(const char* a, const char* b);
char* bz_path_filename(const char* path);       // last component
char* bz_path_extension(const char* path);      // including dot, e.g. ".txt"
char* bz_path_stem(const char* path);           // filename without extension
char* bz_path_dirname(const char* path);        // directory part
char* bz_path_absolute(const char* path);       // realpath equivalent

// ---------------------------------------------------------------------------
// stdlib §7.2 FileStream (issue #645) — a buffered OS file handle backing the
// §7.1 Stream operations. `mode`/`access`/`origin` are the FileMode/FileAccess/
// SeekOrigin ordinals (declaration order from 0, §10.2); see the
// implementation comment in bezel_runtime_fileio.cpp for the mapping.
// ---------------------------------------------------------------------------

// `new FileStream(path, mode[, access])`. path_data/path_len are the bz.string
// payload (not NUL-terminated). Null on an OS open failure.
void*   bz_filestream_open(const char* path_data, int64_t path_len,
                           int32_t mode, int32_t access);
// Flush-on-dispose (§7.2): flush buffered writes, close the handle, free.
// Idempotent and null-tolerant.
void    bz_filestream_dispose(void* h);
void    bz_filestream_flush(void* h);
// Windowed read/write over a bz_array_t* of bytes. Both return -1 for an
// out-of-range window so the caller raises IndexOutOfRangeException (§7.1)
// before any I/O; bz_filestream_read otherwise returns the byte count (0 at
// end of stream), bz_filestream_write returns 0 (whole window) or -2.
int64_t bz_filestream_read(void* h, void* arr, int32_t offset, int32_t count);
int32_t bz_filestream_write(void* h, void* arr, int32_t offset, int32_t count);
int64_t bz_filestream_seek(void* h, int64_t offset, int32_t origin);
int64_t bz_filestream_length(void* h);
int64_t bz_filestream_position(void* h);
void    bz_filestream_set_position(void* h, int64_t pos);
int32_t bz_filestream_can_read(void* h);
int32_t bz_filestream_can_write(void* h);
int32_t bz_filestream_can_seek(void* h);
// ReadAll(): current position to end, as a fresh owned Array<byte>.
void*   bz_filestream_read_all(void* h);
// The `Path` property — a fresh +1 bz.string.
void*   bz_filestream_path(void* h);

// ---------------------------------------------------------------------------
// Task scheduler — minimal cooperative/preemptive scheduler for async/await.
// Backed by a POSIX thread pool. Call bz_scheduler_init() once at startup.
// ---------------------------------------------------------------------------

void  bz_scheduler_init(int num_threads);
void  bz_scheduler_shutdown(void);

// Allocate a task object (opaque bz_task_t*).
void* bz_task_alloc(void);

// Schedule `fn(task, state)` on the thread pool. `fn` must eventually call
// bz_task_complete() or bz_task_complete_error().
void  bz_task_schedule(void* task, void (*fn)(void* task, void* state),
                       void* state);

// stdlib §6.7 / language spec §30.10: Task.Delay(milliseconds). Returns a fresh
// task that is NOT yet complete; a thread-pool worker sleeps `ms` milliseconds
// then completes it with no result. This is the genuinely-incomplete task that
// drives the cross-thread suspend/resume path (§30.13): `await Task.Delay(...)`
// in a generated async method registers its continuation via bz_task_then and
// returns to the caller; the worker fires the continuation on the pool when the
// sleep elapses. The scheduler is started lazily if not already running. A
// non-positive `ms` completes the task essentially immediately (still off-thread).
void* bz_task_delay(int64_t milliseconds);

// Mark the task as done with a result (result_size bytes copied from result).
// Wakes any blocked awaiter and runs registered continuations.
void  bz_task_complete(void* task, const void* result, int64_t result_size);

// Mark the task as failed with an error message.
void  bz_task_complete_error(void* task, const char* error_msg);

// §30.9: mark the task as failed with the ORIGINAL exception object (a Bezel
// heap object with a leading vtable word). The awaiter re-raises this exact
// object into its enclosing handler. The task does not own the object.
void  bz_task_complete_error_obj(void* task, void* error_obj);

// Check completion without blocking.
bool  bz_task_is_done(void* task);
bool  bz_task_is_error(void* task);

// §30.5 (v0.162 / #1352): OwningTask single-consumer spent tracking.
// bz_task_mark_spent: record that an OwningTask has been consumed by its first
// await (success or fault). A second await on a spent task raises IOE.
void  bz_task_mark_spent(void* task);
// bz_task_is_spent: returns true after the first await consumed the OwningTask.
bool  bz_task_is_spent(void* task);

// §30.5 / #1332 (v0.162): Pattern A chained slot.
// Transfer ownership of `source` (a Pattern A Task<T:class> result producer) into
// `outer`. The outer task keeps the source alive (and its result buffer valid) until
// bz_task_free(outer) is called, which frees the chain transitively. Used when an
// async method does `return await <Pattern A source>` — the result is a borrow
// through the source, so the source must outlive the outer task.
void  bz_task_set_chained(void* outer, void* source);

// §30.5 / #1503: attach a per-T disposer for `task`'s produced class-typed
// result. bz_task_free runs `disposer` on the result's pointee (once, before
// reclaiming the result buffer) iff the task is not faulted and its result
// was never consumed (a consuming await/`.Result` nulls the slot first via
// bz_task_take_result). Safe to call before the task has completed — the
// disposer fires whenever bz_task_free eventually runs, whether that is
// immediately (already-done) or later via bz_task_fire_and_forget's
// completion continuation (§30.5 "the Dispose() call attaches a continuation
// that disposes the produced result whenever it appears").
void  bz_task_set_result_disposer(void* task, void (*disposer)(void*));

// fix #4159 (§30.5 "Disposing an unconsumed OwningTask<T>", v0.298): queried
// by emitAsyncPatternAResultDecision right after bz_task_set_chained, to
// decide whether the chaining body must ALSO attach its own kind-1 disposer
// for the value it just extracted from `source`. Returns 1 ("orphaned — the
// caller must attach its own disposer") iff `source` has settled with
// NEITHER a chained_source NOR a result_disposer of its own — i.e. nothing
// else will ever dispose the value about to be extracted from it. Returns 0
// when either is set: a genuine Pattern A chain already disposes the result
// transitively, and attaching a second disposer would double-free it.
int8_t bz_task_needs_caller_disposal(void* source);

// Block the calling thread until the task completes. Returns a pointer to the
// result buffer (owned by the task; valid until bz_task_free). Returns null on error.
void* bz_task_await_sync(void* task);

// Retrieve the error message (valid after bz_task_is_error returns true).
const char* bz_task_error(void* task);

// §30.9: retrieve the original exception object (valid after bz_task_is_error).
// Null when the fault carried only a message.
void* bz_task_error_obj(void* task);

// §30.9 / #3030: null the task's error_obj slot. Codegen's scope-end
// fault-disposal preamble reads error_obj via bz_task_error_obj, calls
// emitDisposeAndReclaim on the result, and then calls bz_task_fire_and_forget.
// Pre-#3030 bz_task_free never read error_obj, so leaving the field dangling
// was fine; post-#3030 the FAST path of bz_task_fire_and_forget inspects
// error_obj to decide whether to dispose it (the FAST path is reachable from
// the explicit `Task.FireAndForget(owned t)` user-facing call, which has no
// codegen preamble), so a non-null dangling field would be re-disposed ->
// double-free. This setter nulls the field after codegen's dispose so the
// FAST path's `if (error_obj && ...)` guard sees null and skips. Lock-free:
// by the time codegen runs the preamble the task has settled.
void  bz_task_clear_error_obj(void* task);

// §30.10 / #1529: true if `task`'s error_obj is a BORROWED reference from an
// input task's owned fault (a WhenAll/WhenAll<T> aggregate that propagated an
// input's fault), not owned by `task` itself. Consulted before any fault-
// disposal decision (scope-end, await/`.Result` hand-off) so a borrowing
// aggregate never double-frees an exception its input task still owns.
int8_t bz_task_fault_borrowed(void* task);

// Register a continuation to run when the task completes (on the thread pool).
void  bz_task_then(void* task, void (*cont)(void* task, void* state), void* state);

// fix #3460 (§28.3 / §30.1): increment the task's reference count. Pairs with
// `bz_task_release`; the producer's original reference (the one they balance
// with `bz_task_free`) starts at 1. Used by codegen to make the heap frame
// hold a strong reference to its own `outerTask` for the frame's lifetime, so
// the worker resume continuation can safely read `frame[OUTER_TASK_FIELD]`
// even when the caller's `bz_task_free(outerTask)` from `using var` scope-end
// has already run. Null-safe (a no-op for `task == null`); safe to call on a
// task that has already completed (the refcount outlives task completion).
void  bz_task_retain(void* task);

// fix #3460 (§28.3 / §30.1): decrement the task's reference count; when the
// count reaches zero, dispose the result / error / chained source and free
// the task object exactly as `bz_task_free` did before #3460. `bz_task_free`
// itself is now a thin wrapper around this; both behave identically. Null-
// safe. The final release (the one whose decrement reaches zero) frees the
// struct after the existing `bz_task_free` body runs; intermediate releases
// only touch the atomic counter.
void  bz_task_release(void* task);

// Free the task object. Must be called after the result has been consumed.
// Since #3460 this is a thin wrapper around `bz_task_release`; both balance
// the producer's original reference (counted at `bz_task_alloc`).
void  bz_task_free(void* task);

// fix #3460 (§28.3 / §30.1): cross-task borrow tracking on heap objects.
// Classes in Bezel are single-owner (no GC, no per-class refcount), so a
// borrowed reference to a class instance cannot extend its lifetime through
// refcounting alone. When an async method captures `this = c` into its heap
// frame, the frame must hold a *borrow* on c that outlives every pending
// resumption (the spec §28.3 claim that "the borrow of the captured state
// must outlive any pending resumption"). The borrow registry is a
// process-global side table: bz_register_borrow(c) increments the borrow
// count for c; bz_release_borrow(c) decrements; bz_wait_for_borrows(c)
// blocks until the borrow count for c reaches zero. Codegen emits
// register/release at the frame's lifetime boundary, and emits
// wait_for_borrows at the OWNER's dispose site (e.g. `using var c := new C()`
// scope-end) BEFORE the actual `free(c)`. With both halves in place, the
// race of #3460 — main's `using var c` exit firing `free(c)` while the
// worker's resume continuation still reads `this._h` — closes: the dispose
// site blocks until the worker has run emitAsyncComplete and released its
// borrow. Null-safe; the entry is removed when the count returns to zero
// so the side table does not grow unbounded for long-running services.
void  bz_register_borrow(void* obj);
void  bz_release_borrow(void* obj);
void  bz_wait_for_borrows(void* obj);

// stdlib §6.7 / §30.10: Task.FromResult<T>(value). Allocate a task and
// immediately complete it with the `result_size` bytes at `result`. The
// returned task is already-done; `await` on it takes the fast path. For
// value-typed T the bytes are the value; result_size == 0 is also valid
// (produces an already-completed no-value task, like CompletedTask).
void* bz_task_from_result(const void* result, int64_t result_size);

// stdlib §6.7 / §30.10: Task.WhenAll(tasks: Array<Task>). Takes a
// bz_array_t* of `n` task pointers (ptr-sized elements), returns a NEW
// task that completes (with no value) once ALL input tasks are done.
// Caller owns the returned task. The input array is BORROWED; the caller
// is responsible for its lifetime.
void* bz_task_when_all(void* task_arr);

// stdlib §6.7 / §30.10: Task.WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>.
// Variant of bz_task_when_all that collects each Task<T>'s result into a new
// bz_array_t. `elem_size` is sizeof(T) in bytes. The aggregate task completes
// with a ptr-sized result (the Array<T>* pointer). Fault propagation matches
// bz_task_when_all. Caller owns the returned task.
void* bz_task_when_all_results(void* task_arr, int64_t elem_size);

// stdlib §6.7 / §30.10: Task.WhenAny(tasks: Array<Task>): Task<Task>.
// Takes a bz_array_t* of task pointers, returns a NEW task that completes
// (with a ptr-sized result — the pointer to the FIRST task that finishes)
// as soon as any one input task is done. Caller owns the returned task.
// The input array is BORROWED.
void* bz_task_when_any(void* task_arr);

// stdlib §6.7 / §30.10: Task.WhenAllOwned<T>(owned tasks: OwningArray<OwningTask<T>>):
//   OwningTask<OwningArray<T>> where T: class.
// Pattern B aggregator for class T. Caller transfers ownership of the
// OwningArray<OwningTask<T>>; each OwningTask<T> is awaited and its result
// (a class pointer) collected into a new OwningArray<T>. The input array and
// all input tasks are freed. Returns an OwningTask<OwningArray<T>>.
void* bz_task_when_all_owned(void* task_arr);

// stdlib §6.7 / §30.10: Task.FireAndForget(owned task). Registers a
// continuation on `task` that frees it when done (after discarding any
// result or logging any error). Ownership of `task` transfers; the
// caller must not use or free the pointer again.
void  bz_task_fire_and_forget(void* task);

// §30.10 (v0.179 / #1520): checked-disposal-mode entry point for the `using
// var t := call();` scope-end drop of an unconsumed Task (the "unsupervised"
// abandonment idiom). Identical machinery to bz_task_fire_and_forget, plus a
// backstop: if `task` is still running (genuinely abandoned — its continuation
// resumes only after this disposing scope has already exited) and its
// eventual fault is a DisposedAccessException (vtable == disposedAccessVtable),
// the fault is first reported through the runtime's diagnostic channel (the
// same one FireAndForget's own fault logging uses) and then swallowed — "the
// backstop that catches a §30.1 source-outlives-settlement violation must
// never itself be invisible." Codegen passes the compiled DisposedAccessException
// vtable global; only emitted when compiled with --checked-disposal.
void  bz_task_fire_and_forget_checked(void* task, void* disposedAccessVtable);

// stdlib §6.7 (v0.212) / proposal 0023 D1-D7: Task.WhenEach streaming fan-in.
// `bz_task_when_each_init_borrowing` builds the runtime backing for
// `Task.WhenEach(tasks: Array<Task>)` and `Task.WhenEach<T>(tasks:
// Array<Task<T>>)`. The returned pointer is a vtable-backed object that
// implements IAsyncEnumerable<Task> / IAsyncEnumerable<Task<T>> via vtable
// slots [0]=self, [1]=Dispose, [2]=MoveNextAsync, [3]=Current. The input
// array is BORROWED (the caller is responsible for its lifetime — the
// §28.3 deferred-activation frame carriage binds the borrow to the returned
// iterable so the compiler enforces source-outlives-settlement). `is_typed`
// selects the typed variant (Task<T> element vs. plain Task). Yielded
// elements are already-completed task pointers; faulted/cancelled tasks are
// yielded (not skipped) and surface when the caller awaits the yielded
// element. Empty input -> enumerator that completes immediately (MoveNextAsync
// returns false on first call). Affine per §29.11: a second GetAsyncEnumerator
// raises InvalidOperationException.
void* bz_task_when_each_init_borrowing(void* task_arr, int32_t is_typed);

// stdlib §6.7 (v0.212) / proposal 0023 D7: Task.WhenEachOwned<T> for class T.
// Takes ownership of the OwningArray<OwningTask<T>>; on dispose, any
// unconsumed OwningTask<T> tasks are freed (via bz_task_free - their result
// disposer cascades to dispose the produced class value per §30.5). Yields
// each completed OwningTask<T> through IOwningAsyncEnumerable<OwningTask<T>>.
// Otherwise identical semantics to the borrowing variant. `result_disposer`
// is T's destructor thunk (codegen-synthesized), attached to every input
// task so an UNCONSUMED task's produced class value is disposed exactly once
// on Dispose (#2096: a Task.FromOwnedResult task carries no disposer of its
// own once moved into the array). Null when there is nothing to dispose.
void* bz_task_when_each_init_owning(void* owning_task_arr,
                                    void (*result_disposer)(void*));

// ---------------------------------------------------------------------------
// stdlib §6.10 / spec §28.14 (v0.175, #808): TaskScope — the structured-
// concurrency nursery. See the implementation block comment for the full
// lifecycle contract (Spawn/Join/DisposeAsync/unwind-dispose, first-fault-
// wins + cancel-siblings, drain completeness, quiescence).
// ---------------------------------------------------------------------------

// new TaskScope(): allocate the scope with a fresh internal cancellation cell.
void* bz_task_scope_new(void);
// Token property: the scope's CancellationToken handle (borrowed).
void* bz_task_scope_token(void* scope);
// Cancel(): cancel all children now (same signal path as CTS.Cancel).
void  bz_task_scope_cancel(void* scope);
// CancelAfter(delay-ms): deadline for the whole scope (#1413 timer path).
void  bz_task_scope_cancel_after(void* scope, int64_t ms);
// Spawn(scoped body): invoke `closure` (a `() -> Task` cell, transferred +1)
// synchronously on the current domain and track the child. After close:
// InvalidOperationException via bz_exc (caller emits the pending check).
void  bz_task_scope_spawn(void* scope, void* closure, void* ioe_vtable);
// await Join(): returns a task completing when all children (including ones
// spawned during the drain) are done; re-raises the FIRST fault; closes the
// scope. Caller owns the returned task.
void* bz_task_scope_join(void* scope);
// await using normal-exit disposal: cancel + drain + swallow; faults the
// returned task with InvalidOperationException iff `normal_exit` and Join
// was never awaited. Frees the scope when the returned task completes.
// Caller owns the returned task.
void* bz_task_scope_dispose_async(void* scope, int32_t normal_exit,
                                  void* ioe_vtable);
// Unwind-edge / sync-fallback disposal: cancel + NON-BLOCKING zombie drain
// (the scope is freed at the last child completion). Never raises.
void  bz_task_scope_dispose_unwind(void* scope);

// ---------------------------------------------------------------------------
// Array enumerator (Part B2): wraps bz_array_t* as a vtable-based IEnumerator<T>.
// The returned object has vtable slots: [0]=td/self, [1]=Dispose, [2]=MoveNext,
// [3]=get_Current. This makes Array<T> usable wherever IEnumerator<T> is expected.
//
// Usage: bz_array_enum_alloc(arr, esz) → opaque enumerator ptr
//        [vtable.MoveNext](self) → bool
//        [vtable.get_Current](self) → void* (pointer to current element in arr)
//        [vtable.Dispose](self) → void (frees the enumerator struct)
//
// elem_size: bytes per element (4 for int, 8 for ptr/double, etc.)
void* bz_array_enum_alloc(void* arr, int64_t elem_size);

// ---------------------------------------------------------------------------
// Lazy LINQ enumerators (stdlib §5). Each operator returns a deferred
// enumerator (vtable [td, Dispose, MoveNext(->i32), Current(->void*)]) that
// does no work until enumerated. Operators compose by storing the upstream
// enumerator; an Array<T> source is wrapped via bz_lazy_from_array. Closures
// are invoked through codegen trampolines (fixed C signatures, see .cpp).
// ---------------------------------------------------------------------------
void* bz_lazy_from_array(void* arr, int64_t elem_size);
// stdlib §5 (#1914): wrap a FRESH `Array<IGrouping>` (a bz_array_t* of
// bz_grouping_t* pointers, produced by bz_groupby_to_array) as a lazy source
// that OWNS both the array AND every grouping inside it. bz_lazy_dispose frees
// each grouping (bz_groupby_free_groups) then the outer array. Used by the bare
// `for var g in seq.GroupBy(sel)` path, whose fresh groups temporary no name
// owns; the loop consumes each grouping during iteration and disposes the lazy
// (freeing the groups) only at loop end — so there is no other owner (no
// double-free) and no post-free element read (no UAF).
void* bz_lazy_from_owned_grouping_array(void* arr, int64_t elem_size);
// per §4.6/§4.7 + §5: wrap a bz_llist_t* (LinkedList<T>/OwningLinkedList<T>) as a
// lazy source so LINQ aggregate/terminal operators can enumerate it. Walks nodes
// head-to-tail; does not own the list (caller still responsible for disposal).
void* bz_lazy_from_llist(void* llist, int64_t elem_size);
// issue #775 / §16.3: wrap a Range<T> (start/end/step/flags) as a lazy
// IEnumerable<T> source. flags bit0=inclusive, bit1=startOpen, bit2=endOpen.
void* bz_lazy_from_range(int64_t start, int64_t end, int64_t step,
                         int8_t flags, int64_t elem_size);
void* bz_lazy_where(void* up, void* closure, void* pred);            // pred: i32(closure, elem)
void* bz_lazy_select(void* up, int64_t out_elem_size,
                     void* closure, void* sel);                      // sel: void(closure, elem, out)
void* bz_lazy_take(void* up, int64_t n);
void* bz_lazy_skip(void* up, int64_t n);
void* bz_lazy_distinct(void* up, void* eqfn);                        // eqfn: i32(a, b) (null = raw bytes)
void* bz_lazy_orderby(void* up, void* closure, void* keyfn,
                      int32_t descending);                           // keyfn: i64(closure, elem)
// stdlib §5 Combination: Concat appends `down` after `up` (both lazy enumerators).
void* bz_lazy_concat(void* up, void* down);
// stdlib §5 Slicing: TakeWhile/SkipWhile gate by a predicate trampoline.
void* bz_lazy_takewhile(void* up, void* closure, void* pred);        // pred: i32(closure, elem)
void* bz_lazy_skipwhile(void* up, void* closure, void* pred);        // pred: i32(closure, elem)
// stdlib §5 Projection: SelectMany flattens (T)->IEnumerable<U>. The selector
// trampoline returns a fresh lazy enumerator (or a collection wrapped as one).
void* bz_lazy_selectmany(void* up, int64_t out_elem_size,
                         void* closure, void* sel);                  // sel: void*(closure, elem)
// stdlib §5 Combination: Zip pairs `up` and `down` positionally, applying a
// binary combine trampoline. Stops at the shorter of the two sources.
void* bz_lazy_zip(void* up, void* down, int64_t out_elem_size,
                  void* closure, void* comb);                        // comb: void(closure, a, b, out)
void* bz_lazy_to_array(void* lazy, int64_t elem_size);

// issue #389 / stdlib §4.10: drain a lazy enumerator (a non-Set IEnumerable<T>
// `other` of a set-algebra call) into the key-snapshot shape bz_dict_keys_gen
// produces: `*keys_out` is a malloc'd array of `n` pointers, each a malloc'd copy
// of one element's `elem_size` bytes. Disposes the drained enumerator. The
// snapshot is consumed (and freed) by bz_set_algebra_gen.
void bz_set_collect_enumerator(void* lazy, int64_t elem_size,
                               void*** keys_out, int64_t* n_out);

// ---------------------------------------------------------------------------
// Events (language spec §24). The backing object of an `event` declaration is
// an EventSource holding an ordered list of subscribed handlers. Each handler
// is a Bezel closure value — a heap-allocated { fn_ptr, env_ptr } struct
// pointer (see codegen lowerLambda). The payload type is type-erased here:
// `Raise` is lowered in codegen (it must call each closure with the concrete
// payload type), so the runtime only stores/produces the handler list.
//
// Handlers carry a monotonically-increasing token so a `Subscription` can
// later remove its own handler (idempotently) on Dispose without relying on
// closure-pointer identity.
// ---------------------------------------------------------------------------

// Allocate an empty EventSource. Returned pointer is opaque to Bezel code.
void* bz_event_source_new(void);

// Add a handler closure; returns its token (a positive int64). Used by both
// EventSource.Add and (indirectly) Subscribe.
int64_t bz_event_add(void* src, void* closure);

// Number of currently-active (not-yet-removed) handlers. Used by codegen's
// Raise loop.
int64_t bz_event_count(void* src);

// The closure pointer of the i-th active handler (0-based). Returns null if
// out of range. Codegen calls each with the payload.
void* bz_event_handler_at(void* src, int64_t i);

// Remove the (first) handler whose closure pointer equals `closure`. Used by
// EventSource.Remove (`-=`). No-op if not found. (Pointer identity: matches a
// handler subscribed via the same closure value — see §24 report note.)
void bz_event_remove_closure(void* src, void* closure);

// §27.7.1 (v0.180, #1544): general function-VALUE equality for the `=`/`<>`
// binary operator on two function-typed operands. Each closure cell carries a
// KIND tag (off 32 — see bezel_runtime_internal.hpp's BZ_FVK_* constants) that
// records which §27.7.1 rule the value obeys, so the operator and the `-=`
// desugaring (bz_event_remove_closure) agree and dispatch on kind:
//   - a lambda or value-shaped-receiver (record) reference equals ONLY itself
//     (reference identity on the cell) — two separately-constructed lambdas / two
//     separate record-receiver evaluations are NOT equal;
//   - a class-receiver method/property reference equals another with the same
//     fn (thunk) AND the same receiver pointer (repeated `obj.Method` on the
//     same class-typed `obj` is guaranteed equal);
//   - a static method/property reference equals every other reference to the
//     same method (same fn);
//   - a lambda never equals a method/property reference (distinct kinds).
// Returns 0/1, not C `bool` — matching this header's other codegen-called
// predicates and avoiding any i1-vs-i8 ABI ambiguity at the LLVM declareC
// boundary.
int32_t bz_closure_value_equals(void* a, void* b);

// Remove the handler with the given token. Idempotent (second call is a
// no-op). Used by Subscription.Dispose.
void bz_event_remove_token(void* src, int64_t token);

// Subscribe `closure`; returns an owned Subscription object (opaque). The
// Subscription remembers (src, token) and removes the handler when disposed.
void* bz_event_subscribe(void* src, void* closure);

// Dispose a Subscription: removes its handler from the source (idempotent).
void bz_subscription_dispose(void* sub);

// Free an EventSource and its handler list.
void bz_event_source_free(void* src);

// ---------------------------------------------------------------------------
// Shared event sources (stdlib §6.5, v0.187, fix #1832): the cross-domain
// publication-point pair SharedEventSource<T> / ISharedEvent<T>. The runtime
// representation is the same bz_event_source_t* the instance pair uses — the
// cross-domain contract (synchronized quiescence / lock discipline around the
// subscriber list for cross-domain subscribe/raise/Subscription.Dispose) is
// recorded as a follow-up slice; this first pair of entry points admits the
// type names by mirroring the instance surface. Reclaiming the source still
// severs the subscriber list before freeing the struct, so a publisher's
// disposal remains a deterministic unsubscribe-all (spec §28.14 quiescence).
// ---------------------------------------------------------------------------

// Allocate an empty SharedEventSource. Returns an opaque bz_event_source_t*.
// This is the shared path's construction entry — distinct from
// bz_event_source_new so a future mutex acquisition at construction time lands
// on the shared path without disturbing the instance path's drain.
void* bz_shared_event_source_new(void);

// Free a SharedEventSource and its handler list. Null-safe (mirroring
// bz_event_source_free: `if (!s) return;`).
void bz_shared_event_source_free(void* src);

// ---------------------------------------------------------------------------
// String reference counting (§4.6 / §28.9, issue #422 strings increment).
// `string` is runtime-managed (§4.6): its memory is reclaimed automatically.
// Strings are immutable, reference-shared LEAVES (§28.8), so acyclic reference
// counting is sound. The header carries an int64 rc (offset 24, total header =
// 32 bytes); every heap producer initializes it to 1 (the producer's
// reference). retain increments; release decrements and, at zero, frees the
// data buffer then the header. Literal/global strings carry a SENTINEL rc and
// are no-ops for both (a constant LLVM global must never be freed). null is a
// no-op. Non-atomic for now; becomes atomic when async lands.
void bz_string_retain(void* s);
void bz_string_release(void* s);

// §8.6 / §12.7 (#417): runtime discriminator for `o is string` / `o as string`
// / `o.ToString()` where o: Object holds a string. A string stored in an Object
// slot is a bare bz_string* whose first word is `char* data` (NOT a vtable). A
// class / boxed value / built-in exception in an Object slot has a vtable
// pointer at word 0. `vtable_table` is a null-terminated array of all the
// vtable/box-vtable/exception-vtable globals codegen emitted for this program;
// `obj` is a string iff it is non-null and `obj`'s first word is NOT one of
// those vtables. Returns 1 (string) / 0 (not a string / null). The scan is
// O(#vtables); the table is tiny (one entry per emitted type) and the call is
// only on the rare `is/as/ToString string-through-Object` path.
int32_t bz_obj_is_string(void* obj, void** vtable_table);

// §8.3 (v0.204, #2055): assigned identity hash. The class-default
// `GetHashCode` and the box-default identity hash call this helper instead of
// deriving from the object's storage address. On the first request for `obj`
// the runtime draws the next value from a per-domain deterministic sequence
// (or the startup/shutdown-thread sequence when no domain is active, §30.13)
// and stores it in a process-wide side-table keyed by object pointer; later
// requests for the same object return the stored value. Objects whose hash is
// never requested pay nothing (no per-object metadata word). The values are a
// pure function of the order in which objects' hashes are first requested —
// deterministic under §30.13 — and never expose addresses. Null returns 0.
int32_t bz_obj_identity_hash(void* obj);

// §8.3 / #2058: reap `obj`'s side-table entry at its free site. Emitted by
// codegen at every owned-class reclaim (immediately before the heap free) and
// called by `bz_box_release` at the last drop of a box. Bounds side-table
// growth for long-running services without affecting the deterministic counter
// sequence (§30.13). A no-op on null and on objects that were never hashed.
void bz_obj_identity_hash_forget(void* obj);

// §8.3 / #2058: test/debug accessor — current number of live entries in the
// identity-hash side-table. Used by the regression test for #2058 to assert
// the table is BOUNDED after many alloc+hash+free cycles; harmless in
// production (reads size() under the side-table lock), never called by
// generated code.
int32_t bz_identity_hash_table_size(void);

// ---------------------------------------------------------------------------
// Closure-cell reference counting (§27.5 / §28.9, issue #422 closures
// increment; issue #672 typed env-disposer foundation).
// Function-typed values (lambda closures, capturing or not, and method
// references) are runtime-managed (§28.9): the heap-allocated closure object
// backing the value is reclaimed automatically.  The cell is laid out
//   { void* fn (off 0), void* env (off 8), int64 rc (off 16),
//     void* envdtor (off 24), int64 kind (off 32) }  total 40 bytes.
// fn (off 0) and env (off 8) keep their offsets.  rc at off 16 is unchanged.
// kind (off 32, §27.7.1 / #1544): a tag recording the function-value equality
// rule (see bezel_runtime_internal.hpp's BZ_FVK_* constants); consulted only by
// bz_closure_same / bz_closure_value_equals, never by retain/release.
// envdtor (off 24, #672 foundation): a per-lambda typed env-disposal function
// emitted by codegen beside the trampoline.  When non-null, bz_closure_release
// calls it as envdtor(env) at the last drop BEFORE freeing the env block; this
// is the hook the follow-up capture-retain activation lane will use to release
// +1-retained string / class / box captures.  Foundation: all producers store
// envdtor = null (calloc zero-fill + explicit store); behaviour is identical
// to the prior 24-byte layout.
// `disposed` (off 40, #4475): §28.2 use-after-dispose guard. One-byte flag set
// to 1 by an explicit `c.Dispose()` (codegen emits bz_closure_mark_disposed
// before the release under --checked-disposal). bz_closure_release reads the
// byte at last-drop and, when set, QUARANTINES the cell instead of freeing —
// a deliberate memory leak parallel to bz_array_mark_disposed /
// bz_dict_mark_disposed (the same shape that lets a post-dispose member
// access throw DisposedAccessException under --checked-disposal instead of
// UAF). Outside --checked-disposal the byte is never written, so the legacy
// "free on last drop" path is unchanged.
// A closure owns NEITHER its captures (they are borrows per §28.9 / §28.3) nor
// the boxed cells backing them in the current foundation state; release frees
// only the cell + env block, never a captured value. null is a no-op.
void bz_closure_retain(void* c);
void bz_closure_release(void* c);
// fix #4509 (§27.5 / §28.9, v0.306): CAPTURE-CELL reference counting.
//
// A captured local is promoted to a shared heap CELL (`boxLocalSlot`). The
// declaring frame holds one reference to it (the cell-holder alloca) and every
// closure env that captures the local holds another. When at least one of those
// envs belongs to a lambda the escape analysis can only class as
// CONSERVATIVELY escaping — "merely passed as an argument", which proves
// nothing about when the env dies — no static route can name a single safe free
// site, so today nobody frees the cell at all (the 8 B/capture leak #4509
// measures). These two entry points make that decision at RUNTIME instead: the
// last holder to let go frees the cell, whichever it is.
//
// PROTOCOL, and it is opt-in per cell. `bz_cell_retain` is emitted at an env's
// capture-store and `bz_cell_release` in that env's `envdtor` and once at the
// declaring scope's end, and ONLY for cells codegen has classed as refcounted
// (see `CaptureEntry::retainCellInEnv`). A cell nobody retains never enters the
// table, and `bz_cell_release` on such a cell is a plain `free` — so the two
// protocols coexist and an unclassified cell keeps byte-identical behaviour.
//
// The count is kept in a side table rather than in a header word on the cell
// (the layout #2718 sketches) deliberately: the cell holds a value of arbitrary
// type at offset 0 and is read through by `boxedStorage`, every lambda body,
// and several by-value struct GEPs, so a header is an ABI change across all of
// them, while a side table is none. It costs a lock + hash per capture and per
// drop, which is the simplicity-over-speed trade this project makes until the
// compiler is functionally complete.
//
// The FIRST retain records 2, not 1: the frame's own reference is implicit (it
// exists from `calloc` and is never announced), so the first env to arrive is
// the second holder. null is a no-op for both.
//
// fix #4564 (§27.5 / §28.9, v0.306): the last drop leaves a TOMBSTONE in the
// table rather than erasing the entry, so a release PAST the last drop — an
// over-release, always a defect at the call site — is reported by
// `bz_refcount_overrelease_fail` instead of taking the "never tracked, so the
// frame is the sole holder" arm and free()ing the block a second time (#4547's
// defect, reached here by a different route). The cell itself is still freed on
// the same call as before; a retain re-arms a tombstoned entry (calloc recycles
// a dead cell's address constantly), and a bounded ring evicts old tombstones
// so a correct program's table does not grow. See the block comment above
// `g_cell_rc` in bezel_runtime_refcounting.cpp for the full argument.
void bz_cell_retain(void* cell);
void bz_cell_release(void* cell);
// #4475 (§28.2): closure-as-object use-after-dispose guard. Set/read the
// per-instance disposed flag (offset 40, see layout above). bz_closure_mark_disposed
// is idempotent (subsequent calls no-op). bz_closure_is_disposed returns 0 for
// null (a null closure is never "disposed" — it never held anything).
void bz_closure_mark_disposed(void* c);
int8_t bz_closure_is_disposed(void* c);

// ---------------------------------------------------------------------------
// Boxed value-type reference counting (§8.6 / §28.9, issue #422 boxes
// increment). When a value type (record/enum/primitive/nullable value) is used
// where `Object` (or an interface it implements) is expected, it is BOXED: a
// heap wrapper holds the value. Boxed value types are runtime-managed (§28.9):
// the wrapper is reclaimed automatically. The box cell is laid out
//   { void* vtable (off 0), int64 rc (off 8), value-bytes (off 16) }.
// The rc sits at a FIXED offset 8 (between the vtable word and the payload) so a
// single runtime offset works for EVERY box regardless of the boxed value's
// size; codegen advances past TWO words to reach the payload (boxValue store,
// the `as` unbox load, and the interface-dispatch box thunk). Every producer
// (boxValue) initializes rc to 1 (the producer's reference). retain increments;
// release decrements and, at zero, frees the box cell. A boxed value is a VALUE
// type — it owns no sub-objects (records/enums/primitives are leaves), so
// release frees ONLY the wrapper, like a string. null is a no-op (a null T?
// boxes to a null Object reference per §8.6). Non-atomic for now; becomes atomic
// when async lands.
//
// SAFETY — these helpers are NEVER handed a class instance. Codegen emits a
// release ONLY for a `var` Object/interface local whose declaration initializer
// has a boxable VALUE static type (a local boxValue itself produced); a class
// instance coerced to Object (a REFERENCE initializer) is left as its bare
// pointer and is never release-eligible. So `rc at offset 8` never aliases a
// class instance's bytes ({ vtable, i8 disposed-flag, fields }) — the box-vs-
// class distinction is resolved at the producer site, mirroring strings/closures.
void bz_box_retain(void* b);
void bz_box_release(void* b);

// ---------------------------------------------------------------------------
// Split a string by a delimiter: returns bz_array_t* of bz.string* (heap alloc'd).
// Caller disposes result array. All pointers point into separately malloc'd strings.
void* bz_string_split(const char* s, int64_t s_len,
                       const char* delim, int64_t delim_len);

// per §9.6 Split(separators: Array<char>): owned Array<string>. Splits `s` at
// every position where ONE OF the `nsep` code points in `seps` occurs (each
// separator is a single code point — the §9.6 char-set overload). A run of two
// adjacent separators yields an empty segment between them, matching the
// single-string Split's "no coalescing" behaviour. Returns a bz_array_t* of
// bz.string* (each a fresh +1 producer); the caller (owned result) disposes it.
void* bz_string_split_anychar(const char* s, int64_t s_len,
                              const int32_t* seps, int64_t nsep);

// ---------------------------------------------------------------------------
// §4.6 / #535: Width-aware (code-point-level) string search/slice helpers.
//
// Every function below accepts full bz_string* pointers (layout
// {ptr data, i64 len, i8 width, i64 rc}) and compares code points by value,
// handling width 1/2/4 uniformly. All index/length arguments and return values
// are code-point counts/indices, not byte offsets (§9.6 semantics).
//
// Callers own any newly allocated string results (rc := 1 on return).
int32_t bz_str_startswith(const void* self, const void* prefix);
int32_t bz_str_endswith(const void* self, const void* suffix);
int32_t bz_str_contains(const void* self, const void* sub);
int32_t bz_str_indexof(const void* self, const void* sub);
int32_t bz_str_lastindexof(const void* self, const void* sub);
// fix #4483 (stdlib §9.6, v0.306): Equals(other, ignoreCase: true) — ordinal,
// code-point-wise equality under the simple case fold. Replaces a codegen-
// inlined lowering that (a) passed width-aware code points to <ctype.h>'s
// tolower (undefined above U+00FF, C11 §7.4p1 — the #4420 defect class) and
// (b) truncated each folded code point to ONE BYTE at a hardcoded stride of
// 1, so distinct strings above U+00FF compared equal. The fold is
// bz_char_to_lower, the same shared helper the #4420 char.* family uses, so
// the runtime holds one case policy rather than two. Either pointer may be
// null (null equals only null); widths may differ (§4.6).
int32_t bz_str_equals_ignorecase(const void* self, const void* other);
// fix #4487 (stdlib §9.6 + spec §4.6, v0.306): the ONE ordinal string
// comparison — CompareTo(other), CompareTo(other, ignoreCase), and the
// </<=/>/>= operators, which both documents define as the same relation
// ("ordinal lexicographic comparison of the code-point sequence", §4.6).
// Returns -1/0/+1 (stdlib §2.8 specifies the SIGN only, not the magnitude).
//
// It REPLACES bz_string_compare(a_data, a_len, b_data, b_len, ignore_case),
// which is deleted: that signature carried no storage width, so it walked
// min(a_len, b_len) BYTES over a CODE-POINT length (the #535 defect class)
// and two different non-ASCII strings compared EQUAL. Every former caller
// has the string OBJECT in hand and now passes it here, so the width is no
// longer discarded. The fold is bz_char_to_lower — the same shared helper
// #4420 introduced and #4483 reused — so the runtime holds ONE case policy.
// Either pointer may be null (a null operand compares as EMPTY, exactly as
// bz_string_compare documented); widths may differ (§4.6).
int32_t bz_str_compare_cp(const void* self, const void* other,
                          int32_t ignore_case);
// Substring(start, count): fresh string of `count` code points from `start`.
void*   bz_str_substring(const void* self, int64_t start, int64_t count);
// stdlib §3.3 / #971 item 1: the ReadOnlyStringSpan search + compare family.
// Each operand is an explicit (source string, start, len) code-point triple —
// a span views a SUB-RANGE of its source, so the whole-string helpers above do
// not apply. Comparison is code-point-by-code-point, so the two operands'
// storage widths are irrelevant and may differ (§4.6). `Contains` is not a
// separate entry point: it is `bz_strspan_indexof* >= 0` (§3.3). The returned
// index of the IndexOf forms is relative to the RECEIVER SPAN's start.
int32_t bz_strspan_indexof_cp(const void* self, int64_t s_start, int64_t s_len,
                              int32_t cp);
int32_t bz_strspan_indexof(const void* self, int64_t s_start, int64_t s_len,
                           const void* other, int64_t o_start, int64_t o_len);
int32_t bz_strspan_startswith(const void* self, int64_t s_start, int64_t s_len,
                              const void* other, int64_t o_start, int64_t o_len);
int32_t bz_strspan_endswith(const void* self, int64_t s_start, int64_t s_len,
                            const void* other, int64_t o_start, int64_t o_len);
int32_t bz_strspan_seqequal(const void* self, int64_t s_start, int64_t s_len,
                            const void* other, int64_t o_start, int64_t o_len);
// #1282: char.ToString() / char interpolation — a single-code-point string at
// the narrowest storage width (preserves the full Unicode scalar, no truncation).
void*   bz_char_to_string(int32_t cp);
// fix #4420 (stdlib §9.5, v0.306): the single shared, UB-free lowering for
// every char classification / simple-case-mapping member — see the
// definitions in bezel_runtime_strings.cpp for exactly which categories are
// Unicode-exact across the FULL char domain (IsControl/Cc, IsWhiteSpace/
// White_Space — each a closed, version-stable code-point list) versus
// ASCII-only pending full Unicode category data (everything else). Replaces
// the direct <ctype.h> calls that were undefined above U+00FF. Also used by
// string.IsNullOrWhiteSpace's per-code-point loop (§9.6), so the fix is one
// lowering, not two.
int32_t bz_char_is_digit(int32_t cp);
int32_t bz_char_is_letter(int32_t cp);
int32_t bz_char_is_letter_or_digit(int32_t cp);
int32_t bz_char_is_white_space(int32_t cp);
int32_t bz_char_is_upper(int32_t cp);
int32_t bz_char_is_lower(int32_t cp);
int32_t bz_char_is_control(int32_t cp);
int32_t bz_char_is_punctuation(int32_t cp);
int32_t bz_char_to_upper(int32_t cp);
int32_t bz_char_to_lower(int32_t cp);
// §9.6 / #1295: PadLeft/PadRight — width-aware padding to `totalWidth` code
// points with `padChar` (leftAlign=1 for PadRight, self-then-pad; 0 for PadLeft,
// pad-then-self). Preserves code points > U+00FF (fixed-width storage, §4.6).
void*   bz_str_pad(const void* self, int64_t totalWidth,
                   int32_t padChar, int8_t leftAlign);
// Replace(old, new): fresh string with all occurrences of `old` replaced.
void*   bz_str_replace(const void* self, const void* old_str, const void* new_str);
// Split(sep: char): fresh Array<string> split at every code point equal to sep.
void*   bz_str_split_cp(const void* self, int32_t sep);
// Split(sep: string): fresh Array<string> split at every occurrence of sep.
void*   bz_str_split_str(const void* self, const void* sep);
// §9.6 / issue #654: ToCharArray(): owned Array<char> (elem_size=4, i32 code points).
void*   bz_str_to_char_array(const void* self);

// StringBuilder helper — concatenates an array of bz.string pointers.
// Caller must free *out_data with free() after use.
// ---------------------------------------------------------------------------
void bz_strbuilder_to_string(void* parts_arr,  // bz_array_t* of bz.string ptrs
                              char** out_data,
                              int64_t* out_len);

// ---------------------------------------------------------------------------
// StringBuilder mutators (§3.1). A StringBuilder is a bz_array_t* of bz.string*
// parts; its logical content is the concatenation of those parts. Each mutator
// flattens the current parts to one buffer, applies the operation, and resets
// the parts array to a single fresh +1 string part. The old part pointers are
// left as-is (the builder does not retain/own its parts — matching Append and
// Clear, which never release them; a leak is tolerated, a double-free is not).
// Each is a no-op on out-of-range arguments that would read out of bounds; the
// index/length conventions match the §9.6 String forms.
//   Insert(index, value): insert `value` (as bytes) at code-point `index`.
//   Remove(startIndex, length): delete `length` code points from `startIndex`.
//   Replace(oldValue, newValue): replace every occurrence of `oldValue`.
// `value`/`oldValue`/`newValue` are bz.string* (may be a literal/global).
void bz_sb_insert(void* parts_arr, int64_t index, const void* value);
void bz_sb_remove(void* parts_arr, int64_t start_index, int64_t length);
void bz_sb_replace(void* parts_arr, const void* old_value, const void* new_value);
// §3.1 AppendFormat(format, args): substitute {N} placeholders in `format`
// (a bz.string*) with args[N]'s string form (virtual ToString, vtable slot 3)
// and append the result as one new part, like Append. `args` is a bz_array_t*
// of object pointers (Array<Object>); args beyond Count or a null arg → "".
// Alignment/sub-format specifiers after the index are skipped; {{ }} are literal
// braces. fix #1179 (was a silent no-op: no codegen/runtime case existed).
void bz_sb_append_format(void* parts_arr, const void* format_ptr,
                         const void* args_arr);

// ---------------------------------------------------------------------------
// per §32.5/§32.6: re-encode a bz_string (layout {ptr data, i64 len, i8 width})
// into a freshly malloc'd, null-terminated UTF-8 byte buffer for the CString
// FFI boundary. The internal storage holds raw code points at width 1/2/4 (a
// width>1 string has interior zero bytes that a C `char*` reader would stop on),
// so the marshalling path must produce true UTF-8 before handing the pointer to
// native code. Returns a malloc'd buffer the caller may free(); never returns
// NULL for a non-NULL input (an empty/allocation-failure case yields "").
char* bz_string_to_utf8z(const void* str);

// stdlib §10.1 / fix #977: CString.FromBytes(bytes: Array<byte>) — copies the
// byte array into a fresh malloc'd null-terminated buffer. Caller frees.
char* bz_cstring_from_bytes(const void* arr);

// stdlib §10.1 / issue #1000: CString.ToBytes(): owned Array<byte> — returns a
// FRESH, independently-owned bz_array_t holding a COPY of the CString's bytes
// (count = strlen, the byte length excluding the null terminator; elem_size 1).
// The buffer does NOT alias the CString, so disposing both the CString and the
// returned array frees each allocation exactly once. Released via bz_array_free.
void* bz_cstring_to_bytes(const char* cstr);

// stdlib §10.2 / fix #977: WString.From(s: string) — encodes the Bezel string
// as a null-terminated UTF-16LE buffer of uint16_t code units. Caller frees.
void* bz_string_to_utf16z(const void* str);

// stdlib §10.2 / fix #983: WString.Length — count of UTF-16 code units in a
// null-terminated uint16_t buffer, excluding the terminating zero unit.
int64_t bz_wstrlen(const void* wstr);

// spec §32.6 / stdlib §10.4 (v0.240) / fix #2734: the FFI READ-BACK direction —
// the inverses of bz_string_to_utf8z / bz_string_to_utf16z. Copy a
// NUL-terminated C buffer into a fresh +1 managed Bezel string; the source
// buffer is only read (never freed, never retained), so "the original is
// unaffected and is still subject to the caller's disposal obligation" (§32.6).
// Return NULL on ill-formed input (the stdlib §3.2 strict fault model — codegen
// raises FormatException) and on a NULL argument. They back
// `string.From(c: CString)` / `string.From(w: WString)` and the raw-pointer
// forms `string.FromCStringPointer` / `string.FromWStringPointer`.
void* bz_string_from_utf8z(const void* p);
void* bz_string_from_utf16z(const void* p);

// stdlib §10.3 / fix #977 / #985: RawBytes.From(buffer: Array<byte>) — copies
// the byte array into a fresh malloc'd buffer and wraps it with its length in a
// { ptr, len } heap struct so RawBytes is a backed pointer-and-length pair.
// Released via bz_rawbytes_free (frees the buffer, then the struct).
void* bz_rawbytes_from(const void* arr);

// stdlib §10.3 / fix #985: RawBytes accessors over the { ptr, len } struct.
// Length — the byte count; Pointer — the buffer address (codegen wraps as
// IntPtr); CopyTo — memcpy `len` bytes into destination at destOffset
// (clamped to the destination's bounds). All null-guarded.
int64_t bz_rawbytes_length(const void* rb);
void*   bz_rawbytes_ptr(const void* rb);
void    bz_rawbytes_copyto(const void* rb, void* dest, int64_t destOffset);

// stdlib §10.3 / fix #985: dispose a RawBytes — free the owned buffer, then the
// struct. Idempotent on null. Distinct from the single free() of CString/WString.
void    bz_rawbytes_free(void* rb);

// ---------------------------------------------------------------------------
// Encoding (stdlib §3.2) — UTF-8 concrete encoding only (Utf16/Utf32/Ascii are
// deferred; their GetBytes/GetString semantics are not pinned down by §3.2).
//
// bz_encoding_utf8_get_bytes(s): `Encoding.Utf8.GetBytes(s): owned Array<byte>`.
// Returns a fresh, owned bz_array_t* of bytes (elem_size 1) holding the UTF-8
// encoding of the string's code points. The caller owns the result.
void* bz_encoding_utf8_get_bytes(const void* str);

// bz_encoding_utf8_get_string(bytes, index, count): `Encoding.Utf8.GetString`.
// Decodes `count` UTF-8 bytes starting at `index` of the byte array into a fresh
// +1 managed bz.string (rc := 1, narrowest §4.6 width). The one-argument overload
// passes index=0, count=Array.Length. The byte array is borrowed (not freed).
void* bz_encoding_utf8_get_string(void* bytes, int64_t index, int64_t count);

// §3.2 v0.165 (#1355): GetByteCount(s: string): int
// Returns the number of bytes needed to encode `s` in the given encoding.
// bz_encoding_ascii_get_byte_count returns -1 when any code point is > U+007F
// (the codegen caller turns this into FormatException, matching the strict error
// model of §3.2 v0.136).
int64_t bz_encoding_utf8_get_byte_count(const void* str);
int64_t bz_encoding_utf16_get_byte_count(const void* str);
int64_t bz_encoding_utf32_get_byte_count(const void* str);
int64_t bz_encoding_ascii_get_byte_count(const void* str);

// §3.2 v0.165 (#1355): GetBytes(s, destination: Span<byte>): int
// Write the encoding of `s` into the pre-allocated byte buffer `dest`.
// The caller must ensure dest points to at least GetByteCount(s) bytes.
void bz_encoding_utf8_write_bytes(const void* str, void* dest);
void bz_encoding_utf16_write_bytes(const void* str, void* dest);
void bz_encoding_utf32_write_bytes(const void* str, void* dest);
void bz_encoding_ascii_write_bytes(const void* str, void* dest);

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Per-interface itable lookup (CG35-itable, §13 / §12 interface dispatch).
// Scans the null-terminated flat map `itmap` stored in vtable[1]:
//   [ td*I1, itab*I1, td*I2, itab*I2, ..., NULL ]
// Returns the itab* whose td* == iface_td, or NULL (unreachable for a typed
// program — every implementing class has the interface in its itmap).
// The itab is a constant array of function pointers in interface-declaration
// order; callers GEP into it by method index. No allocation: returns an
// interior pointer into the static global itmap.
void** bz_itable_lookup(void** itmap, void* iface_td);
void** bz_itable_lookup_inst(void** itmap, void* inst_td, void* iface_td);

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// CancellationToken / CancellationTokenSource (stdlib §6.1 / language spec §30.8)
//
// CancellationTokenSource is a disposable heap object that owns a cancellation
// state cell.  CancellationToken is a thin handle (a borrowed pointer to the
// same state cell); it carries NO ownership and lives only as long as its source
// is alive (§30.8 / §9.7).
//
// The state cell is a plain int32_t flag (0 = not cancelled, 1 = cancelled).
// Access is non-atomic for now (single-threaded JIT tests); the spec (§30.8)
// requires atomicity for cross-thread use — this matches the non-atomic
// stance the rest of the runtime takes (bz_string_retain, bz_closure_retain,
// etc.) until the async milestone lands.
//
// Token value = pointer to the state cell (bz_cancel_state_t*).
// CancellationToken.None = null pointer (source never cancels).
// ---------------------------------------------------------------------------

// Allocate a new CancellationTokenSource.  Returns an opaque pointer to the
// internal bz_cancel_source_t struct (the source owns a state cell).
// The Token property is the inner state cell pointer (bz_ctoken_t*).
void* bz_cts_alloc(void);

// Retrieve the CancellationToken handle (borrow of the source's state cell).
// The handle is valid only while the source is alive — per §30.8.
void* bz_cts_token(void* src);

// Read the IsCancellationRequested flag on the source (convenience mirror of
// bz_ctoken_is_cancelled, per stdlib §6.1: the two always agree).
int32_t bz_cts_is_cancelled(void* src);

// Cancel the source: set the flag and signal any registered tokens.
// Idempotent — a second call is a no-op (flag already 1).
void bz_cts_cancel(void* src);

// Dispose the CancellationTokenSource: marks the source cancelled (so any
// surviving token observers see IsCancellationRequested == true, per §6.1
// destructor note), then frees the source struct.  The state cell itself is
// NOT freed here — outstanding token handles borrow it and outlive this call
// by design (the spec requires the source to outlive its tokens, but defensive
// code may read the token after the source is disposed; we keep the cell alive
// via a separate refcount so reads are safe).  In practice, for the
// synchronous single-threaded tests, callers always keep the source alive as
// long as they need the token.
void bz_cts_dispose(void* src);

// v0.173 / #810 (stdlib §6.1 / spec §30.8): linked cancellation sources.
// CreateLinked(a, b) returns a new CancellationTokenSource whose token cancels
// when EITHER parent token (a or b) cancels, or via the child's own
// Cancel/CancelAfter — whichever happens first (cancellation is idempotent).  A
// parent already cancelled at creation cancels the child SYNCHRONOUSLY before the
// call returns (the same policy as registering on an already-cancelled token).
// CancellationToken.None parents (null handles) are ignored.  CreateLinkedAll
// does the same over an Array<CancellationToken> (a bz_array_t* of ptr elements,
// each a CancellationToken handle; null elements are ignored).  Both return the
// child source (owned by the caller; disposed via bz_cts_dispose, which
// unregisters from every parent BEFORE signalling — no parent cascade fires into
// a disposed child).  Parents are token handles (== the parent source's state
// cell), so linking onto a token links onto that source's cell.  Intra-domain
// only (§30.8): no cross-domain machinery.
void* bz_cts_create_linked(void* tokA, void* tokB);
void* bz_cts_create_linked_all(void* parents);

// #716: token-level retain / release.  A token handle (void* == bz_cancel_state_t*)
// can be retained by async slow-paths (bz_task_delay_cancelable, mutex/semaphore
// acquire_async) to extend the state cell's lifetime across the pool-worker
// boundary.  These two helpers wrap the atomic rc operations so callers outside
// the runtime .cpp (including runtime unit tests) can drive the same paths.
// token == null → no-op.
void bz_cts_retain(void* token);
void bz_cts_release(void* token);

// Read IsCancellationRequested on a token handle.
// token == null  → false  (CancellationToken.None never cancels, §6.1).
int32_t bz_ctoken_is_cancelled(void* token);

// ThrowIfCancellationRequested (stdlib §6.1): if the token is cancelled, raise
// OperationCanceledException by storing it in the global exception slot and
// branching to the enclosing handler.  In the C runtime we mark cancellation by
// setting the global exception pointer to a sentinel non-null value and returning
// 1 so codegen can branch.  If not cancelled, returns 0.
// The codegen emits:
//   if (bz_ctoken_throw_if_cancelled(token)) goto unwind;
int32_t bz_ctoken_throw_if_cancelled(void* token);

// stdlib §6.7 (line 2868) / spec §30.8: Task.Delay with cancellation.
// Token-aware Task.Delay: checks the token before and after the sleep.
// If cancelled (before or after sleep), completes the task as faulted with
// an OperationCanceledException constructed from oce_vtable. Otherwise,
// completes normally. oce_vtable is the vtable pointer for
// OperationCanceledException, supplied by codegen.
void* bz_task_delay_cancelable(int64_t milliseconds, void* token, void* oce_vtable);

// ---------------------------------------------------------------------------
// per §33.3 / §22.13 (line 3845): abnormal-termination handler for an exception
// that escapes Main with no enclosing handler. §33.3 reserves exit code 0 for
// NORMAL completion (and for a legitimate `return 0`); §22.13 distinguishes
// "Main returning normally" from "Main returning by an uncaught exception that
// the runtime intercepts". This helper is the runtime's interception point: it
// writes a short diagnostic to stderr and terminates the process with a non-zero
// exit code so the abnormal state is observable to the OS / shell / CI. The exit
// code (70 = EX_SOFTWARE, "internal software error", BSD sysexits.h) is an
// implementation choice — the spec pins only "not the normal-completion code".
// Declared [[noreturn]]: it never returns to the caller.
//
// v0.163 / #1366: takes the in-flight exception object (and the null-terminated
// table of built-in exception vtables) so §33.3's report (type, message, stack
// trace, CauseChain) can be printed before exiting. The table gates the §31.4
// field reads to objects with that layout.
#ifdef __cplusplus
[[noreturn]]
#else
_Noreturn
#endif
void bz_unhandled_exception(void* exc, void** excVtableTable);

// fix #492 / §31.1 / §2.6: the per-thread in-flight exception pointer.
// Made thread_local by fix #704 (see below).
// fix #704: bz_exc is now thread_local so each domain worker thread has its
// own slot.  Direct extern references to a thread_local symbol are not
// compatible with LLVM's ORC JIT (the JIT generates emulated-TLS code that
// looks for "__emutls_v.bz_exc", a different symbol from the real TLS "bz_exc"
// the host process exports).  Codegen therefore accesses the slot through the
// accessor pair bz_exc_addr() / bz_exc_set() which are plain functions that the
// JIT resolves correctly via DynamicLibrarySearchGenerator.
//
// Usage from the runtime: read/write bz_exc directly (same TU as definition).
// Usage from codegen (JIT + AOT): call bz_exc_addr() to get &bz_exc for the
//   current thread, then load/store through the returned pointer.
extern thread_local void* bz_exc;

// bz_exc_addr() — return a pointer to the calling thread's bz_exc TLS slot.
// Codegen emits `call ptr @bz_exc_addr()` then load/stores through the result.
// The function is in the same TU as bz_exc, so it accesses the real TLS slot;
// the JIT resolves the symbol via the host process's dynamic symbol table.
extern void** bz_exc_addr();

// fix #713: bz_current_domain_ptr() — return the current g_current_domain value
// for the calling thread.  Used by tests to verify that domain_drain sets the
// tracker correctly; not called from generated code.
extern void* bz_current_domain_ptr();

// ---------------------------------------------------------------------------
// Shared<T> / Weak<T> — domain-local shared ownership (§28.13 / stdlib §2.16)
//
// A bz_shared_t control block owns one underlying value and tracks strong +
// weak reference counts (unsynchronised — domain-local per spec §28.13).
// Shared handles (bz_shared_t*) and Weak handles (bz_weak_t*, which is the
// same struct) are opaque to Bezel code; codegen stores them as plain ptr.
// ---------------------------------------------------------------------------

// Allocate a control block wrapping `value`.  The block's strong count is 1
// and weak count is 0.  `dispose_value` is the void(void*) thunk that runs
// the value's vtable Dispose and frees the block — the same thunk used by
// elementDisposerThunk() for owning collections.  Returns the new strong
// handle (bz_shared_t*).
void* bz_shared_new(void* value, void (*dispose_value)(void*));

// Increment the strong count and return a new handle pointing to the same
// control block.  The caller becomes an additional owner. per §28.13 Clone().
void* bz_shared_clone(void* handle);

// Return the underlying value pointer (a borrow; the handle's lifetime gates
// the value's lifetime). per §28.13 Get().  Does NOT transfer ownership.
void* bz_shared_get(void* handle);

// Mint a Weak<T> handle (increments the weak count; does not affect the strong
// count). per §28.13 Downgrade().  Returns a bz_weak_t*.
void* bz_shared_downgrade(void* handle);

// TryUpgrade: if strong count > 0, increment it and write a new strong handle
// to *out_handle, then return 1 (true).  If strong count is 0 (value already
// disposed), write null to *out_handle and return 0 (false). per §28.13.
int   bz_weak_try_upgrade(void* weak_handle, void** out_handle);

// Decrement the strong count.  When it reaches 0: run dispose_value on the
// value (disposing via vtable + freeing the block if the weak count is also
// 0).  Idempotent via the disposed-flag convention. per §28.13 Dispose().
void  bz_shared_release(void* handle);

// Decrement the weak count.  When the strong count is already 0 and the weak
// count also reaches 0, free the control block. per §28.13 Weak.Dispose().
void  bz_weak_release(void* weak_handle);

// §28.2 v0.135 / issue #627: checked-disposal-mode quarantine for Shared<T>.
// Called instead of bz_shared_release for the explicit handle.Dispose() in
// checked builds. Marks handle_disposed=1 and disposes the value, but does NOT
// free the control block (deliberate quarantine). bz_shared_release becomes a
// no-op when handle_disposed is set, so the scope-end auto-dispose is safe.
void bz_shared_mark_disposed(void* handle);

// §28.2 v0.135 / issue #627: return non-zero if the Shared<T> handle has been
// quarantined via bz_shared_mark_disposed (checked-disposal builds only).
int8_t bz_shared_is_disposed(void* handle);

// ---------------------------------------------------------------------------
// Channel<T> and Domain.Spawn runtime (§30.14/§30.15, #600 phase c)
//
// Channel<T> is backed by a bz_channel_t (opaque to Bezel code, stored as ptr).
// ChannelSender<T> is backed by a bz_channel_sender_t (also opaque, ptr).
//
// Element encoding: class items → void* (ownership transferred); value/string
// items → void* (uint64 bit-cast inline; no deep disposal via dispose_elem).
// dispose_elem is null for value/string channels; non-null for class channels
// (vtable Dispose + free, same as OwningArray<T> element policy).
// ---------------------------------------------------------------------------

// Allocate a new empty Channel<T>. `dispose_elem` is the per-element
// disposer (null for value/string elements; non-null for class elements).
// v0.174 (#809): `capacity` is 0 for an unbounded channel (the nullary ctor)
// or >= 1 for a bounded one; codegen guards capacity >= 1 (ArgumentException)
// before the call.
void* bz_channel_new(void (*dispose_elem)(void*), int64_t capacity);

// Mint a new ChannelSender<T> handle. Increments the channel's sender count.
// Caller owns the returned sender (must release via bz_channel_sender_release).
void* bz_channel_new_sender(void* ch);

// Send one item (via the channel pointer directly). Item ownership transfers.
// Per fix #634 / §30.15: if the channel is already closed or disposed, disposes
// the item and stores a ChannelClosedException in bz_exc (cce_vtable supplies
// the vtable; may be null for best-effort). Codegen emits emitPendingCheck()
// after every Call to this function.
// v0.174 (#809): on a FULL bounded channel the sync Send never blocks — it
// disposes the item and stores a ChannelFullException in bz_exc (cfe_vtable).
void bz_channel_send(void* ch, void* item, void* cce_vtable, void* cfe_vtable);

// Send one item via a ChannelSender handle.
// Per fix #634: also signals ChannelClosedException via bz_exc when closed.
// v0.174 (#809): cfe_vtable supplies the ChannelFullException vtable for the
// full-bounded case.
void bz_channel_sender_send(void* tx, void* item, void* cce_vtable,
                            void* cfe_vtable);

// v0.174 (#809): SendAsync — returns a bz_task_t* that completes when the item
// is accepted (immediately when space is free or the channel is unbounded;
// after a Receive frees a slot on a full bounded channel). If the channel
// closes while the send waits, the pending item is disposed and the task faults
// with ChannelClosedException (cce_vtable supplies the vtable).
void* bz_channel_send_async(void* ch, void* item, void* cce_vtable);
void* bz_channel_sender_send_async(void* tx, void* item, void* cce_vtable);

// Receive one item asynchronously. Returns a bz_task_t* (already-done if
// an item is buffered; pending if empty+open; faulted with ChannelClosedException
// if empty+closed). cce_vtable is the ChannelClosedException vtable global.
void* bz_channel_receive(void* ch, void* cce_vtable);

// Non-blocking receive: stores item in *out_item and returns 1 if available;
// returns 0 if empty. Thread-safe.
int32_t bz_channel_try_receive(void* ch, void** out_item);

// Number of buffered (not-yet-received) items.
int64_t bz_channel_count(void* ch);

// True (non-zero) if the channel is closed (all senders released).
int32_t bz_channel_is_closed(void* ch);

// Release one ChannelSender handle. If this was the last sender, closes the
// channel and faults any pending receivers with ChannelClosedException.
// cce_vtable is the ChannelClosedException vtable global.
void bz_channel_sender_release(void* tx, void* cce_vtable);

// Dispose the Channel<T>: close it, fault pending receivers, dispose any
// undelivered items in REVERSE insertion order (§28.11), and free the struct.
void bz_channel_dispose(void* ch, void* cce_vtable);

// §30.15 / stdlib §6.9 v0.186 (#1724): Channel<T>.ReceiveAll() — build a
// fresh drain-enumerable frame over the channel and return it as
// IAsyncEnumerable<T>* (is_owning=0, value/string T) or
// IOwningAsyncEnumerable<T>* (is_owning=1, class T). The returned pointer
// is the enumerable's vtable-self-address; vtable slot 4 (GetAsyncEnumerator)
// returns the same pointer (the enumerable and enumerator are one object).
//
// Returns nullptr + stores an InvalidOperationException in bz_exc when the
// single-consumer rule is violated: another ReceiveAll is already active OR
// a Receive() task is parked on the channel. ioe_vtable is the
// InvalidOperationException vtable (best-effort — pass null to skip the
// exception object construction).
void* bz_channel_recv_all_init(void* ch, void* ioe_vtable, int32_t is_owning);

// Domain.Spawn: spawn a fresh domain that runs entry_fn(arg_ptr).
// Returns an OwningTask bz_task_t* that completes (with the result pointer)
// when the spawned domain finishes, or faults with DomainFaultedException.
// dfe_vtable is the DomainFaultedException vtable global.
void* bz_domain_spawn(void* (*entry_fn)(void*), void* arg_ptr,
                      void* dfe_vtable);
// §2.15 (issue #550): BinarySerializer / BinaryDeserializer — compact binary
// format for ISerializer / IDeserializer.
//
// Wire format (little-endian, not a stable interchange format per §2.15):
//   - Integers: variable-length zigzag encoding (signed) or varint (unsigned).
//   - float:  IEEE 754 single stored as 4 bytes (little-endian i32 bits).
//   - double: IEEE 754 double stored as 8 bytes (little-endian i64 bits).
//   - decimal: 16 bytes little-endian (unused in tier 1, reserved).
//   - bool: 1 byte (0 or 1).
//   - char: varint of the Unicode scalar value.
//   - string: varint byte-length prefix, then UTF-8 bytes (no NUL).
//   - null: marker byte 0x00 written before the value.
//   - BeginObject(n): varint field count.
//   - WriteFieldName(s): string (as above).
//   - EndObject: no bytes (count was written at BeginObject).
//   - BeginArray(n): varint element count.
//   - EndArray: no bytes.
//
// All bz_binser_* / bz_binder_* functions are extern "C" and safe to call
// from JIT-compiled Bezel code via DynamicLibrarySearchGenerator.

// --- Serializer (writer) ---
// Allocate a new BinarySerializer state. Returns an opaque handle.
void* bz_binser_alloc(void);
// Free the serializer (without transferring the buffer).
void  bz_binser_free(void* ser);
// Transfer the accumulated bytes as an owned OwningBuffer<byte>. The caller
// owns the result. The serializer continues to be valid after this call
// (it retains the buffer for further writes — the semantics here: a fresh
// OwningBuffer copy is returned, the internal buffer is cleared).
void* bz_binser_to_bytes(void* ser);

// ISerializer interface methods on the BinarySerializer.
void bz_binser_write_int(void* ser, int64_t value);
void bz_binser_write_uint(void* ser, uint64_t value);
void bz_binser_write_float(void* ser, double value);   // WriteFloat(double)
void bz_binser_write_decimal(void* ser, void* val);    // WriteDecimal (stub)
void bz_binser_write_bool(void* ser, int8_t value);
void bz_binser_write_string(void* ser, void* str);     // bz_string*
void bz_binser_write_char(void* ser, int32_t value);   // Unicode scalar
void bz_binser_write_null(void* ser);
void bz_binser_begin_object(void* ser, int32_t field_count);
void bz_binser_write_field_name(void* ser, void* name); // bz_string*
void bz_binser_end_object(void* ser);
void bz_binser_begin_array(void* ser, int32_t element_count);
void bz_binser_end_array(void* ser);

// §9.8 tier-3: identity-tag graph serialization helpers.
// Wire markers: BZ_SER_INLINE (0x01) = first owning reach; BZ_SER_REF (0x02) = back-edge.
// bz_binser_graph_get_tag    — return existing tag_id for ptr, or 0 if not yet seen.
// bz_binser_graph_assign_tag — assign next tag_id to ptr and return it.
// bz_binser_write_inline_tag — write BZ_SER_INLINE + LE32 tag_id before inline object.
// bz_binser_write_ref_tag    — write BZ_SER_REF + LE32 tag_id for a back-edge.
int32_t bz_binser_graph_get_tag(void* ser, void* ptr);
int32_t bz_binser_graph_assign_tag(void* ser, void* ptr);
void    bz_binser_write_inline_tag(void* ser, int32_t tag_id);
void    bz_binser_write_ref_tag(void* ser, int32_t tag_id);

// --- §2.15 JsonSerializer (kind=1) helpers ---
// Allocate a new JsonSerializer (same bz_binser_t, kind=1, writes JSON text).
void* bz_jser_alloc(void);
// Allocate a JsonSerializer with options (fix #687 / stdlib §2.15 JsonOptions).
// indented=1 → pretty-print with newlines; indent_width = spaces per level.
// max_depth = nesting guard (raises SerializationException when exceeded).
void* bz_jser_alloc_opts(int8_t indented, int32_t indent_width, int32_t max_depth);
// Transfer the accumulated JSON bytes as an owned OwningBuffer<byte>.
void* bz_jser_to_bytes(void* ser);
// Transfer the accumulated JSON bytes as a Bezel string (UTF-8 → bz_string*).
// Returns a newly-allocated bz_string_struct* (caller owns, rc=1).
void* bz_jser_to_string(void* ser);

// --- §2.15 JsonDeserializer (kind=1/2) helpers ---
// Allocate a JsonDeserializer reading from a UTF-8 JSON byte span (kind=1, borrows data).
void* bz_jder_alloc(const uint8_t* data, int64_t len, int8_t ignore_unknown);
// Allocate a JsonDeserializer with full options (fix #687 / stdlib §2.15 JsonOptions).
void* bz_jder_alloc_opts(const uint8_t* data, int64_t len,
                          int8_t ignore_unknown, int8_t allow_trailing_commas,
                          int32_t max_depth);
// Allocate a JsonDeserializer from a malloc'd UTF-8 buffer (kind=2, takes ownership of utf8z).
// `utf8z` is a malloc'd byte buffer of `len` bytes; bz_binder_free will free it.
void* bz_jder_alloc_str(char* utf8z, int64_t len, int8_t ignore_unknown);
// Allocate a JsonDeserializer from a malloc'd UTF-8 buffer with full options.
void* bz_jder_alloc_str_opts(char* utf8z, int64_t len,
                              int8_t ignore_unknown, int8_t allow_trailing_commas,
                              int32_t max_depth);
// (bz_binder_free handles disposal for both binary and JSON — same struct)

// --- Deserializer (reader) ---
// Allocate a BinaryDeserializer reading from a byte span.
// `data` is borrowed for the lifetime of the deserializer; `len` is byte count.
// `ignore_unknown` mirrors the IDeserializer strict/lenient policy.
void* bz_binder_alloc(const uint8_t* data, int64_t len, int8_t ignore_unknown);
// Free the deserializer.
void  bz_binder_free(void* der);

// IDeserializer interface methods on the BinaryDeserializer.
int64_t bz_binder_read_int(void* der);
uint64_t bz_binder_read_uint(void* der);
double  bz_binder_read_float(void* der);    // ReadFloat() → double
void*   bz_binder_read_decimal(void* der);  // stub
int8_t  bz_binder_read_bool(void* der);
void*   bz_binder_read_string(void* der);   // returns bz_string* (rc=1)
int32_t bz_binder_read_char(void* der);     // Unicode scalar
int8_t  bz_binder_try_read_null(void* der);
int32_t bz_binder_begin_object(void* der);  // returns field count
void*   bz_binder_read_field_name(void* der); // returns bz_string* (rc=1)
void    bz_binder_skip_value(void* der);
void    bz_binder_end_object(void* der);
int32_t bz_binder_begin_array(void* der);   // returns element count
int8_t  bz_binder_has_more_elements(void* der);
void    bz_binder_end_array(void* der);

// §9.8 tier-3: identity-tag graph deserialization helpers.
// bz_binder_read_graph_marker   — read 1-byte marker + LE32 tag_id; returns BZ_SER_INLINE/REF/0.
// bz_binder_graph_mark_in_progress — mark tag as being constructed (cycle sentinel).
// bz_binder_graph_register_tag  — store constructed ptr for tag_id; clears in-progress.
// bz_binder_graph_resolve_tag   — resolve tag_id to ptr (NULL if in-progress = cycle).
int32_t bz_binder_read_graph_marker(void* der, int32_t* out_tag);
void    bz_binder_graph_mark_in_progress(void* der, int32_t tag_id);
void    bz_binder_graph_register_tag(void* der, int32_t tag_id, void* ptr);
void*   bz_binder_graph_resolve_tag(void* der, int32_t tag_id);

// §9.8 tier-4: polymorphic-dispatch deserialization entry point.
// Reads the opening object and the "$type" discriminator field; sets up
// poly_preread_count so the concrete type's bz_binder_begin_object returns
// the remaining field count without re-consuming the object header.
// Returns a newly-allocated bz_string* (rc=1) holding the discriminator value.
// Raises SerializationException if the first field is not "$type" or input is truncated.
void* bz_binder_read_poly_discriminator(void* der);

// §9.8 choice serialization (v0.200): choice-dispatch deserialization entry point.
// Reads the opening object and the leading "$case" discriminator field; sets up
// poly_preread_count so the following bz_binder_begin_object returns the remaining
// payload field count without re-consuming the object header.  Returns a
// newly-allocated bz_string* (rc=1) holding the case discriminator value.
// Raises SerializationException if the object does not lead with "$case", is
// empty, or the input is truncated.
void* bz_binder_read_case_discriminator(void* der);

// SerializationException throw helper — called by synthesized code on bad input.
// `msg` is a NUL-terminated C string describing the fault (for diagnostics).
// Stores a built-in SerializationException in the global exception slot (bz_exc)
// and returns (codegen emits the unwind branch after the call).
void bz_ser_throw(const char* msg, void* ser_exn_vtable);

// The SerializationException vtable pointer — registered by the codegen-generated
// main prologue before any user code runs (bz_ser_exc_vtable_init stores it).
// bz_ser_throw uses this so runtime-internal throws create a well-formed exception
// object with a valid vtable pointer (required by emitIsA catch dispatch).
extern void* bz_ser_exc_vtable;

// Register the SerializationException vtable; called by the generated main prologue.
void bz_ser_exc_vtable_init(void* vtable);

// issue 2330 / §31.4 / §30.9: register the built-in exception vtable and type-
// descriptor tables with the runtime, from the same generated main prologue and
// for the same reason as bz_ser_exc_vtable_init above. The pure-runtime
// vtable-Dispose-then-free reclaim sites (bz_dispose_class_obj and
// bz_domain_spawn's DomainFaultedException wrap, bezel_runtime_concurrency.cpp)
// run with no codegen-supplied table argument; they reclaim a disposed
// exception's §31.4 CauseChain snapshot through
// bz_exc_reclaim_causechain_registered, which reads what was registered here.
void bz_exc_tables_init(void** excVtableTable, void** excDescTable);

// Reclaim `obj`'s §31.4 CauseChain snapshot using the tables registered by
// bz_exc_tables_init. A no-op unless `obj` is a built-in exception or a user
// subclass of one (the same gate the codegen reclaim site applies), so it is
// safe to call on any class object about to be freed. Call it AFTER the virtual
// Dispose dispatch and BEFORE freeing the object, matching the codegen terminal
// reclaim site's order.
void bz_exc_reclaim_causechain_registered(void* obj);

// OwningBuffer<byte> helpers used by BinarySerializer/BinaryDeserializer.
// bz_owned_buffer_alloc: allocate an OwningBuffer<byte> with the given bytes,
// returning the opaque handle (an owned bz_array_t* wrapper). Caller owns it.
void* bz_owned_buffer_byte_alloc(const uint8_t* data, int64_t len);
// bz_owned_buffer_as_rospan: return a ReadOnlySpan<byte> struct {ptr, len} as
// a pair of {i64 ptr, i64 len} words (matches the codegen layout for spans).
// Actually returns a bz_array_t* with read semantics (span borrows from it).
// The span borrows the buffer; the buffer must outlive the deserializer.
void bz_owned_buffer_get_span(void* buf, const uint8_t** data_out, int64_t* len_out);

// ---------------------------------------------------------------------------
// Mutex and Lock runtime (stdlib §6.2, issue #647)
//
// Mutex: non-recursive POSIX mutex with refcounted core (bz_mutex_t*).
// Lock:  owned guard (bz_lock_t*) returned by acquire/try_acquire.
//
// Recursiveness (recorded decision — §6.2 is silent):
//   NON-RECURSIVE (PTHREAD_MUTEX_ERRORCHECK). TryAcquire-while-held in the
//   same thread times out (returns null) rather than deadlocking.
//
// All extern "C" (no mangling — the JIT-mangling lesson from #632).
// ---------------------------------------------------------------------------

// Allocate a new Mutex. Returns bz_mutex_t* (core_refs = 1 for the Mutex object).
void* bz_mutex_new(void);

// Block until the mutex is available; return an owned bz_lock_t*. Per §6.2
// Acquire(): owned Lock. The Lock acquires a core ref so it can outlive the Mutex.
void* bz_mutex_acquire(void* mutex);

// Attempt to acquire with a millisecond timeout. Returns an owned bz_lock_t*
// on success, or null on timeout / already-held (non-recursive: EDEADLK → null).
// Per §6.2 TryAcquire(timeout: TimeSpan): owned Lock? . timeout_ms <= 0 → trylock.
void* bz_mutex_try_acquire(void* mutex, int64_t timeout_ms);

// Release the held lock (idempotent per §28.2). Called by Lock's disposer at
// scope exit or explicit Dispose(). Frees the bz_lock_t and drops its core ref.
void  bz_lock_release(void* lock);

// Dispose the Mutex object: marks disposed (idempotent), drops the Mutex's own
// core ref. The pthread_mutex is destroyed only when the last Lock is also gone.
void  bz_mutex_dispose(void* mutex);

// ---------------------------------------------------------------------------
// §6.2 async variants (issue #647, v0.139): AcquireAsync / TryAcquireAsync.
//
// Both return an OwningTask<Lock> / OwningTask<Lock?> (bz_task_t* carrying a
// bz_lock_t* result stored as a void*-sized payload).
//
// bz_mutex_acquire_async(mutex, token, oce_vtable)
//   token     — CancellationToken (bz_cancel_state_t*); null = .None.
//   oce_vtable — vtable* for OperationCanceledException (for token cancel faults).
//   Returns a bz_task_t* whose result, once done, is a bz_lock_t* (void*).
//
// bz_mutex_try_acquire_async(mutex, timeout_ms, token, oce_vtable)
//   timeout_ms — milliseconds; <= 0 → trylock (immediate fail if held).
//   Returns a bz_task_t* whose result is a bz_lock_t* (non-null) or null
//   (stored as a null void*) on timeout.
//
// Design: uncontended fast-path via pthread_mutex_trylock → already-done task;
// contended path submits a pool worker that blocks on the mutex (or times out),
// then calls bz_task_complete with the lock pointer.  Cancellation is pre-checked
// before submission; post-submit cancellation for the blocking worker faults the
// task on wakeup (checked after blocking returns) -- the pthread blocking call
// itself is not interrupted (conservative: no POSIX interruptible mutex wait),
// but a cancel during the wait is detected immediately on return.
// ---------------------------------------------------------------------------
void* bz_mutex_acquire_async(void* mutex, void* token, void* oce_vtable);
void* bz_mutex_try_acquire_async(void* mutex, int64_t timeout_ms,
                                  void* token, void* oce_vtable);

// ---------------------------------------------------------------------------
// Semaphore and SemaphoreToken runtime (stdlib §6.3, issue #647)
//
// Semaphore: counting semaphore with refcounted core (bz_semaphore_t*).
// SemaphoreToken: owned guard (bz_semaphore_token_t*) returned by acquire.
//
// bz_semaphore_new(initial_count, max_count) — allocate a new Semaphore.
//   initial_count: initial permit count; max_count: advisory upper bound.
// bz_semaphore_acquire(sem) — block until a permit is available; return
//   an owned bz_semaphore_token_t*.  Per §6.3 Acquire(): owned SemaphoreToken.
//   The token acquires a core ref so it can outlive the Semaphore object.
// bz_semaphore_try_acquire(sem, timeout_ms) — attempt with a millisecond
//   timeout.  Returns an owned bz_semaphore_token_t* on success, or null on
//   timeout / no permits available.
// bz_semaphore_token_release(tok) — release the held permit (idempotent per
//   §28.2).  Called by SemaphoreToken's disposer (scope exit / Dispose()).
//   Frees the bz_semaphore_token_t and drops its core ref.
// bz_semaphore_dispose(sem) — dispose the Semaphore object (idempotent),
//   drops the Semaphore's own core ref.  sem_t destroyed only when all tokens
//   are also released (via semaphore_core_release → core_refs → 0).
// ---------------------------------------------------------------------------
void* bz_semaphore_new(int initial_count, int max_count);
void* bz_semaphore_acquire(void* sem);
void* bz_semaphore_try_acquire(void* sem, int64_t timeout_ms);
void  bz_semaphore_token_release(void* token);
void  bz_semaphore_dispose(void* sem);

// ---------------------------------------------------------------------------
// §6.3 async variants (issue #647, v0.140): AcquireAsync / TryAcquireAsync.
//
// Both return an OwningTask<SemaphoreToken> / OwningTask<SemaphoreToken?>.
//
// bz_semaphore_acquire_async(sem, cancel_token, oce_vtable)
//   Returns bz_task_t* whose result, once done, is a bz_semaphore_token_t*.
//
// bz_semaphore_try_acquire_async(sem, timeout_ms, cancel_token, oce_vtable)
//   Returns bz_task_t* whose result is bz_semaphore_token_t* (success)
//   or null (timeout).
//
// Design mirrors bz_mutex_acquire_async (§6.2 / v0.139): uncontended fast
// path via sem_trywait; contended path submits a pool worker that blocks on
// sem_wait / sem_timedwait; cancellation pre-checked before pool submission.
// ---------------------------------------------------------------------------
void* bz_semaphore_acquire_async(void* sem, void* token, void* oce_vtable);
void* bz_semaphore_try_acquire_async(void* sem, int64_t timeout_ms,
                                      void* token, void* oce_vtable);

// ---------------------------------------------------------------------------
// Atomic<T> and AtomicInteger<T> runtime (stdlib §6.4, issue #647)
//
// All operations are sequentially consistent (__ATOMIC_SEQ_CST) per §6.4 v0.140.
// The cell is a heap object (bz_atomic_t*) with a refcounted core and a
// payload slot whose width is determined by the T restriction:
//   Atomic<T>:        T in {bool, char, byte, sbyte, short, ushort, int, uint,
//                           long, ulong, IntPtr, UIntPtr}  (cell width 1–8 bytes).
//   AtomicInteger<T>: T in {byte, sbyte, short, ushort, int, uint, long, ulong}.
//
// cell_size: width of the atomic cell in bytes (1, 2, 4, or 8).
//   Used for allocation and for routing __atomic_* builtins in the runtime.
//
// bz_atomic_new(cell_size, initial) — allocate. initial is zero-extended into
//   the cell; passed as uint64_t regardless of T width for ABI uniformity.
// bz_atomic_dispose(ptr)            — idempotent dispose; drops the core ref.
// bz_atomic_load(ptr, cell_size)    — seq-cst load; returned as uint64_t.
// bz_atomic_store(ptr, val, cell_size) — seq-cst store; val as uint64_t.
// bz_atomic_exchange(ptr, val, cell_size) — atomic swap; returns old value.
// bz_atomic_compare_exchange(ptr, expected, replacement, cell_size)
//   — strong CAS; returns the observed value (== expected on success,
//     != expected on failure).
// bz_atomic_add(ptr, delta, signed_, cell_size, ove_vtable)
//   — seq-cst fetch_add; returns POST-add value (§6.4).  On arithmetic
//     overflow raises OverflowException via ove_vtable (§6.1 rule).
//     signed_: 1 if T is signed (sbyte/short/int/long), 0 if unsigned.
//
// AtomicInteger uses the same bz_atomic_t layout; bz_atomic_add covers
// Add/Increment/Decrement (delta=1/-1 respectively).
// ---------------------------------------------------------------------------

// Allocate a new Atomic<T> / AtomicInteger<T> cell. cell_size in {1,2,4,8}.
// initial is passed as uint64_t; only the low cell_size bytes are stored.
void* bz_atomic_new(int cell_size, uint64_t initial);

// Idempotent dispose. Drops the core ref; last drop frees the object.
void  bz_atomic_dispose(void* atom);

// Sequentially-consistent load. Returns the value zero-extended to uint64_t.
uint64_t bz_atomic_load(void* atom, int cell_size);

// Sequentially-consistent store. val is zero-/sign-truncated to cell_size bytes.
void     bz_atomic_store(void* atom, uint64_t val, int cell_size);

// Atomic exchange: stores val, returns old value (both as uint64_t).
uint64_t bz_atomic_exchange(void* atom, uint64_t val, int cell_size);

// Strong compare-and-swap.  Returns the observed value before the swap
// (== expected on success; != expected on failure).
uint64_t bz_atomic_compare_exchange(void* atom, uint64_t expected,
                                    uint64_t replacement, int cell_size);

// Atomic fetch-add with overflow check (§6.1 / §6.4).
// delta: addend (as int64_t — sign matters for signed T, but the raw bits are
//   the same as uint64_t for twos-complement; callers pass the int64_t bits).
// signed_: 1 if T is signed, 0 if unsigned.
// ove_vtable: vtable* for OverflowException; null → no check (should not
//   happen in a well-typed program but guards against stale IR).
// Returns the POST-add value (§6.4: "returns post-operation value").
uint64_t bz_atomic_add(void* atom, int64_t delta, int signed_,
                       int cell_size, void* ove_vtable);

// ---------------------------------------------------------------------------
// Buffer<T> — fixed-capacity compiler-intrinsic primitive (issue #545).
//
// Used as the low-level backing store for self-hosted collections.
// Value-type elements only (this revision): int/byte/short/long/double/
// decimal/char/bool.  Class-element Buffer is the §5.20 design pass (#545).
//
// Layout: 16-byte header {int64 len, int64 elem_size} + inline calloc'd payload.
//   len:       number of elements (fixed at construction).
//   elem_size: size of one element in bytes.
//
// bz_buffer_new(len, elem_size)  — calloc a zero-initialised Buffer<T>.
//   Returns a pointer to the 16-byte header; payload immediately follows.
// bz_buffer_get_ptr(buf, idx)    — return a pointer to element [idx] in the
//   payload.  NO bounds check (caller emits §16 bounds check before calling).
// bz_buffer_free(buf)            — free the buffer (header + inline payload).
//   Idempotent (null-safe); plain free — value elements have no disposal.
// ---------------------------------------------------------------------------

// Allocate a zero-initialised Buffer<T> for `len` elements of `elem_size` bytes.
// Calls calloc so every slot starts as zero-bytes (type zero value).
void* bz_buffer_new(int64_t len, int64_t elem_size);

// Return a pointer to the element at index `idx` inside the buffer payload.
// The caller is responsible for bounds-checking idx before calling this.
void* bz_buffer_get_ptr(void* buf, int64_t idx);

// Free the buffer.  Null-safe (no-op on null).  Plain free — value elements
// carry no ownership, so no per-element disposal is performed.
void  bz_buffer_free(void* buf);

// ---------------------------------------------------------------------------
// §6.6 / issue #974: TaskCompletionSource<T> / OwningTaskCompletionSource<T> /
// TaskCompletionSource (non-generic). A TCS is a heap-allocated struct that
// holds a pre-allocated inner task and an atomic completion flag. The producer
// calls TrySet* to complete the inner task; the consumer awaits the .Task
// property. The TCS OWNS the inner task; bz_tcs_dispose releases it.
// ---------------------------------------------------------------------------

// Allocate a new TCS struct (bz_tcs_t*). Internally calls bz_task_alloc()
// to create the inner task. The returned pointer is owned by the caller
// (typically via `using var tcs := new TaskCompletionSource<T>()`).
void* bz_tcs_new(void);

// Return the inner task pointer (borrowed — ownership stays with the TCS).
// The caller MUST NOT call bz_task_free on the returned pointer; the TCS's
// own disposal (bz_tcs_dispose / bz_tcs_free) releases it.
void* bz_tcs_task(void* tcs);

// Attempt to complete the inner task with a result.
// Atomically checks and sets the completed flag.
// Returns 1 if the task was completed by this call, 0 if already in a terminal
// state. `result` is the bytes to copy into the task's result buffer;
// `result_size` is 0 for a void Task, sizeof(ptr) for a class result, or
// sizeof(T) for a value type result.
// If this returns 0, the result bytes have no effect; the caller has given up
// ownership and must not use the result further (for class T, the caller must
// dispose the object separately when TrySetResult returns 0).
int32_t bz_tcs_try_set_result(void* tcs, const void* result, int64_t result_size);

// Attempt to complete the inner task with an exception object.
// Returns 1 if this call completed the task, 0 if already done.
// exc_obj must be a heap-allocated Bezel exception (vtable + message fields).
// Ownership of exc_obj transfers to the task on success; on failure (return 0)
// exc_obj is NOT freed by this function — the caller is responsible.
int32_t bz_tcs_try_set_exception(void* tcs, void* exc_obj);

// Attempt to complete the inner task with an OperationCanceledException.
// Returns 1 if this call completed the task, 0 if already done.
// oce_vtable is the Bezel vtable* for OperationCanceledException, supplied by
// codegen. If null, falls back to bz_task_complete_error (string only).
int32_t bz_tcs_try_set_cancelled(void* tcs, void* oce_vtable);

// If the task is not yet in a terminal state, complete it with an
// InvalidOperationException("source disposed before completion").
// ioe_vtable is the Bezel vtable* for InvalidOperationException, supplied by
// codegen. If null, falls back to bz_task_complete_error.
// Does NOT free the TCS struct or the inner task; call bz_tcs_free after.
void bz_tcs_fault_if_pending(void* tcs, void* ioe_vtable);

// Release the TCS struct (and its ownership of the inner task) without
// faulting the task first. Call bz_tcs_fault_if_pending first if needed,
// then handle the inner task's error_obj, then call bz_task_fire_and_forget
// on the inner task, then call bz_tcs_free.
// This just frees the bz_tcs_t struct itself.
void bz_tcs_free(void* tcs);

#ifdef __cplusplus
} // extern "C"
#endif
