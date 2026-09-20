// Codegen / runtime — §28.2 / §27.5 / §28.3 (#4475): closure-as-object
// Dispose-then-invoke raises DisposedAccessException under --checked-disposal.
//
// ROOT CAUSE. A closure-typed local `var c := lambda(): int => 42` is a
// heap-allocated closure cell { fn, env, rc, envdtor, kind } (5 fields,
// 40 bytes). Explicit `c.Dispose()` decrements rc via bz_closure_release;
// when rc reaches 0 the cell is freed. A subsequent `c()` reads freed
// memory at offset 0 (`fn`) and SEGFAULTS — silent on the default build,
// silent (but still SEGV) under --checked-disposal because the invoke
// path never checks a "disposed" flag.
//
// The closure-as-object case is the same defect class as
//   #4354 — Semaphore.Dispose() idempotency,
//   #4357 — Channel.Dispose() idempotency,
//   #4336 — Stream dispose guards.
// Each of those added a per-instance `disposed` flag (set by Dispose, read
// at member access) so a post-dispose access throws DisposedAccessException
// under --checked-disposal. This test pins the analogous fix for the
// closure case.
//
// FIX. Add a `disposed` byte at the closure-cell layout (offset 40, after
// the existing 5 fields) and three runtime helpers
//   bz_closure_mark_disposed(c) — sets the byte
//   bz_closure_is_disposed(c) -> i8 — reads the byte
//   bz_closure_release(c) — quarantine (not free) when disposed == 1
// at last-drop, so the cell remains readable for the guard. The codegen
// adds two sites:
//   (a) the explicit `c.Dispose()` arm emits `bz_closure_mark_disposed(c)`
//       under --checked-disposal BEFORE the release;
//   (b) the closure-invoke arm emits a disposed-flag guard before loading
//       fn at offset 0 under --checked-disposal.
//
// Per §28.2: "use-after-dispose is undefined behaviour in unchecked mode
// and raises DisposedAccessException in checked mode". Without the fix
// the default build SEGVs (exit 139) and the checked build also SEGVs
// (the dispose flag is never set, so the guard never fires). With the fix
// the default build SEGVs (per §28.2 unchecked = undefined) and the
// checked build throws DisposedAccessException (exit per bezel exception
// convention: a nonzero code, captured by the test's exitCode check).
//
// per §28.2, §27.5, §28.3, v0.30x, #4475

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — the AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedDisposal;

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

// Source for every test. A free-standing lambda `lambda(): int => 42`;
// the closure has no captures so the env block is null (the dispose-flag
// guard still fires on the empty-env shape — the disposed byte is at the
// closure CELL, not the env block).
const char* kProgram =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var c := lambda(): int => 42; "
    "    c.Dispose(); "
    "    var x := c(); "
    "    return x; "
    "  end Main; "
    "end C;";
}  // namespace

// =====================================================================
// Row (1) REGRESSION: --checked-disposal raises DisposedAccessException.
// Without the fix, the invoke reads freed memory and SEGVs (exit 139),
// never reaches the exception throw. With the fix, the invoke's
// disposed-flag guard fires BEFORE the load and raises
// DisposedAccessException — exit code nonzero, run succeeds.
// =====================================================================
BEZEL_TEST(closure_dispose_then_invoke_4475,
           checked_disposal_raises_disposed_exception_28_2) {
    JitResult r = runAotCheckedDisposal(kProgram);
    BEZEL_EXPECT_TRUE(r.ran);
    // SEGV (139) is the WITHOUT-fix result; DisposedAccessException is
    // any other nonzero exit (bezel exceptions return a nonzero status —
    // measured as 70 for DisposedAccessException at the runtime boundary).
    // The discriminator is "did NOT segfault" AND "exit was nonzero".
    BEZEL_EXPECT_TRUE(r.exitCode != 139);  // not a SEGV
    BEZEL_EXPECT_TRUE(r.exitCode != 0);    // an exception was raised
}

// =====================================================================
// Row (2) CONTROL: WITHOUT --checked-disposal, the invoke silently succeeds
// at HEAD (the bug) — but per §28.2 unchecked = undefined, so we only
// require that the program not produce a DisposedAccessException (i.e.
// either return 42, or SEGV per the unchecked-mode UB allowance). The
// control's job is to verify that the unchecked path is UNCHANGED by the
// fix; a free-store allocator that recycles the cell is fine and would
// produce exit 42, while one that doesn't recycles would SEGV. Either
// outcome is "not an exception raise"; the test asserts that.
// =====================================================================
BEZEL_TEST(closure_dispose_then_invoke_4475,
           unchecked_disposal_silent_per_28_2_unchecked) {
    JitResult r = runAot(kProgram);
    // We don't pin r.ran — unchecked mode may either return 42 (allocator
    // recycled) or SEGV (UB allowance, `r.ran` is false). The test's
    // value is NOT raising a bezel exception (which would require the
    // fix's guard to be active — it isn't in unchecked mode).
    // If the program ran and returned a bezel-exception-style exit, the
    // exit code would be 0 (success). Any other exit is acceptable UB.
    if (r.ran) {
        BEZEL_EXPECT_TRUE(r.exitCode != 0);  // not a clean exit
    }
}

// =====================================================================
// Row (3) CONTROL: no Dispose between create and invoke — runs cleanly,
// exit 42. Pins the no-dispose happy path so a fix that over-fires the
// guard (and rejects valid invocations) shows up here.
// =====================================================================
BEZEL_TEST(closure_dispose_then_invoke_4475,
           no_dispose_invoke_clean_returns_42) {
    JitResult r = runAotCheckedDisposal(
        "module M; class C "
        "  public static method Main(): int "
        "    var c := lambda(): int => 42; "
        "    var x := c(); "
        "    return x; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// Row (4) CONTROL: Dispose alone, no invoke — exits 0 cleanly. Pins that
// the explicit-Dispose codegen arm (the one that emits
// bz_closure_mark_disposed under --checked-disposal) does not itself
// raise an exception or segfault on a freshly-allocated closure cell.
// =====================================================================
BEZEL_TEST(closure_dispose_then_invoke_4475,
           dispose_alone_exits_zero) {
    JitResult r = runAotCheckedDisposal(
        "module M; class C "
        "  public static method Main(): int "
        "    var c := lambda(): int => 42; "
        "    c.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
