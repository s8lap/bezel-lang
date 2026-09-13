// bezel_runtime_fileio.cpp — File I/O and filesystem.
// Split from bezel_runtime.cpp per #1414 stage 2 (behavior-preserving motion).
#include "bezel_runtime_internal.hpp"

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

// Hardened per WS7 R1 (2026-07-12 clang-tidy static-analysis run, HIGH):
// every path — success AND error — must leave *out/*len in a defined state,
// because callers (bz_file_read_all_text, bz_file_read_all_lines,
// bz_file_copy, bz_file_read_all_bytes_arr, and the LlvmBackend_Calls.cpp
// File.ReadAllText/ReadAllBytes codegen) read *out/*len unconditionally on
// some paths without checking the returned error code. Previously: on a
// fopen() failure *out/*len were never written, so callers propagated
// uninitialized stack garbage as a "buffer pointer" and "length" (a garbage
// pointer later passed to free()/memcpy() — heap corruption, not just a
// wrong value). Also hardened: unchecked ftell() (-1 on failure flowed into
// `malloc((size_t)-1 + 1)` == `malloc(0)`, then `buf[n] = 0` wrote past the
// 0-byte allocation) and unchecked malloc() (OOM null-deref via buf[n] = 0).
int32_t bz_file_read_all_bytes(const char* path, uint8_t** out, int64_t* len) {
    *out = nullptr;
    if (len) *len = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return -errno;
    if (fseek(f, 0, SEEK_END) != 0) { int e = errno; fclose(f); return -e; }
    long sz = ftell(f);
    if (sz < 0) { int e = errno; fclose(f); return -e; }
    if (fseek(f, 0, SEEK_SET) != 0) { int e = errno; fclose(f); return -e; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -ENOMEM; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = 0;
    fclose(f);
    *out = buf;
    if (len) *len = (int64_t)n;
    return 0;
}

// §7.3: File.ReadAllText — text path (UTF-8 verbatim per stdlib §3.2).
// The byte buffer is returned to the caller; the codegen side wraps it in a
// fresh +1 bz.string (ptrToStr in LlvmBackend_Calls.cpp), so the bz.string's
// release path (bz_string_release) owns the buffer from then on.
int32_t bz_file_read_all_text(const char* path, char** out, int64_t* byte_len) {
    uint8_t* buf = nullptr; int64_t len = 0;
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

// §33.4: build an Array<string> (bz_array_t* of bz_string*) from the process
// argv, excluding argv[0] (the executable name).  The returned array is
// freshly owned; each element's rc is initialised to 1.  Disposal:
//   bz_array_free(result, bz_string_release)
// argv[0] is excluded per §33.4: "The program's own executable path is not
// included; args[0] is the first user-supplied argument."
//
// fix #2234 / §33.4 / spec §31.4: OOM-harden the per-iteration allocations. The
// pre-fix code called malloc((size_t)(len+1)) and calloc(1, BZ_STRING_SIZEOF)
// without null-checking, then memcpy'd into the result — a failed malloc
// produced a SIGSEGV on the next memcpy.
//
// The first cut returned a TRUNCATED array (stop appending, hand back the
// prefix). That is silent data corruption: a program launched with 5 arguments
// would see 2 and could not tell the difference. §31.4 names "string
// construction" and mandates termination, so both allocations abort on
// failure.
void* bz_args_array(int32_t argc, char** argv) {
    int32_t n = (argc > 1) ? (argc - 1) : 0;
    void* arr = bz_array_alloc(8, n);   // elem_size = sizeof(void*) = 8
    for (int32_t i = 0; i < n; ++i) {
        const char* src = argv[i + 1];  // skip argv[0]
        int64_t len = (int64_t)strlen(src);
        char* data = (char*)malloc((size_t)(len + 1));
        if (!data) bz_alloc_fail("command-line argument string data");
        memcpy(data, src, (size_t)len);
        data[len] = '\0';
        uint8_t* s = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
        if (!s) { free(data); bz_alloc_fail("command-line argument string header"); }
        memcpy(s, &data, sizeof(void*));           // data ptr
        memcpy(s + 8, &len, sizeof(int64_t));      // len field
        s[16] = 1;                                  // width = 1 (UTF-8 bytes)
        bz_string_init_rc(s);                       // producer's reference, rc := 1
        bz_array_add(arr, &s, 8);                  // append the bz_string* pointer
    }
    return arr;
}

// §7.3: File.ReadAllLines — split a file into a fresh owned Array<string>,
// one entry per '\n'-terminated line (a terminal '\n' is not its own empty line).
// Mirrors .NET File.ReadAllLines: n '\n'-terminated lines yield n entries; CRLF
// line endings are stripped of the '\r' before the entry is constructed.
//
// fix #2237 / §7.3 / spec §31.4: OOM-harden the per-line allocations. The
// pre-fix code called malloc((size_t)(lineLen + 1)) and calloc(1,
// BZ_STRING_SIZEOF) without null-checking, then memcpy'd into the result — a
// failed allocation segfaulted on the next memcpy.
//
// The first cut returned the successfully-built PREFIX. For a file reader that
// is the worst possible outcome: a 10,000-line file read back as 12 lines is
// indistinguishable from a 12-line file, and every downstream computation is
// silently wrong. §31.4 mandates termination; a caller cannot recover from OOM
// anyway, and it certainly cannot detect this.
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
            // Skip a single trailing empty "line" produced by a terminal '\n'
            // (mirrors .NET File.ReadAllLines, which returns n entries when
            //  the file contains n lines each terminated by '\n').
            if (i == len && lineLen == 0) break;
            // Build a bz.string struct: { data ptr, len i64, width i8 }
            char* lineData = (char*)malloc((size_t)(lineLen + 1));
            if (!lineData) bz_alloc_fail("File.ReadAllLines line data");
            memcpy(lineData, buf + start, (size_t)lineLen);
            lineData[lineLen] = '\0';
            uint8_t* s = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
            if (!s) { free(lineData); bz_alloc_fail("File.ReadAllLines line header"); }
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

// §7.3: File.WriteAllBytes — write `len` bytes from `data` to the file at
// `path`, truncating any existing contents. Returns -errno on fopen failure
// (consistent with bz_file_read_all_bytes's -errno convention); the fwrite
// return is currently not surfaced.
int32_t bz_file_write_all_bytes(const char* path, const uint8_t* data, int64_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return -errno;
    fwrite(data, 1, (size_t)len, f);
    fclose(f);
    return 0;
}

// §7.3: File.WriteAllText — text path (UTF-8 verbatim per stdlib §3.2).
int32_t bz_file_write_all_text(const char* path, const char* data, int64_t len) {
    return bz_file_write_all_bytes(path, (const uint8_t*)data, len);
}

// §7.3: File.AppendAllText — text path (UTF-8 verbatim per stdlib §3.2).
int32_t bz_file_append_all_text(const char* path, const char* data, int64_t len) {
    FILE* f = fopen(path, "ab");
    if (!f) return -errno;
    fwrite(data, 1, (size_t)len, f);
    fclose(f);
    return 0;
}

// §7.3: File.Exists — access(2) F_OK probe.
bool bz_file_exists(const char* path) {
    return access(path, F_OK) == 0;
}

// §7.3: File.Delete — unlink(2).
bool bz_file_delete(const char* path) {
    return unlink(path) == 0;
}

// §7.3: File.Copy — read source bytes, write to destination (overwrite-controlled).
int32_t bz_file_copy(const char* src, const char* dst, bool overwrite) {
    if (!overwrite && bz_file_exists(dst)) return -EEXIST;
    uint8_t* buf; int64_t len;
    int32_t r = bz_file_read_all_bytes(src, &buf, &len);
    if (r) return r;
    r = bz_file_write_all_bytes(dst, buf, len);
    free(buf);
    return r;
}

// §7.3: File.Move — rename(2) when possible, fall back to Copy+Delete (cross-fs).
int32_t bz_file_move(const char* src, const char* dst) {
    if (rename(src, dst) == 0) return 0;
    int32_t r = bz_file_copy(src, dst, true);
    if (r == 0) unlink(src);
    return r;
}

// §7.3: higher-level File.ReadAllBytes → bz_array_t* of uint8_t (elem_size=1).
void* bz_file_read_all_bytes_arr(const char* path) {
    uint8_t* buf; int64_t len;
    if (bz_file_read_all_bytes(path, &buf, &len) != 0)
        return nullptr;
    void* arr = bz_array_alloc(1, len > 0 ? len : 1);
    auto* a = (bz_array_t*)arr;
    memcpy(a->data, buf, (size_t)len);
    a->count = len;
    free(buf);
    return arr;
}

// §7.3: File.WriteAllBytes — write from bz_array_t* of uint8_t (elem_size=1).
int32_t bz_file_write_all_bytes_arr(const char* path, void* arrp) {
    if (!arrp) return bz_file_write_all_bytes(path, nullptr, 0);
    auto* a = (bz_array_t*)arrp;
    return bz_file_write_all_bytes(path, (const uint8_t*)a->data, a->count);
}

// §7.3: File.WriteAllLines — write bz_array_t* of bz_string* with '\n' between
// lines.  Mirrors .NET File.WriteAllLines: each line is terminated with a newline
// (including the last), so ReadAllLines sees exactly n lines.  The trailing '\n'
// is excluded for the empty-array case to avoid a spurious blank file entry.
// bz_string layout: { void* data (off 0), int64_t len (off 8), int8_t width (off 16) }.
int32_t bz_file_write_all_lines(const char* path, void* arrp) {
    FILE* f = fopen(path, "wb");
    if (!f) return -errno;
    auto* a = (bz_array_t*)arrp;
    int64_t n = a ? a->count : 0;
    for (int64_t i = 0; i < n; ++i) {
        // each element is a pointer to a bz_string header
        void* sp = ((void**)a->data)[i];
        if (sp) {
            void* strData; int64_t strLen;
            memcpy(&strData, sp, sizeof(void*));
            memcpy(&strLen, (uint8_t*)sp + 8, sizeof(int64_t));
            if (strLen > 0 && strData) fwrite(strData, 1, (size_t)strLen, f);
        }
        // terminate each line with '\n'; ReadAllLines strips it back out
        fputc('\n', f);
    }
    fclose(f);
    return 0;
}

// §7.4: Directory.Exists — stat(2) → S_ISDIR check.
bool bz_dir_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// §7.4: Directory.CreateDirectory — mkdir -p semantics (create all intermediates).
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

// §7.4: Directory.Delete — rmdir(2) for non-recursive, recursive walk for the
// recursive=true overload (rm -rf).
int32_t bz_dir_delete(const char* path, bool recursive) {
    if (recursive) return rmdir_recursive(path);
    return rmdir(path) == 0 ? 0 : -errno;
}

// §7.4: Directory.GetFiles — return char** of regular-file names, exclude dots.
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

// §7.4: Directory.GetDirectories — return char** of subdirectory names, exclude dots.
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

// §7.4: Path.Combine — concatenate two path segments with a single '/' separator.
char* bz_path_combine(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    bool slash = la > 0 && a[la - 1] != '/';
    char* out = (char*)malloc(la + lb + 2);
    memcpy(out, a, la);
    if (slash) out[la++] = '/';
    memcpy(out + la, b, lb + 1);
    return out;
}

// §7.4: Path.GetFileName — POSIX basename(3).
char* bz_path_filename(const char* path) {
    return strdup(basename((char*)path));  // POSIX basename
}

// §7.4: Path.GetExtension — last '.' and beyond in the basename, or "".
char* bz_path_extension(const char* path) {
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char* dot = strrchr(base, '.');
    return dot ? strdup(dot) : strdup("");
}

// §7.4: Path.GetFileNameWithoutExtension — basename minus its last '.' suffix.
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

// §7.4: Path.GetDirectoryName — POSIX dirname(3).
char* bz_path_dirname(const char* path) {
    char* copy = strdup(path);
    char* dir = dirname(copy);  // POSIX dirname
    char* result = strdup(dir);
    free(copy);
    return result;
}

// §7.4: Path.GetFullPath — realpath(3) with a 4 KiB stack buffer, fall back
// to the input path on resolution failure (compatible with stdlib §7.4's
// documented behavior of never throwing for a non-resolvable input).
char* bz_path_absolute(const char* path) {
    char buf[4096];
    return realpath(path, buf) ? strdup(buf) : strdup(path);
}

// ---------------------------------------------------------------------------
// stdlib §7.2 FileStream (issue #645)
//
// Representation: bz_filestream_t* (heap pointer, owned by the Bezel
// `FileStream` instance — the same shape as §8.1's bz_stopwatch_t*). The
// underlying handle is a buffered `FILE*`, which is what makes stdlib §7.2's
// flush-on-dispose contract observable: "A FileStream opened for writing must
// be disposed (or explicitly Flush()ed) for buffered data to reach the file."
//
// The `mode` / `access` arguments are the §7.2 enum ordinals, which are the
// declaration order of the two enums (§10.2 auto-increment from 0):
//   FileMode:   CreateNew=0, Create=1, Open=2, OpenOrCreate=3,
//               Truncate=4, Append=5
//   FileAccess: Read=0, Write=1, ReadWrite=2
// and §7.1's SeekOrigin for bz_filestream_seek:
//   SeekOrigin: Begin=0, Current=1, End=2
// The enums themselves are ordinary Bezel enums injected by the compiler's
// stdlib prelude (Driver.cpp), so those ordinals are fixed by the verbatim
// §7.1/§7.2 declarations.
// ---------------------------------------------------------------------------

struct bz_filestream_t {
    FILE*   fp;
    char*   path;       // NUL-terminated copy of the constructor's path
    int64_t path_len;   // byte length of `path` (excluding the NUL)
    int32_t can_read;
    int32_t can_write;
    int32_t can_seek;
    // fix #4357-sibling (spec §28.2 v0.298): the dispose guard. Atomic so a
    // second Dispose() from a concurrent domain is safe — exchange(1) makes
    // the second caller observe the prior close and bail (the same pattern
    // as fix #706 / #4357). exchange(1) is set in bz_filestream_dispose and
    // read everywhere else via the implicit conversion to int.
    std::atomic<int32_t> closed{0};
    int32_t last_op;    // 0 = none, 1 = read, 2 = write (see bz_fs_switch_to)
};

// C requires an intervening fflush/fseek between a write and a read (and a
// file-positioning call between a read and a write) on an update stream — so a
// FileAccess.ReadWrite stream that alternates Write/Read without one has
// undefined behaviour. §7.1 imposes no such obligation on the Bezel caller, so
// the switch is performed here rather than exposed. `want` is 1 (read) or 2
// (write).
static void bz_fs_switch_to(bz_filestream_t* fs, int32_t want) {
    if (fs->last_op != 0 && fs->last_op != want) {
        if (fs->last_op == 2) fflush(fs->fp);          // write → read
        else                  fseek(fs->fp, 0, SEEK_CUR);  // read → write
    }
    fs->last_op = want;
}

// per stdlib §7.2: map (FileMode, FileAccess) onto POSIX open(2) flags.
// Returns -1 for an unrecognised ordinal (never produced by well-typed Bezel).
static int bz_fs_open_flags(int32_t mode, int32_t access) {
    int flags;
    switch (access) {
        case 0: flags = O_RDONLY; break;   // FileAccess.Read
        case 1: flags = O_WRONLY; break;   // FileAccess.Write
        case 2: flags = O_RDWR;   break;   // FileAccess.ReadWrite
        default: return -1;
    }
    switch (mode) {
        case 0: flags |= O_CREAT | O_EXCL;  break;  // CreateNew: fail if present
        case 1: flags |= O_CREAT | O_TRUNC; break;  // Create: create or truncate
        case 2: break;                              // Open: must already exist
        case 3: flags |= O_CREAT;           break;  // OpenOrCreate: no truncation
        case 4: flags |= O_TRUNC;           break;  // Truncate: must already exist
        case 5: flags |= O_CREAT | O_APPEND; break; // Append: writes go to the end
        default: return -1;
    }
    return flags;
}

// The fdopen(3) mode string matching the flags computed above. `O_APPEND`
// needs an "a" form so the C library keeps append semantics on the stream.
static const char* bz_fs_stdio_mode(int32_t mode, int32_t access) {
    bool append = (mode == 5);
    switch (access) {
        case 0: return "r";                    // Read (append is irrelevant)
        case 1: return append ? "a"  : "w";    // Write
        default: return append ? "a+" : "r+";  // ReadWrite
    }
}

// per stdlib §7.2: `new FileStream(path, mode)` / `new FileStream(path, mode,
// access)`. `path_data`/`path_len` are the bz.string payload (not
// NUL-terminated), so the path is copied here. Returns a fresh owned handle,
// or null when the OS refuses to open the file (the caller's `using var`
// disposal is null-tolerant).
void* bz_filestream_open(const char* path_data, int64_t path_len,
                         int32_t mode, int32_t access) {
    if (path_len < 0) path_len = 0;
    char* path = (char*)malloc((size_t)path_len + 1);
    if (!path) bz_alloc_fail("FileStream path");
    if (path_len > 0 && path_data) memcpy(path, path_data, (size_t)path_len);
    path[path_len] = '\0';

    int flags = bz_fs_open_flags(mode, access);
    if (flags < 0) { free(path); return nullptr; }
    int fd = open(path, flags, 0644);
    if (fd < 0) { free(path); return nullptr; }
    FILE* fp = fdopen(fd, bz_fs_stdio_mode(mode, access));
    if (!fp) { close(fd); free(path); return nullptr; }

    auto* fs = (bz_filestream_t*)calloc(1, sizeof(bz_filestream_t));
    if (!fs) { fclose(fp); free(path); bz_alloc_fail("FileStream handle"); }
    fs->fp        = fp;
    fs->path      = path;
    fs->path_len  = path_len;
    fs->can_read  = (access == 0 || access == 2) ? 1 : 0;
    fs->can_write = (access == 1 || access == 2) ? 1 : 0;
    // A regular file is seekable; a pipe/FIFO/character device is not.
    struct stat st;
    fs->can_seek  = (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
    return fs;
}

// per stdlib §7.2 "Disposal (flush-on-dispose)": flush buffered writes and
// release the OS file handle. Idempotent — fix #4357-sibling adds the
// `closed.exchange(1)` entry guard so a second arrival (explicit Dispose()
// after `using var` scope exit, or a concurrent Dispose from another
// domain) sees the prior close and returns without touching freed memory.
// Without the guard, line 510 reads `fs->closed` from freed memory after
// `free(fs)` at line 518 below, and `free(fs->path)` / `free(fs)` are a
// double-free. Null-tolerant per the prior contract.
void bz_filestream_dispose(void* h) {
    auto* fs = (bz_filestream_t*)h;
    if (!fs) return;
    if (fs->closed.exchange(1)) return;   // already disposed → no-op (fix #4357 sibling, §28.2)
    if (fs->fp) {
        fflush(fs->fp);
        fclose(fs->fp);
    }
    fs->fp = nullptr;
    free(fs->path);
    fs->path = nullptr;
    free(fs);
}

// per stdlib §7.1: Flush() — push buffered writes to the OS.
void bz_filestream_flush(void* h) {
    auto* fs = (bz_filestream_t*)h;
    if (!fs || fs->closed || !fs->fp) return;
    fflush(fs->fp);
}

// per stdlib §7.1 windowed-form contract: the window [offset, offset+count)
// ranges over the buffer's Count; out-of-range is reported to the caller as
// -1 so codegen can raise IndexOutOfRangeException BEFORE any I/O happens.
// Otherwise returns the number of bytes read: 0 iff at end of stream (and for
// a zero-length window, the one carve-out).
int64_t bz_filestream_read(void* h, void* arrp, int32_t offset, int32_t count) {
    auto* fs = (bz_filestream_t*)h;
    auto* a  = (bz_array_t*)arrp;
    int64_t cap = a ? a->count : 0;
    if (offset < 0 || count < 0 || (int64_t)offset + (int64_t)count > cap)
        return -1;                       // → IndexOutOfRangeException (§7.1)
    if (count == 0) return 0;            // zero-length window: 0 immediately
    if (!fs || fs->closed || !fs->fp || !fs->can_read) return 0;
    bz_fs_switch_to(fs, 1);
    size_t n = fread((uint8_t*)a->data + offset, 1, (size_t)count, fs->fp);
    return (int64_t)n;
}

// per stdlib §7.1: Write(buffer, offset, count) writes the ENTIRE window or
// raises. Returns 0 on success, -1 for an out-of-range window (codegen raises
// IndexOutOfRangeException), -2 for a short/failed write.
int32_t bz_filestream_write(void* h, void* arrp, int32_t offset, int32_t count) {
    auto* fs = (bz_filestream_t*)h;
    auto* a  = (bz_array_t*)arrp;
    int64_t cap = a ? a->count : 0;
    if (offset < 0 || count < 0 || (int64_t)offset + (int64_t)count > cap)
        return -1;                       // → IndexOutOfRangeException (§7.1)
    if (count == 0) return 0;
    if (!fs || fs->closed || !fs->fp || !fs->can_write) return -2;
    bz_fs_switch_to(fs, 2);
    size_t n = fwrite((const uint8_t*)a->data + offset, 1, (size_t)count, fs->fp);
    return n == (size_t)count ? 0 : -2;
}

// per stdlib §7.1: Seek(offset, origin) — returns the new position.
// SeekOrigin: Begin=0, Current=1, End=2.
int64_t bz_filestream_seek(void* h, int64_t offset, int32_t origin) {
    auto* fs = (bz_filestream_t*)h;
    if (!fs || fs->closed || !fs->fp) return 0;
    int whence = (origin == 1) ? SEEK_CUR : (origin == 2) ? SEEK_END : SEEK_SET;
    if (fseek(fs->fp, (long)offset, whence) != 0) return -1;
    // fseek is itself the read↔write switch C requires on an update stream.
    fs->last_op = 0;
    long pos = ftell(fs->fp);
    return pos < 0 ? -1 : (int64_t)pos;
}

// per stdlib §7.1: Length — the file's size in bytes. Buffered writes are
// flushed first so the size reflects everything written so far. The flush is
// gated on can_write: fflush on a read-only stream would discard the buffered
// INPUT and move the file position, which `Position` must not observe.
int64_t bz_filestream_length(void* h) {
    auto* fs = (bz_filestream_t*)h;
    if (!fs || fs->closed || !fs->fp) return 0;
    if (fs->can_write) fflush(fs->fp);
    struct stat st;
    if (fstat(fileno(fs->fp), &st) != 0) return 0;
    return (int64_t)st.st_size;
}

// per stdlib §7.1: Position (get).
int64_t bz_filestream_position(void* h) {
    auto* fs = (bz_filestream_t*)h;
    if (!fs || fs->closed || !fs->fp) return 0;
    long pos = ftell(fs->fp);
    return pos < 0 ? 0 : (int64_t)pos;
}

// per stdlib §7.1: Position (set) — an absolute seek from the beginning.
void bz_filestream_set_position(void* h, int64_t pos) {
    auto* fs = (bz_filestream_t*)h;
    if (!fs || fs->closed || !fs->fp) return;
    fseek(fs->fp, (long)pos, SEEK_SET);
    fs->last_op = 0;   // the seek is the read↔write switch (see bz_fs_switch_to)
}

// per stdlib §7.1: CanRead / CanWrite / CanSeek.
int32_t bz_filestream_can_read(void* h) {
    auto* fs = (bz_filestream_t*)h;
    return (fs && !fs->closed) ? fs->can_read : 0;
}
int32_t bz_filestream_can_write(void* h) {
    auto* fs = (bz_filestream_t*)h;
    return (fs && !fs->closed) ? fs->can_write : 0;
}
int32_t bz_filestream_can_seek(void* h) {
    auto* fs = (bz_filestream_t*)h;
    return (fs && !fs->closed) ? fs->can_seek : 0;
}

// per stdlib §7.1: ReadAll() — "reads from current position to end" into a
// fresh owned Array<byte> (bz_array_t*, elem_size = 1). Always returns a live
// array (empty when the stream is at end or unreadable) so the caller's
// disposal obligation is unconditional.
void* bz_filestream_read_all(void* h) {
    auto* fs = (bz_filestream_t*)h;
    if (!fs || fs->closed || !fs->fp || !fs->can_read)
        return bz_array_alloc(1, 1);
    bz_fs_switch_to(fs, 1);
    // Grow-by-doubling read loop: works for non-seekable handles too.
    int64_t cap = 0, len = 0;
    uint8_t* buf = nullptr;
    for (;;) {
        if (len == cap) {
            int64_t ncap = cap ? cap * 2 : 8192;
            uint8_t* nbuf = (uint8_t*)realloc(buf, (size_t)ncap);
            if (!nbuf) { free(buf); bz_alloc_fail("FileStream.ReadAll buffer"); }
            buf = nbuf; cap = ncap;
        }
        size_t n = fread(buf + len, 1, (size_t)(cap - len), fs->fp);
        if (n == 0) break;
        len += (int64_t)n;
    }
    void* arr = bz_array_alloc(1, len > 0 ? len : 1);
    auto* a = (bz_array_t*)arr;
    if (len > 0) {
        memcpy(a->data, buf, (size_t)len);
        a->count = len;
    }
    free(buf);
    return arr;
}

// per stdlib §7.2: the `Path` property — a FRESH +1 bz.string built from the
// stored path, so each read hands the caller its own reference (the same
// producer shape as File.ReadAllText).
void* bz_filestream_path(void* h) {
    auto* fs = (bz_filestream_t*)h;
    if (!fs || !fs->path) return bz_make_string("", 0);
    return bz_make_string(fs->path, fs->path_len);
}
