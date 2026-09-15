// Codegen — issue #4227: reassigning a function-typed (closure) local that
// IS ITSELF CAPTURED by an escaping lambda, from INSIDE the capturing
// lambda's own body, never releases the DISPLACED previous value. This is
// the captured-cell twin of #4174 (which fixed the plain localSlot() path).
//
// Measured on the pre-fix compiler (`bezelc --sanitize=address`,
// LeakSanitizer on, 500x amplified):
//
//     var f: () -> int := lambda() => 0;
//     var mutate := lambda() => do
//       f := lambda() => 42;   // reassign captured local from INSIDE capture
//     end;
//     mutate();
//     return f;
//                                   ->  40 bytes / 1 allocation leaked per call
//
// ROOT CAUSE. `LlvmBackend_Stmts.cpp`'s AssignmentStmt lowering has a
// retain-new/release-old "drop-on-overwrite" helper for closure locals
// (`lowerClosureLocalReassign`, fix #4174), but it is gated on
// `currentFn_ == closureReleaseEligibleFn_` — and for a captured local
// declared in an outer scope, the analyzed function (where the local is
// declared) differs from the lowering function (the capturing lambda body
// where the reassignment occurs). The captured-cell path therefore falls
// through to a plain store with no release. The fix is a separate helper
// (`lowerCapturedClosureLocalReassign`) that handles the captured-cell path
// unconditionally, mirroring `lowerClosureLocalReassign`'s own
// retain-before-release ordering for self-assignment safety.
//
// CITATION NOTE. Same consideration as #4174's file header: function values
// are declared runtime-managed by §28.9 ("exempt from manual disposal... the
// type's lifetime is managed by the runtime without user involvement"), but
// §27.5 never mentions reassignment and §28.7's drop-on-overwrite rule
// explicitly scopes itself away from function-typed locals. This is an
// internal refcounting-soundness fix, not a spec-mandated rule.
//
// FAIL-WITHOUT / PASS-WITH, measured program-by-program (500x-loop-amplified
// where noted) on one build of each compiler:
//
//   program                                          pre-fix   post-fix
//   f captured by mutate(); f reassigned inside
//     mutate(); return f; (x500 outer loop,
//     runAotChecked)                                FAIL      PASS (clean)
//   f NOT captured, reassigned from OUTSIDE
//     (parity with #4174's own path, x500,
//     runAotChecked)                                PASS      PASS (clean)
//
// per issue #4227 (see the CITATION NOTE above for why no spec section is
// claimed as authority for the reassignment mechanism itself)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── The issue's exact repro, 500x amplified. Pre-fix: 20000 bytes / 500
// allocations leaked (the displaced `lambda() => 0` cell, once per call).
// The returned value must still be correct (42) — the release must not
// disturb the TRANSFERRED final closure. (issue #4227)
BEZEL_TEST(closure_local_reassign_captured_cell_4227,
           captured_closure_local_reassigned_from_inside_capture_body_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method MakeClosure(): () -> int "
        "    var f: () -> int := lambda() => 0; "
        "    var mutate := lambda() => do "
        "      f := lambda() => 42; "
        "    end; "
        "    mutate(); "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      var g := C.MakeClosure(); "
        "      total := total + g(); "
        "      i := i + 1; "
        "    end; "
        "    return total mod 97; "
        "  end Main; "
        "end C;"),
        /* 500 * 42 = 21000; 21000 mod 97 = 3 */ 21000 % 97);
}

// ── PARITY: binding a closure local exactly once (no reassignment) was
// already clean pre-fix and must stay clean. (issue #4227)
BEZEL_TEST(closure_local_reassign_captured_cell_4227,
           unreassigned_captured_closure_local_still_clean_parity_spec_27_5) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method MakeClosure(): () -> int "
        "    var f: () -> int := lambda() => 42; "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      var g := C.MakeClosure(); "
        "      total := total + g(); "
        "      i := i + 1; "
        "    end; "
        "    return total mod 97; "
        "  end Main; "
        "end C;"),
        21000 % 97);
}

// ── PARITY: non-captured closure local reassigned from OUTSIDE (the path
// #4174 fixed) must remain clean. (issue #4227, parity with #4174)
BEZEL_TEST(closure_local_reassign_captured_cell_4227,
           non_captured_reassign_from_outside_still_clean_parity_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method MakeClosure(): () -> int "
        "    var f: () -> int := lambda() => 0; "
        "    f := lambda() => 42; "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      var g := C.MakeClosure(); "
        "      total := total + g(); "
        "      i := i + 1; "
        "    end; "
        "    return total mod 97; "
        "  end Main; "
        "end C;"),
        21000 % 97);
}

// ── SCALING axis: TWO reassignments inside the capturing lambda displace
// TWO distinct producer values, so the leak scales linearly.
// Pre-fix: 40000 bytes / 1000 allocations (2 x 20000/500) leaked.
// (issue #4227)
BEZEL_TEST(closure_local_reassign_captured_cell_4227,
           two_reassignments_inside_capture_body_both_displaced_released_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method MakeClosure(): () -> int "
        "    var f: () -> int := lambda() => 0; "
        "    var mutate := lambda() => do "
        "      f := lambda() => 1; "
        "      f := lambda() => 2; "
        "    end; "
        "    mutate(); "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      var g := C.MakeClosure(); "
        "      total := total + g(); "
        "      i := i + 1; "
        "    end; "
        "    return total mod 97; "
        "  end Main; "
        "end C;"),
        /* 500 * 2 = 1000; 1000 mod 97 */ 1000 % 97);
}
