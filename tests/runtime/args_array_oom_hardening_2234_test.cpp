// Direct unit tests for bz_args_array's argv -> Array<string> construction
// (stdlib §33.4, language spec §33.2 / §33.6).
//
// Root cause (2026-07-12 hardening wave, follow-up to #1762 finding R1 — see
// also tests/runtime/fileio_read_all_error_paths_test.cpp):
// bz_args_array in src/runtime/bezel_runtime_fileio.cpp:68 built each
// per-arg string via `malloc((size_t)(len + 1))` and
// `calloc(1, BZ_STRING_SIZEOF)` without null-checking, then memcpys into the
// result. A failed allocation produced a SIGSEGV on the next memcpy instead
// of a clean partial Array<string> the caller could dispose.
//
// This test pins the fix at the C-runtime level (not via the JIT), mirroring
// the fileio_read_all_error_paths pattern. Two contracts are checked:
//
//   1. Happy path: a non-empty argv produces an Array<string> of length n
//      with each element's content equal to the corresponding argv[i+1]
//      (i.e. argv[0] — the executable path — is correctly excluded per
//      §33.4). This is the regression: the OOM fix must not change the
//      observable shape of the produced array on the normal path.
//   2. Empty-argv path: argc <= 1 (no user args) produces an array of
//      length 0, again per §33.4 ("args[0] is the first user-supplied
//      argument").
//
// Direct OOM injection (LD_PRELOAD malloc hook, or a cgroup OOM trigger) is
// out of scope here — it would lock the test to a runtime build that supports
// a mock allocator. The shape-pinning on the happy path is what proves the
// null-check guards in bz_args_array did not disturb the normal case.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/bezel_runtime.h"
#include "test_harness.hpp"

namespace {

// Walk a bz_array_t* (the bz_args_array return type) and copy each element
// into a std::string. The element type is bz_string_struct* (§4.6, 32-byte
// header: {void* data, int64_t len, int8_t width, int64_t rc}). We only
// read the data/len fields here (rc is internal to bz_string_retain/release).
std::vector<std::string> readArgs(void* arr) {
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

} // namespace

// ---------------------------------------------------------------------------
// §33.4: a non-empty argv produces an Array<string> with one element per
// user arg, in order, content-equal to argv[1..]. The OOM-hardening fix
// must not change this — if it did, Main(args: Array<string>) programs
// would silently see a corrupted argv.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_args_array_oom_2234, happy_path_excludes_argv0_and_preserves_content) {
    std::vector<std::string> userArgs = {"alpha", "beta", "gamma-delta"};
    // Construct a C-style argv with argv[0] = "program" (the executable).
    std::vector<std::vector<char>> storage;
    storage.reserve(userArgs.size() + 1);
    storage.push_back({'p','r','o','g','r','a','m','\0'});
    for (const auto& s : userArgs) {
        std::vector<char> buf(s.begin(), s.end());
        buf.push_back('\0');
        storage.push_back(std::move(buf));
    }
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& s : storage) argv.push_back(s.data());
    int32_t argc = static_cast<int32_t>(argv.size());

    void* arr = bz_args_array(argc, argv.data());

    BEZEL_EXPECT_TRUE(arr != nullptr);
    BEZEL_EXPECT_EQ(bz_array_len(arr),
                    static_cast<int64_t>(userArgs.size()));
    auto got = readArgs(arr);
    BEZEL_EXPECT_EQ(got.size(), userArgs.size());
    for (std::size_t i = 0; i < userArgs.size(); ++i) {
        BEZEL_EXPECT_EQ(got[i], userArgs[i]);
    }

    // Dispose: each element is a heap-allocated bz_string_struct, so the
    // array's disposer is bz_string_release.
    bz_array_free(arr, bz_string_release);
}

// ---------------------------------------------------------------------------
// §33.4: argc == 0 (or argc == 1 with only argv[0]) produces an empty
// Array<string>. Catches a regression where the fix accidentally
// pre-allocates a non-empty buffer in the n == 0 case.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_args_array_oom_2234, zero_user_args_yields_empty_array) {
    std::vector<char> prog{'p','r','o','g','r','a','m','\0'};
    char* argv[1] = { prog.data() };

    void* arr = bz_args_array(1, argv);
    BEZEL_EXPECT_TRUE(arr != nullptr);
    BEZEL_EXPECT_EQ(bz_array_len(arr), (int64_t)0);
    bz_array_free(arr, bz_string_release);

    // argc == 0 is the C-language edge case: bz_args_array is not actually
    // called this way (libc always passes argc >= 1), but the function
    // handles it — pin that the empty case still works.
    void* arr2 = bz_args_array(0, nullptr);
    BEZEL_EXPECT_TRUE(arr2 != nullptr);
    BEZEL_EXPECT_EQ(bz_array_len(arr2), (int64_t)0);
    bz_array_free(arr2, bz_string_release);
}
