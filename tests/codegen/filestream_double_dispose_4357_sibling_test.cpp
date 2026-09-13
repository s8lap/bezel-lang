// Codegen / runtime — §7.2 / §28.2 (#4357 sibling): FileStream.Dispose is idempotent.
//
// Root cause: bz_filestream_dispose had no entry guard. After the first call
// ran `free(fs)`, the second call read `fs->closed` and `fs->fp` from freed
// memory at line 510, then wrote to the freed struct, called `free(fs->path)`
// on whatever random pointer was there, and called `free(fs)` again — UAF /
// double-free. Identical defect class to #4357 channel dispose, fixed the
// same way: `closed` is `std::atomic<int32_t>` and `closed.exchange(1)`
// guards re-entry. Without the guard, ASan catches the double-free and a
// concurrent Dispose from two domains is a data race on the plain int.
//
// Fix (#4357 sibling): change `bz_filestream_t::closed` from `int32_t` to
// `std::atomic<int32_t>` and add `if (fs->closed.exchange(1)) return;` at
// the top of `bz_filestream_dispose` — the same pattern as fix #706 / #4357.
//
// Test: dispose a FileStream twice. The second call must be a no-op (no
// crash, no double-free). Loops 20× so the allocator is more likely to
// hand the same chunk back and the freed-struct reads become observably
// bad memory rather than zeroed-out memory that happens to be safe.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — the AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// A checked run is SKIPPED (not failed) when the ASan toolchain is absent:
// runAotChecked returns ran=false with a "SKIP" diagnostic. clang is required
// to build this repo, so a skip is not expected — guard anyway.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// Module header + class shell + Main body + close. Built around the body so
// the Bezel source can contain `";` (inside `end C;`) without prematurely
// closing a C++ raw-string literal.
std::string wrapMain(const std::string& body) {
    return std::string(
        "module M; "
        "class C "
        "  public static method Main(): int "
    ) + body +
        "  end Main; "
        "end C;";
}

const std::string kBody =
    "    using var fs := new FileStream(\"/tmp/bzt4357fs_dd.bin\", "
    "                                FileMode.Create, FileAccess.Write); "
    "    using var b := new Array<byte>(); b.Add(byte(1)); "
    "    fs.Write(b, 0, 1); "
    "    fs.Dispose(); "       // first dispose: fclose + free path + free fs
    "    fs.Dispose(); "       // second dispose: no-op with the fix; crash without
    "    var i := 0; "
    "    while i < 20 do "     // amplify: ASan exit code shifts more reliably
    "      using var fs2 := new FileStream(\"/tmp/bzt4357fs_dd.bin\", "
    "                                  FileMode.Open, FileAccess.Read); "
    "      fs2.Dispose(); "    // using-var scope end AND explicit — same path
    "      fs2.Dispose(); "    // second dispose: still a no-op with the fix
    "      i := i + 1; "
    "    end; "
    "    return 42; ";         // if we get here, the double-dispose was harmless

}  // namespace

// =====================================================================
// #4357 sibling — functional: double-dispose is a no-op (exit code 42).
// Without the fix: second dispose reads freed memory and double-frees.
// With the fix: second dispose returns immediately, no crash.
// =====================================================================
BEZEL_TEST(filestream_double_dispose_4357_sibling,
           double_dispose_no_crash_spec_7_2) {
    JitResult r = runAot(wrapMain(kBody));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// #4357 sibling — leak / ASan: double-dispose must not double-free. Without
// the fix the second dispose reads `fs->closed` from freed memory and
// `free(fs->path)` / `free(fs)` are a double-free that ASan / LSan catches.
// With the fix the second call is a no-op.
// =====================================================================
BEZEL_TEST(filestream_double_dispose_4357_sibling,
           double_dispose_no_double_free_spec_7_2) {
    JitResult r = runAotChecked(wrapMain(kBody));
    if (wasSkipped(r)) return;            // ASan toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(r.ran);             // no UAF / double-free
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}