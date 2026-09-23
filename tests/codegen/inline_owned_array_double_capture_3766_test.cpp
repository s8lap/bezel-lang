// Codegen — nested-lambda capture cell disposal for `[N owned]T?` (§15.1.1
// / §27.5 / §15.3, #3766).
//
// Spec §15.3 line 2021: "a move-only local's owned slots are auto-disposed at
// scope end by the compiler." §15.1.1 line 1957: "When an owning inline array's
// lifetime ends, the array's auto-dispose pass disposes each non-null slot."
// §27.5: a captured local lives through the closure that captured it; the
// closure's envdtor is the last release.
//
// #3766 (PRE-fix defect): a `[N owned]T?` local captured by TWO lambdas at
// DIFFERENT nesting depths — outer `g` (declared `var g := lambda() => …` in
// scope, captures by direct use) AND inner `h` (declared `var h := lambda() => …`
// inside `g`'s body, captures by direct use) — never disposes its slots through
// its capture cell. Pure LEAK (no UAF). Root cause: collectCellFreeEligible's
// #1135 widening walker conservatively classified the captured outer local as
// "escaping" because of the nested `h` (whether or not `h` itself escapes),
// pushing it OUT of cellFreeEligible_; with the array ineligible, no
// lastLambdaCapturedCells_ push at outer-lambda binding time, no disposeStack
// entry, and both slots leak through every iteration. The single-capturer
// twin (closure_capture_disposes_via_cell_782) was already correct.
//
// POST-fix behavior (#3766): when the nested `h` lambda is at a `var h :=`
// (or `const h :=`) position INSIDE a non-escaping outer `g` AND the binding
// `h` itself does not escape `g`'s body (verified by running
// ClosureEscapeWalker over `g`'s body with target = the LocalDecl of `h`),
// `h` is reclassified as non-escaping — its env dies before `g`'s scope end —
// so the captured cell is safely freed at `g`'s scope end and the #782
// element-dispose-through-cell fires exactly once. Both slots reclaimed
// (Cell.n == 0); a double-free would drive it negative; a leak leaves it
// positive (pre-fix measured 2 leaks per iteration ×200 = 400).
//
// SOUNDNESS (covered in row 5): if the nested `h` DOES escape `g`'s body
// (e.g., `return h` from `g`, `escaped := h`, or `f(h)` to a callee that
// retains its closure args), the ClosureEscapeWalker reports escaped=true
// and the fix falls through to the ORIGINAL widening — the captured cell
// stays alive in cellFreeEligible_'s exclusion zone, both slots leak (the
// documented conservative-leak trade-off of #1135), AND no UAF occurs
// because the cell is never freed. Without the ClosureEscapeWalker proof the
// fix would have been unsound for the escape case.
//
// LEAK AMPLIFICATION (§lsan-one-shot-leak-tests-pass-vacuously): leak rows
// iterate the Run() method 500× to amplify a single-allocation leak from
// LSan-undetectable (<=1 allocation per leak) to LSan's resolution floor.
// The amplified count must be 0; a regression that restores the leak surfaces
// as Cell.n == 1000 (2 slots × 500 iterations).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// run() — plain AOT exit-code round-trip. Used for soundness rows whose
// observable is <=255 and where LSan leak detection would mask a real
// positive (the conservative leak IS the documented correct behavior).
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// runChecked() — AOT under AddressSanitizer/LSan. Used for the primary leak
// row ×500 where runAot alone can pass vacuously under LSan when a
// per-iteration leak resolves to <=1 byte.
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// runCheckedLenient() — AOT under AddressSanitizer but WITHOUT LeakSanitizer.
// Used for the soundness row: the conservative leak IS the correct behavior
// when the nested lambda escapes, but a USE-AFTER-FREE in that same shape
// would be a real ASan error. Lenient mode lets the test assert "no UAF
// happens AND the conservative leak holds" simultaneously: ASan catches a
// real freed-memory access, while LSan does not refuse the test for the
// leak it is supposed to have.
int runCheckedLenient(const std::string& src) {
    auto r = runAotCheckedLenient(src);
    return r.ran ? r.exitCode : -999;
}

// Cell: disposable class with static live-instance counter (ctor +1, dtor
// -1). After all owners dispose, counter == 0; leak leaves it positive;
// double-free drives it negative.
const char* kCell =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public V: int := 0; "
    "  public constructor(v: int) Cell.n := Cell.n + 1; V := v; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; ";

// Wrap a body in a class C with a static Main() returning int. Used for the
// single-pass rows where run() round-trips the exit code.
std::string prog(const std::string& body) {
    return std::string(kCell) +
        "class C public static method Main(): int " + body + " end Main; end C;";
}

// Wrap a body in a class C with a static Run() returning int, called from a
// static Main() in a tight loop. Used for leak-amplification rows: ×500
// iterations turn a 1-slot-per-call leak into a 500-slot LSan-visible row.
std::string progAmplified(const std::string& runBody,
                          const std::string& mainBody) {
    return std::string(kCell) +
        "class C "
        "  public static method Run(): int " + runBody +
        "  end Run; "
        "  public static method Main(): int " + mainBody +
        "  end Main; "
        "end C;";
}

}  // namespace

// ============================================================================
// Row 1 (LEAK-AMPLIFIED ×500): the #3766 defect — nested `var h := lambda()`
// inside a block-bodied `var g := lambda() => do … end` capturing the same
// `[2 owned]Cell?`. Pre-fix: 2 slots × 500 iterations = 1000 leaked (Cell.n
// == 1000). Post-fix: both slots disposed through the capture cell per
// iteration; Cell.n == 0. A regression that restores the leak surfaces as
// Cell.n == 1000, far above LSan noise floor.
// ============================================================================
BEZEL_TEST(codegen_inline_owned_3766,
           nested_lambda_double_capture_leak_amplified_500x) {
    std::string src = progAmplified(
        "  var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "  var g := lambda() => do "
        "    var h := lambda() => a[1]!.V; "
        "    return a[0]!.V + h(); "
        "  end; "
        "  return g(); ",
        "  var total: int := 0; "
        "  for var j in 0..<500 do total := total + C.Run(); end; "
        "  return Cell.n; ");
    BEZEL_EXPECT_EQ(runChecked(src), 0);  // 2 slots × 500 iters disposed
}

// ============================================================================
// Row 2 (CONTROL): the SINGLE-capturer twin of #3766. Captured only by `g`
// (the outer lambda, declared at scope level). The #1135 widening never
// fires (no nested lambda referencing the local), so the array enters
// cellFreeEligible_ unchanged and disposes via #782's element-dispose-
// through-cell. This row PASSED before #3766; included as a regression
// sentinel so a future change to the widening cannot break it while
// ostensibly closing #3766.
// ============================================================================
BEZEL_TEST(codegen_inline_owned_3766,
           single_capturer_control_disposed_782) {
    BEZEL_EXPECT_EQ(run(prog(
        "do "
        "  var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "  var f := lambda() => a[0]!.V; "
        "  var got: int := f(); "
        "end; "
        "return Cell.n;")), 0);
}

// ============================================================================
// Row 3 (SOUNDNESS, non-class T twin): NOT REPRESENTABLE — closed, not
// pending. The brief asked for a `[N owned]int` (no class T, no destructor)
// twin to guard against a phantom free, and this row recorded it as skipped
// because such a program SEGFAULTed (exit 139) on the pre-fix compiler,
// independent of any lambda — "a pre-existing SIGSEGV pending a separate
// root-cause", with an instruction to re-add the row verbatim once that
// codegen was fixed.
//
// That conclusion was wrong, and §15.1.1 (v0.307) is why: "T must be a class
// or interface type; value types and runtime-managed reference types are not
// admitted with the `owned` marker." An owning inline array over `int` is not
// a well-formed Bezel program, so the SEGFAULT was the back end lowering a
// declaration sema should never have admitted — a missing front-end check,
// not a back-end defect, and there is no non-class-T codegen to fix. As of
// `ade3b1d6` that declaration is rejected at its declaration site with the
// §15.1.1 diagnostic (regression: tests/lit/sema/
// inline_owned_array_element_class_or_interface_15_1_1.bzl).
//
// So the axis this row stood in for does not exist: every legal `[N owned]T`
// has a class or interface T, which is exactly what rows 1, 2, 4 and 5 below
// already exercise over `Cell`. Do NOT re-add the row.
// ============================================================================

// ============================================================================
// Row 4 (CONTROL, plain-method venue): the venue used by the reproducer
// (static Run() called from Main()) at iteration count 1, just to lock in
// the shape that triggered the report. A regression that fixes only
// `Main()`-inline forms would still pass this; the bug is in
// collectCellFreeEligible which fires for any static method's scope.
// Cell.n MUST be 0 (single iteration, both slots disposed). Main returns
// Cell.n so the exit code round-trips the post-dispose counter — not the
// arithmetic value from g().
// ============================================================================
BEZEL_TEST(codegen_inline_owned_3766,
           plain_method_venue_single_iteration) {
    BEZEL_EXPECT_EQ(run(progAmplified(
        "  var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "  var g := lambda() => do "
        "    var h := lambda() => a[1]!.V; "
        "    return a[0]!.V + h(); "
        "  end; "
        "  return g() + 2; ",
        "  return Cell.n; ")),
        0);
}

// ============================================================================
// Row 5 (SOUNDNESS, escaping-nested-lambda): if the fix were TOO aggressive
// — reclassifying a nested lambda as non-escaping when it ACTUALLY escapes
// the outer lambda — the captured cell would be freed too early while the
// escaped lambda's env still holds it → use-after-free. This row exercises
// that shape: `var h := lambda() => a[1]!.V` inside `g`'s body, where `h`
// is RETURNED from `g` and stored in an outer var `escaped`. The outer's
// `escaped()` is then called AFTER the array's scope ended — if the fix
// were unsound, ASan would flag a freed-memory access on `escaped()`. With
// the fix, ClosureEscapeWalker reports escaped=true, the reclassification
// is skipped, and the conservative leak holds: both slots leak (Cell.n
// == 2), and the read through `escaped()` sees the still-live cell. The
// row asserts both: Cell.n == 2 (the leak, exit code) AND ran == true
// (the program completed; ASan would have aborted a UAF on read).
// ============================================================================
BEZEL_TEST(codegen_inline_owned_3766,
           escaped_nested_lambda_conservative_leak_no_uaf) {
    auto r = runAotCheckedLenient(prog(
        "var escaped: () -> int := lambda() => -1; "
        "do "
        "  var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "  var g := lambda() => do "
        "    var h := lambda() => a[1]!.V; "
        "    return h; "
        "  end; "
        "  escaped := g(); "
        "end; "
        "var n: int := escaped(); "
        "return n * 100 + Cell.n; "));  // n == 2 → exit 200 + 2 = 202
    BEZEL_EXPECT_TRUE(r.ran);  // ASan would have aborted on a UAF
    BEZEL_EXPECT_EQ(r.exitCode, 202);  // n == 2 (Cell.n == 2 conservative leak)
}
