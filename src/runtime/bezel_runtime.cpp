// bezel_runtime.cpp — Bezel standard library runtime.
// All functions are extern "C" (no mangling) so LLVM IR can call them directly.
#include "bezel_runtime.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
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

void bz_array_clear(void* arrp) {
    if (arrp) ((bz_array_t*)arrp)->count = 0;
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

// Generic-key variants (stub — implement similarly to string variant)
void* bz_dict_alloc_generic(int64_t /*key_size*/,
                              uint64_t (*/*hash*/)(const void*),
                              bool     (*/*eq*/)(const void*, const void*)) {
    // For now, delegate to the string-key variant treating keys as raw bytes.
    return bz_dict_alloc();
}

bool bz_dict_try_get_gen(void* d, const void* key, void** out) {
    // Minimal: treat key as pointer (8-byte comparison)
    char buf[32];
    snprintf(buf, sizeof(buf), "%p", key);
    return bz_dict_try_get_str(d, buf, (int64_t)strlen(buf), out);
}

void bz_dict_set_gen(void* d, const void* key, void* val) {
    char buf[32]; snprintf(buf, sizeof(buf), "%p", key);
    bz_dict_set_str(d, buf, (int64_t)strlen(buf), val);
}

bool bz_dict_remove_gen(void* d, const void* key) {
    char buf[32]; snprintf(buf, sizeof(buf), "%p", key);
    return bz_dict_remove_str(d, buf, (int64_t)strlen(buf));
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

int32_t bz_dir_delete(const char* path, bool /*recursive*/) {
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
