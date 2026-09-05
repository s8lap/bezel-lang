// Codegen / semantics + leak — issue #3757 (spec §27.5 "Closures", §15.1.1
// "Owning inline arrays", §15.3 "Move-only value types", v0.286): a
// `[N owned]T` / `[N owned]T?` move-only owning inline-array LOCAL declared
// in a SYNC (non-async) lambda body, and captured by a NESTED lambda inside
// that same sync-lambda body, never disposed its slots through its capture
// cell — the elements' destructors never ran (a LEAK, never a double-free).
//
// SIBLING of #3738 (fixed: LlvmBackend_Exprs2.cpp, `moveOnlyArrayTransferredFn_`
// — the §15.3 transfer-analysis token). #3738's own regression suite pinned
// this shape as a KNOWN GAP
// (`sync_lambda_inline_owned_array_disposal_3738_test.cpp`,
// `captured_by_nested_lambda_no_double_free_spec_27_5`, `total=600 n=400`)
// because it is a DIFFERENT, sibling Fn-keyed-token family:
// `cellFreeEligibleFn_` / `boxCaptureLocalsFn_` (the closure-capture-cell
// disposal family, §27.5 / #429 / #672 / #694 / #2334 / #2706 / #3426) — also
// never re-pinned for the sync-lambda venue at all (`grep` found no
// `cellFreeEligibleFn_ =` / `boxCaptureLocalsFn_ =` anywhere in
// `lowerLambda`'s sync branch, pre-fix).
//
// ROOT CAUSE: identical shape to #3738's — `collectCellFreeEligible` /
// `collectBoxCaptureLocals`'s admission test is `currentFn_ ==
// cellFreeEligibleFn_` / `currentFn_ == boxCaptureLocalsFn_`, and neither
// token was ever assigned to the sync lambda's own `llvm::Function*`, so the
// guard read whatever the ENCLOSING function last pinned — a mismatch that
// fails every capture-cell eligibility test for the whole sync-lambda body.
//
// FIX: reuse `CaptureReclaimSetScope` (LlvmBackend_impl.hpp, added by fix
// #3500 for exactly this save/restore need in `emitIteratorMethod` /
// `emitAsyncMethod` / `emitAsyncIteratorMethod` — no new predicate or scope)
// around the sync branch's body emission, and re-run
// `collectClosureReleaseEligible` / `collectBoxCaptureLocals` /
// `collectDefinitelyEscapingLambdas` / `collectCellFreeEligible` over the
// lambda's own `stmts`, pinning all of `closureReleaseEligibleFn_` /
// `boxCaptureLocalsFn_` / `cellFreeEligibleFn_` to the lambda's own `fn` —
// mirroring `emitAsyncLambda`'s identical block for its resume fn
// (LlvmBackend_Async.cpp ~L1943-1969) and Setup.cpp's block for a plain
// method body (~L9197-9228) byte-for-byte in the tokens it pins.
//
// `closureReleaseEligibleFn_` is the THIRD member of this family and is also
// the fix for the sibling issue #3762 (a lambda-LOCAL — no capture at all —
// declared and invoked inside a sync lambda body never released its own
// closure cell); see
// `tests/codegen/sync_lambda_closure_release_disposal_3762_test.cpp`.
//
// `Cell` bumps a static counter in its constructor and decrements it in its
// destructor, so `n` at exit is a direct oracle for "did the destructor
// run". Rows 1-3, 5, 6 are `runAotChecked` (generated-code ASan with
// LeakSanitizer on): a double free / use-after-free crashes the row before
// `n` is compared (ran=false -> "<did-not-run>"), and any leftover
// allocation also fails the same way via LSan at exit. 200 activations, full
// drain — the §6b over-release grid this family's danger (a DOUBLE FREE, not
// only a leak) requires: normal / early return / throw / capture by nested
// lambda at 2 and 3 levels / an element moved out from under the capture /
// the enclosing method's own captured array after the call.
//
// Row 4 (capture at 3 levels) is a KNOWN GAP, plain `runAot` (see its own
// comment) — measured IDENTICAL (`total=1000 n=400`, 9600 B / 600
// allocations under ASan, no double-free/UAF) whether `a`'s declaring scope
// is a sync lambda (this file) or a PLAIN METHOD (unaffected by anything
// this commit touches) — a separate, pre-existing defect filed as #3766,
// orthogonal to this family.
//
// Measured on r12-cycle-1 + #3699/#3705/#3728/#3738 (452935e, the local
// cherry-pick of #3738's fix onto this lane's base — NOT the lane-31 SHA
// `452b925` directly, whose tree predates fix #3742 and will not link in
// this worktree), BEFORE this commit's fix, `bezelc` (plain and
// `--sanitize=address`, scratch-copy of the parent's `src/codegen/
// LlvmBackend_Exprs2.cpp` swapped in, rebuilt, measured, restored — LANE_
// RULES §R12-H):
//
//     normal exit, capture 2 levels                    total=600  n=400
//     early return before the nested call               total=1200 n=400
//     throw after reading through the capture           total=200  n=400
//     capture 3 levels (f -> g -> h)  [KNOWN GAP, #3766] total=1000 n=400
//     one slot moved out from under the capture          total=600  n=200
//     enclosing method's OWN captured array after call   total=2000 n=400
//
// AFTER this commit's fix, rows 1, 2, 3, 5, 6 all read `n=0` (verified
// clean under `bezelc --sanitize=address` — 0 leaked, no double-free); row 4
// is unchanged (`total=1000 n=400`, same #3766 gap, same byte count under
// ASan on both sides of this commit).
// per §27.5, §15.1.1, §15.3 (issue #3757)

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// Cell: a disposable class with a static live-instance counter. ctor +1,
// destructor -1. Mirrors sync_lambda_inline_owned_array_disposal_3738_test.cpp.
const char* kHdr =
    "module M; import System; import System.Collections.Generic; "
    "class Cell "
    "  public static n: int := 0; "
    "  public V: int := 0; "
    "  public constructor(v: int) Cell.n := Cell.n + 1; V := v; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; ";

const char* kTail =
    "    Console.WriteLine(\"total=\" + total.ToString() + "
    "                      \" n=\" + Cell.n.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";
}  // namespace

// ── Row 1, NORMAL EXIT, the issue's own repro: `a` captured by a NESTED
// lambda `g` (2 levels: f -> g). g() reads a[0]!.V == 1; f() returns 1+2=3.
// 200 * 3 = 600. per §27.5, §15.1.1 (issue #3757)
BEZEL_TEST(sync_lambda_capture_cell_disposal_3757,
           normal_exit_disposes_captured_array_via_cell_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Run(): int "
        "    var f := lambda() => do "
        "      var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "      var g := lambda() => a[0]!.V; "
        "      return g() + 2; "
        "    end; "
        "    return f(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Run(); end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 2, EARLY RETURN: the `return` that exits the sync lambda body sits
// textually BEFORE the array's declared scope's fall-through point, proving
// the return-handler's disposal walk sees the capture-cell registration this
// fix adds, not only the fall-through path. g() == 1, so the early return
// yields 1+5=6. 200 * 6 = 1200. per §27.5, §28.3 (issue #3757)
BEZEL_TEST(sync_lambda_capture_cell_disposal_3757,
           early_return_disposes_captured_array_via_cell_spec_28_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Run(): int "
        "    var f := lambda() => do "
        "      var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "      var g := lambda() => a[0]!.V; "
        "      if true then return g() + 5; end; "
        "      return 3; "
        "    end; "
        "    return f(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Run(); end; " +
        kTail),
        "total=1200 n=0\n");
}

// ── Row 3, THROW: the sync lambda body reads through the capture then
// throws; the caller catches it. Proves the capture-cell registration this
// fix adds is visible on the unwind edge too (Disposal.cpp's `boxedCellFree`
// fires on both the normal and unwind edges unless explicitly marked
// unwind-unsafe, which this shape is not). Always returns 1. 200 * 1 = 200.
// per §27.5, §28.3 (issue #3757)
BEZEL_TEST(sync_lambda_capture_cell_disposal_3757,
           throw_disposes_captured_array_via_cell_on_unwind_spec_28_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Run(): int "
        "    var f := lambda() => do "
        "      var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "      var g := lambda() => a[0]!.V; "
        "      var v := g(); "
        "      throw new InvalidOperationException(\"v=\" + v.ToString()); "
        "    end; "
        "    try "
        "      f(); "
        "      return 0; "
        "    catch "
        "      var e: Exception => do return 1; end "
        "    end try; "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Run(); end; " +
        kTail),
        "total=200 n=0\n");
}

// ── Row 4, CAPTURE AT 3 LEVELS (f -> g -> h), #3766 FIXED. `a` is declared
// in the SYNC lambda `f`'s own body; `g` (nested in `f`) captures `a`
// directly, and `h` (nested inside `g`) ALSO captures `a` directly — a
// SECOND, independent capture of the same cell, one level deeper than the
// first. Pre-fix (`r12-cycle-1` baseline, before this commit's #3766 fix):
// `total=1000 n=400` (2 slots × 200 iterations leaked through the cell).
// Post-fix: `total=1000 n=0` — the element-dispose-through-cell fires per
// iteration exactly as in rows 1-3/5/6. The fix path is in
// `collectLambdaCaptures`'s #1135 widening walker (LlvmBackend_Setup.cpp ~L29240):
// when the widening walker encounters a nested lambda `h` at a `var h :=
// lambda()` (or `const h := lambda()`) position INSIDE a non-escaping
// outer lambda `g`, AND `h`'s binding does not in turn escape `g`'s body
// (proven by ClosureEscapeWalker), the widening is skipped and `h` is
// reclassified as non-escaping — its captures (`a`) join
// capturedByNonEscaping so `a` lands in cellFreeEligible_ and the cell
// frees at `g`'s scope end. Same oracle as rows 1-3/5/6 (checkedOut /
// ASan+LSan): a regression to a NEGATIVE `n` or a crash is caught by the
// ASan half; a return to the OLD leak is caught by the LSan half. a[0]!.V=1,
// h()=a[1]!.V=2, g()=1+2=3, f()=3+2=5. 200 * 5 = 1000.
// per §27.5 (issues #3757, #3766)
BEZEL_TEST(sync_lambda_capture_cell_disposal_3757,
           capture_three_levels_deep_known_gap_3766_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Run(): int "
        "    var f := lambda() => do "
        "      var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "      var g := lambda() => do "
        "        var h := lambda() => a[1]!.V; "
        "        return a[0]!.V + h(); "
        "      end; "
        "      return g() + 2; "
        "    end; "
        "    return f(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Run(); end; " +
        kTail),
        "total=1000 n=0\n");
}

// ── Row 5, SOUNDNESS — an element MOVED OUT from under the capture: `g`
// captures `a` and reads slot 0 through the cell; slot 1 is separately moved
// into `using var t` and the slot nulled. The array's own cell-free
// disposal must skip the already-null slot 1 (owned by `t` now) while still
// disposing slot 0 through the cell — not a double free of slot 1 (which
// would drive `n` negative), not a leak of slot 0 (n stuck at 1). r=a[0]=1,
// t!.V=2, f() returns 1+2=3. 200 * 3 = 600. per §15.3, §27.5 (issue #3757)
BEZEL_TEST(sync_lambda_capture_cell_disposal_3757,
           element_moved_out_from_under_the_capture_no_double_free_spec_15_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Run(): int "
        "    var f := lambda() => do "
        "      var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "      var g := lambda() => a[0]!.V; "
        "      var r := g(); "
        "      using var t: Cell? := move a[1]; "
        "      a[1] := null; "
        "      return r + t!.V; "
        "    end; "
        "    return f(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Run(); end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 6, the SAVE/RESTORE half (LANE_RULES §8): the ENCLOSING method
// declares its OWN captured array AFTER calling a sync lambda that also
// captures one. Without `CaptureReclaimSetScope` restoring
// `cellFreeEligibleFn_`/`boxCaptureLocalsFn_`/(etc.) to Run's own values on
// return from `f()`, `ys`'s capture by `h` would be tested against `f`'s
// stale token (currentFn_ == RunFn != cellFreeEligibleFn_ == fFn) and leak.
// r = g()+2 = 1+2 = 3; h() = ys[0]+ys[1] = 3+4 = 7; Run() returns 3+7=10.
// 200 * 10 = 2000. per §27.5, §28.3 (issue #3757)
BEZEL_TEST(sync_lambda_capture_cell_disposal_3757,
           enclosing_method_restores_its_own_capture_cell_family_after_the_lambda_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Run(): int "
        "    var f := lambda() => do "
        "      var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "      var g := lambda() => a[0]!.V; "
        "      return g() + 2; "
        "    end; "
        "    var r := f(); "
        "    var ys: [2 owned]Cell? := [new Cell(3), new Cell(4)]; "
        "    var h := lambda() => ys[0]!.V + ys[1]!.V; "
        "    return r + h(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Run(); end; " +
        kTail),
        "total=2000 n=0\n");
}
