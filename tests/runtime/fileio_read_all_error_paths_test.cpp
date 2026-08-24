// Direct unit tests for bz_file_read_all_bytes / bz_file_read_all_text error
// and boundary paths — stdlib §7.3 File.ReadAllBytes/ReadAllText.
//
// Root cause (WS7 static-analysis run, 2026-07-12, clang-tidy HIGH finding
// R1, bezel_runtime_fileio.cpp:9-30): on a fopen() failure,
// bz_file_read_all_bytes returned an error code WITHOUT writing *out/*len,
// leaving them uninitialized. Every caller — bz_file_read_all_text,
// bz_file_read_all_lines, bz_file_copy, bz_file_read_all_bytes_arr, and the
// File.ReadAllText/ReadAllBytes codegen in LlvmBackend_Calls.cpp — reads
// *out/*len unconditionally on at least one path. In particular
// LlvmBackend_Calls.cpp's `ptrToStr` (File.ReadAllText codegen) takes the
// raw *out pointer as the new bz.string's OWNED data pointer regardless of
// the returned error code: an uninitialized garbage stack value became a
// string's backing pointer, later free()'d by the disposal machinery —
// undefined behavior / heap corruption on every read of a nonexistent file,
// not merely a wrong value.
//
// This file pins the fix directly at the C-runtime level (not via the JIT):
// the internal invariant under test ("outputs are well-defined on every
// return path") is not reliably observable at the Bezel level, since the
// pre-fix bug was undefined behavior that would not deterministically crash.
//
// These tests exercise bz_file_read_all_bytes / bz_file_read_all_text
// directly, linked in via --whole-archive (tests/CMakeLists.txt), mirroring
// the existing tests/runtime/*_test.cpp direct-runtime-unit-test pattern.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "runtime/bezel_runtime.h"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {

// Per-process unique working dir under the shared scratch base. The previous
// hardcoded /tmp/bzt_*.bin literal paths (== /tmp tmpfs) were race-prone:
// parallel shards running sibling tests with the same hardcoded name
// overwrote each other's files mid-test. The atomic unique_stem +
// /var/tmp/bezel-scratch parent fixes that class of flake (#3002-class
// COVERAGE SHORT).
const std::filesystem::path& wtDir() {
    return wtScratchDir("wt-fileio-errpaths");
}

// A path that is guaranteed not to exist (nested under a nonexistent dir, so
// fopen() fails with ENOENT regardless of the working directory's contents).
const std::string kMissingPath =
    (wtDir() / "missing_dir_xyz" / "does_not_exist.bin").string();

} // namespace

// ---------------------------------------------------------------------------
// bz_file_read_all_bytes — fopen-failure path (WS7 R1).
// Before the fix: *out/*len were left uninitialized (stack garbage) on this
// path. After the fix: both are deterministically nullptr/0, and the
// nonzero error code is still returned (errno-style contract unchanged).
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_fileio_ws7_r1, read_all_bytes_missing_file_writes_defined_outputs) {
    uint8_t* out = reinterpret_cast<uint8_t*>(0xDEADBEEF);  // poison, to catch "never written"
    int64_t len = -12345;

    int32_t r = bz_file_read_all_bytes(kMissingPath.c_str(), &out, &len);

    BEZEL_EXPECT_TRUE(r != 0);              // fopen failed -> nonzero (-errno)
    BEZEL_EXPECT_TRUE(out == nullptr);      // §fix: defined, not the poison value
    BEZEL_EXPECT_EQ(len, (int64_t)0);       // §fix: defined, not the poison value
}

// ---------------------------------------------------------------------------
// bz_file_read_all_text — same defect, one level up (stdlib §7.3
// File.ReadAllText). Before the fix: buf/len were UNINITIALIZED LOCALS
// (bezel_runtime_fileio.cpp:25) unconditionally copied into *out/*byte_len
// regardless of the inner call's error code.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_fileio_ws7_r1, read_all_text_missing_file_writes_defined_outputs) {
    char* out = reinterpret_cast<char*>(0xDEADBEEF);
    int64_t byteLen = -12345;

    int32_t r = bz_file_read_all_text(kMissingPath.c_str(), &out, &byteLen);

    BEZEL_EXPECT_TRUE(r != 0);
    BEZEL_EXPECT_TRUE(out == nullptr);
    BEZEL_EXPECT_EQ(byteLen, (int64_t)0);
}

// ---------------------------------------------------------------------------
// bz_file_read_all_bytes — success path on a genuinely empty (0-byte) file.
// This is the boundary the unchecked-ftell defect would have hit hardest
// (sz == 0 -> malloc(1) -> buf[0] = 0): confirm the hardened version still
// handles it cleanly (no OOB write, correct 0 length, non-null buffer).
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_fileio_ws7_r1, read_all_bytes_empty_file_is_zero_length_non_null) {
    std::string path = (wtDir() / "empty.bin").string();
    FILE* f = std::fopen(path.c_str(), "wb");
    BEZEL_EXPECT_TRUE(f != nullptr);
    if (f) std::fclose(f);

    uint8_t* out = nullptr;
    int64_t len = -1;
    int32_t r = bz_file_read_all_bytes(path.c_str(), &out, &len);
    std::remove(path.c_str());

    BEZEL_EXPECT_EQ(r, (int32_t)0);
    BEZEL_EXPECT_TRUE(out != nullptr);      // still a valid (empty) allocation
    BEZEL_EXPECT_EQ(len, (int64_t)0);
    if (out) {
        BEZEL_EXPECT_EQ((int)out[0], 0);    // NUL terminator, in-bounds
        std::free(out);
    }
}

// ---------------------------------------------------------------------------
// bz_file_read_all_bytes — success path, byte-exact round trip, confirming
// the hardening did not disturb the happy path (stdlib §7.3).
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_fileio_ws7_r1, read_all_bytes_success_round_trip) {
    std::string path = (wtDir() / "roundtrip.bin").string();
    const char* content = "hello, bezel";
    FILE* f = std::fopen(path.c_str(), "wb");
    BEZEL_EXPECT_TRUE(f != nullptr);
    if (f) {
        std::fwrite(content, 1, std::strlen(content), f);
        std::fclose(f);
    }

    uint8_t* out = nullptr;
    int64_t len = -1;
    int32_t r = bz_file_read_all_bytes(path.c_str(), &out, &len);
    std::remove(path.c_str());

    BEZEL_EXPECT_EQ(r, (int32_t)0);
    BEZEL_EXPECT_TRUE(out != nullptr);
    BEZEL_EXPECT_EQ(len, (int64_t)std::strlen(content));
    if (out) {
        BEZEL_EXPECT_TRUE(std::memcmp(out, content, std::strlen(content)) == 0);
        std::free(out);
    }
}
