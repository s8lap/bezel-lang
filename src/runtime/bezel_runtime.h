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

// Sort in place. `cmp(a, b)` receives pointers to two elements and must return
// negative/zero/positive analogous to strcmp.
void bz_array_sort(void* arr, int64_t elem_size,
                   int (*cmp)(const void*, const void*));

// Return a pointer directly into the array's internal buffer at element `idx`.
// Only valid until the next mutation; used for zero-copy access in codegen.
void* bz_array_ptr_at(void* arr, int64_t idx, int64_t elem_size);

// Ensure capacity for at least `cap` elements without changing the length.
void bz_array_reserve(void* arr, int64_t elem_size, int64_t cap);

// ---------------------------------------------------------------------------
// bz_dict_t — open-addressing hash table used by Dictionary<K,V>.
// Two variants: string keys (most common) and generic keys (via fn pointers).
// Values are stored as raw `void*` (the Bezel object pointer or boxed scalar).
// ---------------------------------------------------------------------------

// String-key dictionary (keys are UTF-8 byte slices; copied internally).
void*   bz_dict_alloc(void);
void    bz_dict_free(void* d, void (*dispose_val)(void*));
bool    bz_dict_try_get_str(void* d, const char* k, int64_t k_len, void** out);
void    bz_dict_set_str(void* d, const char* k, int64_t k_len, void* val);

// issue #388 / stdlib §4.9: the OwnedDictionary<string,V> indexer setter —
// if the key is already present, dispose the value currently stored under it via
// `dispose_val` (null-safe) before storing the new (owned) value; on a fresh key
// it just inserts. Used by OwnedDictionary<K,V>'s `set(owned)` indexer.
void    bz_dict_set_str_disposing(void* d, const char* k, int64_t k_len,
                                  void* val, void (*dispose_val)(void*));
bool    bz_dict_remove_str(void* d, const char* k, int64_t k_len);
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
bool  bz_dict_contains_gen(void* d, const void* key);

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
// DateTime / TimeSpan (100-ns ticks since 0001-01-01 00:00:00 UTC,
// matching the Bezel spec §2.5 which follows .NET DateTime semantics)
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

// Mark the task as done with a result (result_size bytes copied from result).
// Wakes any blocked awaiter and runs registered continuations.
void  bz_task_complete(void* task, const void* result, int64_t result_size);

// Mark the task as failed with an error message.
void  bz_task_complete_error(void* task, const char* error_msg);

// Check completion without blocking.
bool  bz_task_is_done(void* task);
bool  bz_task_is_error(void* task);

// Block the calling thread until the task completes. Returns a pointer to the
// result buffer (owned by the task; valid until bz_task_free). Returns null on error.
void* bz_task_await_sync(void* task);

// Retrieve the error message (valid after bz_task_is_error returns true).
const char* bz_task_error(void* task);

// Register a continuation to run when the task completes (on the thread pool).
void  bz_task_then(void* task, void (*cont)(void* task, void* state), void* state);

// Free the task object. Must be called after the result has been consumed.
void  bz_task_free(void* task);

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

#ifdef __cplusplus
} // extern "C"
#endif
