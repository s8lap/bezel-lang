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
// bz_array_t — type-erased dynamic array used by Array<T> and OwnedArray<T>.
// Elements are stored as raw bytes; callers supply the element size.
// ---------------------------------------------------------------------------

// Allocate a new array capable of holding `initial_cap` elements of `elem_size`
// bytes each. Returns a pointer to the internal bz_array_t struct (opaque to
// Bezel code, which treats it as a plain `ptr`).
void* bz_array_alloc(int64_t elem_size, int64_t initial_cap);

// Return the current element count.
int64_t bz_array_len(void* arr);

// §4.14: allocate with n pre-filled (zero-initialized) slots; count == capacity == n.
// Used by `new OwnedBuffer<T>(n)`.
void* bz_array_alloc_count_n(int64_t elem_size, int64_t n);

// Append one element (elem_size bytes) to the end. Grows capacity as needed.
void bz_array_add(void* arr, const void* elem, int64_t elem_size);

// Copy the element at `idx` into `out` (elem_size bytes). No bounds check.
void bz_array_get(void* arr, int64_t idx, void* out, int64_t elem_size);

// Overwrite the element at `idx` with the elem_size bytes at `elem`.
void bz_array_set(void* arr, int64_t idx, const void* elem, int64_t elem_size);

// issue #388 / stdlib §4.5: the OwnedArray<T> indexer setter — dispose the
// (non-null) class-typed element currently at `idx` via `dispose_elem`, then
// overwrite the slot with the elem_size bytes at `elem`. The element is treated
// as a pointer-sized object reference (owning collections constrain
// `where T: class`). `dispose_elem` is null-safe per element. The slot is read
// BEFORE the store so the displaced owner is disposed exactly once, then the new
// owner replaces it. Used by OwnedArray<T>'s `set(owned)` indexer (§4.5).
void bz_array_set_disposing(void* arr, int64_t idx, const void* elem,
                            int64_t elem_size, void (*dispose_elem)(void*));

// Remove the element at `idx`, shifting subsequent elements down.
void bz_array_remove_at(void* arr, int64_t idx, int64_t elem_size);

// fix #642 / stdlib §4.5: PopAt(i) — copy element at idx into *out (transferring
// ownership to the caller) then remove the slot WITHOUT disposal.
void bz_array_pop_at(void* arr, int64_t idx, void* out, int64_t elem_size);

// stdlib §4.5: insert one element (elem_size bytes) at `idx`, shifting the
// existing element at `idx` and all subsequent elements up by one. Grows
// capacity as needed. `idx` is clamped to [0, count] (idx == count appends).
// Used by OwnedArray<T>.Insert(index, item) and Array<T>.Insert.
void bz_array_insert_at(void* arr, int64_t idx, const void* elem,
                        int64_t elem_size);

// Set length to 0 (does NOT call dispose_elem; caller is responsible).
void bz_array_clear(void* arr);

// stdlib §4.5: dispose every (non-null) class-typed element, then set length to
// 0. Used by OwnedArray<T>.Clear() and Array<T>.DisposeElements() where the
// element type is a class. `dispose_elem` is null-safe per element. Treats each
// element as a pointer-sized object reference (owning collections constrain
// `where T: class`, so elements are always object pointers). The slots are
// nulled as they are disposed so a subsequent Dispose finds nothing to redo.
void bz_array_clear_disposing(void* arr, void (*dispose_elem)(void*));

// stdlib §4.5: dispose the (non-null) class-typed element at `idx`, then remove
// it (shifting subsequent elements down). Used by OwnedArray<T>.RemoveAt(i).
// No-op if idx is out of range. Disposing happens before the shift so the slot
// leaves the live range afterward (no double-dispose on a later Dispose).
void bz_array_remove_at_disposing(void* arr, int64_t idx, int64_t elem_size,
                                  void (*dispose_elem)(void*));

// Free the array and, if `dispose_elem` is non-null, call it for each element
// passing a pointer to the element's storage.
void bz_array_free(void* arr, void (*dispose_elem)(void*));

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

// Return a shallow copy of `arr` (same element bytes, no deep clone).
// The caller owns the returned array and must free it with
// bz_array_free(result, /*disposer=*/null).  Used by Array<T>.Reversed()
// and Array<T>.Sorted[...] (stdlib §3 / #472).
void* bz_array_copy(void* arr, int64_t elem_size);

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

// Ensure capacity for at least `cap` elements without changing the length.
void bz_array_reserve(void* arr, int64_t elem_size, int64_t cap);

// §33.4: build an Array<string> (bz_array_t* of bz_string*) from the process
// argv, excluding argv[0] (the executable name).  Called by the C-entry shim
// for Main(args: Array<string>) forms.  Dispose with
//   bz_array_free(result, bz_string_release).
void* bz_args_array(int32_t argc, char** argv);

// ---------------------------------------------------------------------------
// bz_llist_t — doubly-linked list used by LinkedList<T> and OwnedLinkedList<T>.
// stdlib §4.6 / §4.7.  Node layout: { prev*, next*, value[elem_size] }.
// List layout: { head*, tail*, count }.  All pointers are opaque to Bezel code.
// Elements are stored inline in each node (same elem_size model as bz_array_t).
// For OwnedLinkedList<T> pass a non-null dispose_elem; for LinkedList<T> pass null.
// ---------------------------------------------------------------------------

// Allocate an empty list. elem_size = bytes per element value stored inline.
void* bz_llist_alloc(int64_t elem_size);

// Return the number of elements in the list.
int64_t bz_llist_count(void* list);

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

bool    bz_dict_try_get_str(void* d, const char* k, int64_t k_len, void** out);
void    bz_dict_set_str(void* d, const char* k, int64_t k_len, void* val);

// issue #388 / stdlib §4.9: the OwnedDictionary<string,V> indexer setter —
// if the key is already present, dispose the value currently stored under it via
// `dispose_val` (null-safe) before storing the new (owned) value; on a fresh key
// it just inserts. Used by OwnedDictionary<K,V>'s `set(owned)` indexer.
void    bz_dict_set_str_disposing(void* d, const char* k, int64_t k_len,
                                  void* val, void (*dispose_val)(void*));
bool    bz_dict_remove_str(void* d, const char* k, int64_t k_len);
// issue M1 / stdlib §4.9: OwnedDictionary<string,V>.Remove — disposes the
// removed value via dispose_val before unlinking (null-safe: skip if null).
bool    bz_dict_remove_str_disposing(void* d, const char* k, int64_t k_len,
                                     void (*dispose_val)(void*));
bool    bz_dict_contains_str(void* d, const char* k, int64_t k_len);
int64_t bz_dict_count(void* d);

// Enumerate all keys as (char*, int64_t len) pairs. The caller must free the
// returned arrays with free(). `*keys_out` and `*lens_out` are malloc'd arrays.
void bz_dict_keys_str(void* d, char*** keys_out, int64_t** lens_out,
                      int64_t* n_out);

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
// issue M1 / stdlib §4.9: OwnedDictionary<K,V>.Remove for generic keys —
// disposes the removed value via dispose_val before unlinking (null-safe).
bool  bz_dict_remove_gen_disposing(void* d, const void* key,
                                   void (*dispose_val)(void*));
bool  bz_dict_contains_gen(void* d, const void* key);

// fix #643 / stdlib §4.9: PopValue(key) — retrieve the stored value pointer
// (written into *out) then remove the entry WITHOUT disposing the value
// (ownership transfers to the caller).  Returns false when key is absent.
bool  bz_dict_pop_value_str(void* d, const char* k, int64_t k_len, void** out);
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
                             void** val_out);
//
// bz_dict_drain_next_gen — generic-keyed dict (key_size > 0).
// Same as bz_dict_drain_next_str but the key is returned as a malloc'd copy
// of the raw key bytes (size = dict->key_size). The caller must free(key_out).
bool bz_dict_drain_next_gen(void* d, int64_t* cursor,
                             void** key_out, void** val_out);

// issue #639 / stdlib §4.5/§4.7/§4.11 v0.135: ordered owning collection drain.
//
// bz_array_drain_next — pops index 0 (front-to-back); for OwnedArray<T>.Drain()
// (front-to-back) and OwnedQueue<T>.Drain() (FIFO). Returns true if an element
// was popped into *val_out; false when the array is empty.
bool bz_array_drain_next(void* arr, void** val_out);
//
// bz_array_drain_next_lifo — pops the last element (LIFO); for OwnedStack<T>.Drain()
// which pops top-first. Returns true if an element was popped; false when empty.
bool bz_array_drain_next_lifo(void* arr, void** val_out);
//
// bz_llist_drain_next — pops the head node (front-to-back); for OwnedLinkedList<T>.Drain()
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

// Parse from string (stdlib §9.3 Decimal.Parse / TryParse).
// per fix #521: bz_decimal_parse raises FormatException (fe_vtable) for
// malformed input and OverflowException (ove_vtable) for out-of-range.
// bz_decimal_try_parse returns 0/1 with no exception.
void    bz_decimal_parse(const void* str, bz_decimal_t* out,
                         void* ove_vtable, void* fe_vtable);
int32_t bz_decimal_try_parse(const void* str, bz_decimal_t* out);

// ToString (stdlib §9.3 Decimal.ToString()) — returns a new bz_string* (rc=1).
void* bz_decimal_to_string(const bz_decimal_t* a);

// Round(digits: int) — result written to *out.
void bz_decimal_round(const bz_decimal_t* a, int32_t digits, bz_decimal_t* out);

// Literal parse at compile-time (called by runtime init for MinValue/MaxValue).
// Same as bz_decimal_parse but takes a null-terminated C string.
// per fix #521: ove_vtable/fe_vtable may be nullptr for compile-time use
// (literals are lexer-validated; overflow cannot occur for known constants).
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

// Parse the canonical 36-char dashed form from a bz_string*. Returns 1 on
// success (writes *out), 0 on failure (leaves *out unchanged).
int32_t bz_guid_try_parse(const void* str, bz_guid_t* out);

// Parse the canonical form; abort/unreachable on failure (caller guarantees
// well-formed input — used by Guid.Parse where failure is an exception).
// Returns 1 on success, 0 on parse failure (codegen emits a trap on 0).
int32_t bz_guid_parse(const void* str, bz_guid_t* out);

// Bytewise equality: returns 1 if *a == *b, 0 otherwise.
int32_t bz_guid_equals(const bz_guid_t* a, const bz_guid_t* b);

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

// Block the calling thread until the task completes. Returns a pointer to the
// result buffer (owned by the task; valid until bz_task_free). Returns null on error.
void* bz_task_await_sync(void* task);

// Retrieve the error message (valid after bz_task_is_error returns true).
const char* bz_task_error(void* task);

// §30.9: retrieve the original exception object (valid after bz_task_is_error).
// Null when the fault carried only a message.
void* bz_task_error_obj(void* task);

// Register a continuation to run when the task completes (on the thread pool).
void  bz_task_then(void* task, void (*cont)(void* task, void* state), void* state);

// Free the task object. Must be called after the result has been consumed.
void  bz_task_free(void* task);

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

// stdlib §6.7 / §30.10: Task.WhenAllOwned<T>(owned tasks: OwnedArray<OwnedTask<T>>):
//   OwnedTask<OwnedArray<T>> where T: class.
// Pattern B aggregator for class T. Caller transfers ownership of the
// OwnedArray<OwnedTask<T>>; each OwnedTask<T> is awaited and its result
// (a class pointer) collected into a new OwnedArray<T>. The input array and
// all input tasks are freed. Returns an OwnedTask<OwnedArray<T>>.
void* bz_task_when_all_owned(void* task_arr);

// stdlib §6.7 / §30.10: Task.FireAndForget(owned task). Registers a
// continuation on `task` that frees it when done (after discarding any
// result or logging any error). Ownership of `task` transfers; the
// caller must not use or free the pointer again.
void  bz_task_fire_and_forget(void* task);

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
// per §4.6/§4.7 + §5: wrap a bz_llist_t* (LinkedList<T>/OwnedLinkedList<T>) as a
// lazy source so LINQ aggregate/terminal operators can enumerate it. Walks nodes
// head-to-tail; does not own the list (caller still responsible for disposal).
void* bz_lazy_from_llist(void* llist, int64_t elem_size);
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

// ---------------------------------------------------------------------------
// Closure-cell reference counting (§27.5 / §28.9, issue #422 closures
// increment). Function-typed values (lambda closures, capturing or not, and
// method references) are runtime-managed (§28.9): the heap-allocated closure
// object backing the value is reclaimed automatically. The cell is laid out
// { void* fn, void* env, int64 rc } (rc at offset 16, total 24 bytes); every
// producer (lowerLambda, makeMethodThunk) initializes rc to 1. retain
// increments; release decrements and, at zero, frees the env block (if any)
// then the cell. A closure owns NEITHER its captures (they are borrows per
// §28.9 / §28.3) nor the boxed cells backing them, so release frees only the
// cell + its env block — never a captured value (which the original owned
// location disposes exactly once). null is a no-op. Non-atomic for now; becomes
// atomic when async lands.
void bz_closure_retain(void* c);
void bz_closure_release(void* c);

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

// per §9.6 Compare(a, b, ignoreCase: bool): int — ordinal string comparison
// over code points (width-1/byte mode). Returns negative/0/positive like the
// 2-arg form. When `ignore_case` is non-zero, each byte is compared under its
// simple ASCII case-folded form (tolower), per §9.6's note that ignoreCase
// comparison is ordinal case-folding with no culture tailoring. Either pointer
// may be null (a null string compares as empty: null==null is 0, null<non-empty).
int32_t bz_string_compare(const char* a, int64_t a_len,
                          const char* b, int64_t b_len, int32_t ignore_case);

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
// Substring(start, count): fresh string of `count` code points from `start`.
void*   bz_str_substring(const void* self, int64_t start, int64_t count);
// Replace(old, new): fresh string with all occurrences of `old` replaced.
void*   bz_str_replace(const void* self, const void* old_str, const void* new_str);
// Split(sep: char): fresh Array<string> split at every code point equal to sep.
void*   bz_str_split_cp(const void* self, int32_t sep);
// Split(sep: string): fresh Array<string> split at every occurrence of sep.
void*   bz_str_split_str(const void* self, const void* sep);

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

// ---------------------------------------------------------------------------
// per §32.5/§32.6: re-encode a bz_string (layout {ptr data, i64 len, i8 width})
// into a freshly malloc'd, null-terminated UTF-8 byte buffer for the CString
// FFI boundary. The internal storage holds raw code points at width 1/2/4 (a
// width>1 string has interior zero bytes that a C `char*` reader would stop on),
// so the marshalling path must produce true UTF-8 before handing the pointer to
// native code. Returns a malloc'd buffer the caller may free(); never returns
// NULL for a non-NULL input (an empty/allocation-failure case yields "").
char* bz_string_to_utf8z(const void* str);

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
#ifdef __cplusplus
[[noreturn]]
#else
_Noreturn
#endif
void bz_unhandled_exception(void);

// fix #492 / §31.1 / §2.6: the in-flight exception pointer shared across ALL
// separately-compiled modules. Codegen emits an ExternalLinkage reference to
// the symbol "bz_exc" in every module (library or main); the linker resolves
// them all to this single runtime definition. Before the fix each module had
// its own InternalLinkage (BSS) copy under the name "bz$exc", so a cross-module
// `throw` wrote one copy while the caller's pending-check read a different,
// still-null copy. The symbol is now renamed "bz_exc" (a valid C identifier).
// Null means "no exception in flight"; non-null is the heap-allocated object.
extern void* bz_exc;

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
// (vtable Dispose + free, same as OwnedArray<T> element policy).
// ---------------------------------------------------------------------------

// Allocate a new empty Channel<T>. `dispose_elem` is the per-element
// disposer (null for value/string elements; non-null for class elements).
void* bz_channel_new(void (*dispose_elem)(void*));

// Mint a new ChannelSender<T> handle. Increments the channel's sender count.
// Caller owns the returned sender (must release via bz_channel_sender_release).
void* bz_channel_new_sender(void* ch);

// Send one item (via the channel pointer directly). Item ownership transfers.
// Per fix #634 / §30.15: if the channel is already closed or disposed, disposes
// the item and stores a ChannelClosedException in bz_exc (cce_vtable supplies
// the vtable; may be null for best-effort). Codegen emits emitPendingCheck()
// after every Call to this function.
void bz_channel_send(void* ch, void* item, void* cce_vtable);

// Send one item via a ChannelSender handle.
// Per fix #634: also signals ChannelClosedException via bz_exc when closed.
void bz_channel_sender_send(void* tx, void* item, void* cce_vtable);

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

// Domain.Spawn: spawn a fresh domain that runs entry_fn(arg_ptr).
// Returns an OwnedTask bz_task_t* that completes (with the result pointer)
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
// Transfer the accumulated bytes as an owned OwnedBuffer<byte>. The caller
// owns the result. The serializer continues to be valid after this call
// (it retains the buffer for further writes — the semantics here: a fresh
// OwnedBuffer copy is returned, the internal buffer is cleared).
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

// SerializationException throw helper — called by synthesized code on bad input.
// `msg` is a NUL-terminated C string describing the fault (for diagnostics).
// Stores a built-in SerializationException in the global exception slot (bz_exc)
// and returns (codegen emits the unwind branch after the call).
void bz_ser_throw(const char* msg, void* ser_exn_vtable);

// OwnedBuffer<byte> helpers used by BinarySerializer/BinaryDeserializer.
// bz_owned_buffer_alloc: allocate an OwnedBuffer<byte> with the given bytes,
// returning the opaque handle (an owned bz_array_t* wrapper). Caller owns it.
void* bz_owned_buffer_byte_alloc(const uint8_t* data, int64_t len);
// bz_owned_buffer_as_rospan: return a ReadOnlySpan<byte> struct {ptr, len} as
// a pair of {i64 ptr, i64 len} words (matches the codegen layout for spans).
// Actually returns a bz_array_t* with read semantics (span borrows from it).
// The span borrows the buffer; the buffer must outlive the deserializer.
void bz_owned_buffer_get_span(void* buf, const uint8_t** data_out, int64_t* len_out);

#ifdef __cplusplus
} // extern "C"
#endif
