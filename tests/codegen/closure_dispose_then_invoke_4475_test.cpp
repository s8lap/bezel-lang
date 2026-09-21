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
// Row (2) in DEFAULT (unchecked) mode: DELIBERATELY NOT TESTED HERE.
//
// An `unchecked_disposal_silent_per_28_2_unchecked` cell stood here. It ran
// kProgram through runAot (default/unchecked mode) and, guarded by
// `if (r.ran)`, asserted only that a run which completed did not exit 0.
//
// That assertion cannot hold under the gate's `--full` pass, and removing it
// is not a workaround — the cell was asking for something incoherent. §28.2
// says use-after-dispose is UNDEFINED in unchecked mode: the invoke reads the
// freed closure cell's `fn` field. Generated-code AddressSanitizer, which
// `--full` forces on every test program, exists precisely to catch that
// read: it reports a heap-use-after-free, the harness sets ran=false, and the
// `if (r.ran)` guard then skips the row's only assertion — the row is vacuous
// on every ASan cycle (filed as #4538), and on the plain oracle it is vacuous
// whenever the allocator happens not to recycle the freed cell, which is the
// very nondeterminism the cell's own comment described. Asserting that
// undefined behaviour "runs to completion" under a sanitizer whose whole job
// is to detect it is a contradiction, and the ASan report is the CORRECT
// outcome, not a false positive (the precedent:
// throw_borrow_field_catch_dispose_1678_test.cpp:93-118, §31.1).
//
// Nothing is lost. The cell's own comment said what it actually pinned was
// COMPILE acceptance of kProgram — and that is already pinned by
// `checked_disposal_raises_disposed_exception_28_2` above, which compiles the
// very same kProgram (a compile error there would fail that row first; the
// checked-disposal flag changes only the RUNTIME guard codegen, not
// parse/sema acceptance). The runtime half is unpinnable by construction: the
// spec defines no observable to assert.
// =====================================================================

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
