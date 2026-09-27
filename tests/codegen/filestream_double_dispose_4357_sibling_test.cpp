// Codegen / runtime — §7.2 / §28.2 (#4357 sibling): FileStream.Dispose is idempotent.
//
// Root cause: bz_filestream_dispose had no entry guard. After the first call
// ran `free(fs)`, the second call read `fs->closed` and `fs->fp` from freed
// memory at line 510, then wrote to the freed struct, called `free(fs->path)`
// on whatever random pointer was there, and called `free(fs)` again — UAF /
// double-free. Identical defect class to #4357 channel dispose, fixed the
// same way: `closed` is `std::atomic<int32_t>` and `closed.exchange(1)`
// guards re-entry. Without the guard, glibc's double-free detection (since
// 2.26) aborts the process on the second `free(fs)`; with the guard the
// second dispose is a no-op on the exchange path and `free(fs)` runs once.
//
// Fix (#4357 sibling): change `bz_filestream_t::closed` from `int32_t` to
// `std::atomic<int32_t>` and add `if (fs->closed.exchange(1)) return;` at
// the top of `bz_filestream_dispose` — the same pattern as fix #706 / #4357.
//
// ─────────────────────────────────────────────────────────────────────────
// Why this test uses a C++ REPRO, not Bezel (per the reviewer):
//   The Bezel source-level `using var fs := new FileStream(...); fs.Dispose();
//   fs.Dispose();` is VACUOUS under current codegen: the explicit Dispose
//   site nulls the slot (LlvmBackend_Calls2.cpp:5240-5243) so the second call
//   reloads null and skips before ever reaching bz_filestream_dispose. The
//   runtime fix never gets exercised; the test passes both with and without.
//   The C++ repro bypasses the codegen by calling bz_filestream_dispose twice
//   directly via the runtime header — proving the runtime is idempotent
//   under a real double-call. Verified mechanically:
//     * WITH fix: 5/5 runs exit 42 (no crash).
//     * WITHOUT fix (stash the exchange line, rebuild): 5/5 runs abort with
//       glibc's "double free or corruption" (exit non-42).
//
// Why no ASan leg: the fix's entry guard itself reads `fs->closed` from the
// freed struct (poisoned by ASan) — ASan reports a use-after-free there
// regardless of whether the guard is present, so it cannot discriminate the
// two states. The plain leg is the regression test; the entry-read UAF is
// the unavoidable cost of "subsequent calls are no-ops" semantics for a
// type whose dispose owns the only reference to the object (cf. mutex's
// fix #706 test, which keeps the struct alive via a live Lock so the entry
// read is not a UAF — see tests/runtime/concurrent_dispose_706_test.cpp).
//
// Spec derivation: §28.2 v0.298 (dispose idempotency) + §7.2 v0.298
// (FileStream disposal contract). The FileStream.Dispose contract requires
// exactly-once release; idempotency is required for safe concurrent dispose
// from multiple domains.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "support/build_paths.hpp"  // #5010: the BEZEL_*_PATH / BEZELC_PATH aliases
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace fs = std::filesystem;

namespace {

// Per-process unique working dir under the shared scratch base (never /tmp).
static const fs::path& wtDir4357() {
    return bezel_test::wtScratchDir("wt-4357-fs-dd");
}

// Loop count: 20 amplifies the signal — a fresh heap on each open keeps the
// freed-memory reads observably bad rather than zeroed-out memory that
// happens to be safe (per the original test's intent).
constexpr int kLoopCount = 20;

// The C++ repro source. Opens a FileStream and disposes it twice — directly,
// bypassing the Bezel codegen slot-null, so the runtime fix is actually
// exercised.  The struct is freed by the first dispose, so the second
// dispose's entry read IS a UAF (the fix's cost on this design — see the
// header for why no ASan leg); glibc's malloc catches the resulting double
// `free(fs)` on the plain build.
std::string reproSource(const std::string& path) {
    std::string body;
    body +=
        "// Minimal #4357 sibling repro: bz_filestream_dispose twice on the\n"
        "// SAME pointer. Codegen slot-null is bypassed entirely — we drive\n"
        "// the runtime directly through bezel_runtime.h.\n"
        "//   * WITHOUT the fix: second dispose reads from freed memory,\n"
        "//     calls fclose/free on stale pointers, double-frees the struct\n"
        "//     (glibc's `free()` aborts with \"double free or corruption\"\n"
        "//     since 2.26 — exits non-42).\n"
        "//   * WITH the fix: second dispose returns immediately on\n"
        "//     `closed.exchange(1)` (already-set branch). exit 42.\n"
        "extern \"C\" {\n"
        "  void* bz_filestream_open(const char*, long, int, int);\n"
        "  void  bz_filestream_dispose(void*);\n"
        "}\n"
        "static int do_repro(int iters) {\n"
        "  for (int i = 0; i < iters; ++i) {\n"
        "    void* fs = bz_filestream_open(\"" + path + "\",\n"
        "                                 " + std::to_string(path.size()) + ",\n"
        "                                 1 /*Create*/, 1 /*Write*/);\n"
        "    if (!fs) return 99;\n"
        "    bz_filestream_dispose(fs);  // first: fclose + free path + free fs\n"
        "    bz_filestream_dispose(fs);  // second: stale pointer (UAF/DF)\n"
        "  }\n"
        "  return 42;\n"
        "}\n"
        "int main() { return do_repro(" + std::to_string(kLoopCount) + "); }\n";
    return body;
}

// Compile + link the C++ repro against libbezel_runtime.a. Returns the
// compilation command's status (0 = ok), or -1 on internal failure.
int compileRepro(const std::string& src, const std::string& exe,
                 const std::string& runtimeLib) {
    std::string cmd = "clang++ -std=c++20 -O0 \"" + src + "\" \"" + runtimeLib +
                      "\" -lm -lpthread -lstdc++ -o \"" + exe + "\" > /dev/null 2>&1";
    return std::system(cmd.c_str());
}

// Run the compiled binary; return its exit code (>= 0) on a clean exit,
// or -1 on a fork/system failure.
int runRepro(const std::string& exe) {
    int status = std::system(exe.c_str());
    if (status == -1) return -1;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

}  // namespace

// =====================================================================
// #4357 sibling — plain functional: double-dispose is a no-op (exit 42).
// Glibc's double-free detection (since 2.26) aborts the process without
// the fix; with the fix, every iteration's second dispose is a no-op on
// the exchange path and `free(fs)` runs exactly once.
// =====================================================================
BEZEL_TEST(filestream_double_dispose_4357_sibling,
           double_dispose_no_crash_spec_7_2) {
#ifndef BEZEL_RUNTIME_LIB_PATH
    BEZEL_EXPECT_TRUE(false);   // BEZEL_RUNTIME_LIB_PATH not defined (build config)
    return;
#else
    auto dir = wtDir4357();
    std::string stem = unique_stem("repro4357fs");
    std::string src = (dir / (stem + ".cpp")).string();
    std::string exe = (dir / stem).string();
    std::string path = "/tmp/bzt4357fs_dd_plain.bin";
    {
        std::ofstream out(src);
        out << reproSource(path);
    }
    int rc = compileRepro(src, exe, BEZEL_RUNTIME_LIB_PATH);
    if (rc != 0) {
        std::remove(src.c_str()); std::remove(exe.c_str());
        BEZEL_EXPECT_EQ(rc, 0);   // compile of C++ repro failed
        return;
    }
    int code = runRepro(exe);
    std::remove(src.c_str()); std::remove(exe.c_str());
    BEZEL_EXPECT_EQ(code, 42);
#endif
}
