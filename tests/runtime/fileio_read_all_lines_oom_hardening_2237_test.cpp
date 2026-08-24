// Direct unit tests for bz_file_read_all_lines' per-line bz.string
// construction (stdlib §7.3 File.ReadAllLines, language spec §28.9).
//
// Root cause (2026-07-12 hardening wave, follow-up to #1762 finding R1 and
// #2234 bz_args_array — see also tests/runtime/args_array_oom_hardening_2234_test.cpp
// and tests/runtime/fileio_read_all_error_paths_test.cpp):
// bz_file_read_all_lines in src/runtime/bezel_runtime_fileio.cpp:105-138
// built each line's bz.string via `malloc((size_t)(lineLen + 1))` and
// `calloc(1, BZ_STRING_SIZEOF)` without null-checking, then memcpys into
// the result. A failed allocation segfaulted on the next memcpy instead of
// returning a clean partial Array<string> the caller could dispose.
//
// This test pins the fix at the C-runtime level (not via the JIT), mirroring
// the args_array_oom_hardening pattern. Two contracts are checked:
//
//   1. Happy path: a multi-line file produces an Array<string> of length n
//      with each element's content equal to the corresponding line in order
//      (CRLF '\r' is stripped, terminal '\n' does not produce a trailing
//      empty entry — the .NET File.ReadAllLines behavior noted in the
//      function's comment). This is the regression: the OOM fix must not
//      change the observable shape of the produced array on the normal path.
//   2. Single-line path: a file with no '\n' produces one element (the
//      whole content), pinning the off-by-one / missing-append cases that
//      the null-check guards could introduce.
//
// Direct OOM injection (LD_PRELOAD malloc hook, or a cgroup OOM trigger) is
// out of scope here — it would lock the test to a runtime build that supports
// a mock allocator. The shape-pinning on the happy path is what proves the
// null-check guards in bz_file_read_all_lines did not disturb the normal case.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "runtime/bezel_runtime.h"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {

// Per-process unique working dir under the shared scratch base. The previous
// hardcoded /tmp/bzt_*.txt literal paths (== /tmp tmpfs) were race-prone:
// parallel shards running sibling tests with the same hardcoded name
// overwrote each other's files mid-test. The atomic unique_stem +
// /var/tmp/bezel-scratch parent fixes that class of flake (#3002-class
// COVERAGE SHORT).
const std::filesystem::path& wtDir2237() {
    return wtScratchDir("wt-2237");
}

namespace fs = std::filesystem;

// Walk a bz_array_t* (the bz_file_read_all_lines return type) and copy each
// element into a std::string. The element type is bz_string_struct* (§4.6,
// 32-byte header: {void* data, int64_t len, int8_t width, int64_t rc}). We
// only read the data/len fields here (rc is internal to bz_string_retain/release).
std::vector<std::string> readLines(void* arr) {
    std::vector<std::string> out;
    if (!arr) return out;
    int64_t n = bz_array_len(arr);
    out.reserve(static_cast<std::size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        void* slot = nullptr;
        bz_array_get(arr, i, &slot, sizeof(void*));
        if (!slot) { out.emplace_back(); continue; }
        void* data = nullptr;
        int64_t len = 0;
        std::memcpy(&data, slot, sizeof(void*));
        std::memcpy(&len, static_cast<uint8_t*>(slot) + 8, sizeof(int64_t));
        if (!data || len <= 0) { out.emplace_back(); continue; }
        out.emplace_back(static_cast<const char*>(data),
                        static_cast<std::size_t>(len));
    }
    return out;
}

// Write the literal bytes `content` to `path`, return true on success.
bool writeFile(const char* path, const char* content, std::size_t n) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::size_t written = std::fwrite(content, 1, n, f);
    std::fclose(f);
    return written == n;
}

} // namespace

// ---------------------------------------------------------------------------
// §7.3: a multi-line file produces an Array<string> with one element per
// line, in order, content-equal to the original lines. CRLF line endings are
// stripped of the trailing '\r'; a terminal '\n' does not produce a trailing
// empty entry. The OOM-hardening fix must not change this — if it did, every
// File.ReadAllLines caller would silently see a corrupted or truncated line
// list.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_fileio_read_all_lines_oom_2237, happy_path_multi_line_round_trip) {
    std::string path = (wtDir2237() / "multi.txt").string();
    const char* cpath = path.c_str();
    // Raw bytes: "alpha\nbeta\r\ngamma\n" — verifies the '\r' strip and the
    // terminal-newline-no-trailing-empty behavior in one file.
    const char content[] = "alpha\nbeta\r\ngamma\n";
    const std::size_t contentLen = sizeof(content) - 1;
    BEZEL_EXPECT_TRUE(writeFile(cpath, content, contentLen));

    void* arr = bz_file_read_all_lines(cpath);
    std::remove(cpath);

    const std::vector<std::string> expected = {"alpha", "beta", "gamma"};
    BEZEL_EXPECT_TRUE(arr != nullptr);
    BEZEL_EXPECT_EQ(bz_array_len(arr), static_cast<int64_t>(expected.size()));
    auto got = readLines(arr);
    BEZEL_EXPECT_EQ(got.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        BEZEL_EXPECT_EQ(got[i], expected[i]);
    }

    // Dispose: each element is a heap-allocated bz_string_struct, so the
    // array's disposer is bz_string_release.
    bz_array_free(arr, bz_string_release);
}

// ---------------------------------------------------------------------------
// §7.3: a single-line file (no '\n') produces an Array<string> with one
// element equal to the whole content. Catches a regression where the fix
// accidentally skipped the only iteration of the loop in the absence of a
// newline terminator.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_fileio_read_all_lines_oom_2237, single_line_no_newline_yields_one_element) {
    std::string path = (wtDir2237() / "single.txt").string();
    const char* cpath = path.c_str();
    const char content[] = "lonely-line-no-newline";
    const std::size_t contentLen = sizeof(content) - 1;
    BEZEL_EXPECT_TRUE(writeFile(cpath, content, contentLen));

    void* arr = bz_file_read_all_lines(cpath);
    std::remove(cpath);

    BEZEL_EXPECT_TRUE(arr != nullptr);
    BEZEL_EXPECT_EQ(bz_array_len(arr), (int64_t)1);
    auto got = readLines(arr);
    BEZEL_EXPECT_EQ(got.size(), (std::size_t)1);
    if (got.size() == 1) {
        BEZEL_EXPECT_EQ(got[0], std::string(content, contentLen));
    }

    bz_array_free(arr, bz_string_release);
}

// ---------------------------------------------------------------------------
// §7.3: an empty file produces an empty Array<string> (length 0). The
// pre-fix code's loop had `if (i == len && lineLen == 0) break;` which
// keeps the empty path null-check-safe; pin that the null-check fix did
// not regress the empty-file case (which has no allocations on the line
// loop).
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_fileio_read_all_lines_oom_2237, empty_file_yields_empty_array) {
    std::string path = (wtDir2237() / "empty.txt").string();
    const char* cpath = path.c_str();
    BEZEL_EXPECT_TRUE(writeFile(cpath, "", 0));

    void* arr = bz_file_read_all_lines(cpath);
    std::remove(cpath);

    BEZEL_EXPECT_TRUE(arr != nullptr);
    BEZEL_EXPECT_EQ(bz_array_len(arr), (int64_t)0);

    bz_array_free(arr, bz_string_release);
}
