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

// Remove the element at `idx`, shifting subsequent elements down.
void bz_array_remove_at(void* arr, int64_t idx, int64_t elem_size);

// Set length to 0 (does NOT call dispose_elem; caller is responsible).
void bz_array_clear(void* arr);

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
bool  bz_dict_remove_gen(void* d, const void* key);

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
// Split a string by a delimiter: returns bz_array_t* of bz.string* (heap alloc'd).
// Caller disposes result array. All pointers point into separately malloc'd strings.
void* bz_string_split(const char* s, int64_t s_len,
                       const char* delim, int64_t delim_len);

// StringBuilder helper — concatenates an array of bz.string pointers.
// Caller must free *out_data with free() after use.
// ---------------------------------------------------------------------------
void bz_strbuilder_to_string(void* parts_arr,  // bz_array_t* of bz.string ptrs
                              char** out_data,
                              int64_t* out_len);

#ifdef __cplusplus
} // extern "C"
#endif
