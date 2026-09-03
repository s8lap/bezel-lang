// Codegen / semantics + leak — issue #3738 (spec §15.1.1 "Owning inline
// arrays", §15.3 "Move-only value types", §27.5 "Closures", §28.3 "Local
// variables, ownership, and borrows", v0.286): a `[N owned]T` / `[N owned]T?`
// move-only owning inline-array LOCAL declared in a plain (SYNCHRONOUS,
// non-state-machine) LAMBDA body never disposed its slots — the elements'
// destructors never ran, an observable semantic defect (§15.1.1: "the array's
// auto-dispose pass disposes each non-null slot ... For locals, this happens
// at scope end"), not only a leak.
//
// This is the fourth and last body-kind venue in the family #3699 (the three
// state-machine emitters) and #3728 (the async lambda) already closed. Both of
// those fixes explicitly declined the sync-lambda venue and pointed at this
// issue; #3699's `registerInlineOwnedArrayDisposal` guard comment
// (LlvmBackend_Setup.cpp) names it directly:
//
//   // Only register when the transfer analysis covers the current body; a
//   // lambda/thunk body lowered mid-function inherits a stale set, so skip
//   // (a safe leak, never a double-free).
//   if (currentFn_ != moveOnlyArrayTransferredFn_) return true;
//
// ROOT CAUSE: `lowerLambda` (LlvmBackend_Exprs2.cpp) never assigned
// `moveOnlyArrayTransferredFn_` at all on the SYNC path, so `currentFn_` (the
// lambda's own `llvm::Function*`) never equalled `moveOnlyArrayTransferredFn_`
// (whatever the ENCLOSING function last pinned it to) — the guard's
// early-return "safe leak" branch fired unconditionally. This is a DIFFERENT
// root cause than #3728's: the sync lambda's block body already runs
// `lowerScopedBlock`, which correctly pushes/pops/`emitDisposals`s its own
// `disposeStack_` scope, so `using var` / borrowing-`Array<T>` literal reclaim
// already worked here (gated only by `disposeStack_`, not by a Fn-keyed
// token). The gap is narrowly the `moveOnlyArrayTransferredFn_` pin, exactly
// as #3699 fixed for the three state-machine emitters — the fix pins it to
// the lambda's own `fn` and re-runs `collectMoveOnlyArrayTransferred` over
// the lambda's `stmts`, wrapped in the existing `MoveOnlyArrayTransferScope`
// RAII guard (LlvmBackend_impl.hpp, reused as-is — no new predicate) so a
// nested lambda/iterator/state-machine leaves this scope's copy correct on
// return (LANE_RULES §8's clear()+save/restore two-halves contract).
//
// `Cell` bumps a static counter in its constructor and decrements it in its
// destructor, so `n` at exit is a direct oracle for "did the destructor run".
// Every row but the last is `runAotChecked` (generated-code ASan with
// LeakSanitizer on): a double free / use-after-free crashes the row before
// `n` is compared (ran=false -> "<did-not-run>"), and any leftover allocation
// also fails the same way via LSan at exit. 200 activations, full drain.
//
// Measured on r12-cycle-1 + #3699/#3705/#3728 (f517ddb), BEFORE this commit's
// fix, `bezelc --sanitize=address` (rows re-measured against the actual
// gtest rows below, git-checkout of the parent commit's src/, see the commit
// body for the exact per-row numbers):
//
//     normal exit, `[2 owned]Cell?`                  total=600  n=400  (issue's own repro)
//     early return before the array's textual end    total=1000 n=400
//     throw (unwind)                                 total=200  n=400
//     `move a[0]` then null the slot                 total=600  n=200
//     whole-array transfer `var b := a`               total=600  n=400
//     lambda nested inside a sync ITERATOR body       total=600  n=400
//     enclosing method's OWN array after the lambda   total=2000 n=400
//
// per §15.1.1, §15.3, §27.5, §28.3 (issue #3738)

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
// destructor -1. Mirrors tests/codegen/state_machine_inline_owned_array_disposal_3699_test.cpp
// and tests/codegen/async_lambda_scope_end_disposal_3728_test.cpp.
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

// ── Row 1, the PRIMARY repro from the issue: normal fall-through exit from a
// sync (non-async) lambda's block body. Pre-fix `total=600 n=400`, 6400 B /
// 400 allocs — the destructors never ran. 200 * 3 = 600.
// per §15.1.1 (issue #3738)
BEZEL_TEST(sync_lambda_inline_owned_array_disposal_3738,
           normal_exit_disposes_both_slots_spec_15_1_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Run(): int "
        "    var f := lambda() => do "
        "      var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "      return 3; "
        "    end; "
        "    return f(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Run(); end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 2, EARLY RETURN: the `return` that leaves the lambda body is textually
// BEFORE the array's declared scope's fall-through point — the registration
// made at `lowerLocalDecl` time is the SAME one every exit path (normal,
// return, throw) discharges, so this proves the return-handler's
// `emitAllDisposals` call sees it too. 200 * 5 = 1000.
// per §28.3 (issue #3738)
BEZEL_TEST(sync_lambda_inline_owned_array_disposal_3738,
           early_return_disposes_before_leaving_the_body_spec_28_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Run(): int "
        "    var f := lambda() => do "
        "      var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "      if true then return 5; end; "
        "      return 3; "
        "    end; "
        "    return f(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Run(); end; " +
        kTail),
        "total=1000 n=0\n");
}

// ── Row 3, THROW: the lambda body throws after declaring the array; the
// caller catches it. Disposal on the unwind edge is independent of this
// commit's registration-time fix (the exception-discharge machinery already
// walks whatever is on `disposeStack_`), so this proves the registration
// this commit adds is visible on that edge too, not just the normal one.
// 200 * 1 = 200. per §28.3, §15.3 (issue #3738)
BEZEL_TEST(sync_lambda_inline_owned_array_disposal_3738,
           throw_disposes_via_unwind_spec_28_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Run(): int "
        "    var f := lambda() => do "
        "      var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "      throw new InvalidOperationException(\"pqr\"); "
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

// ── Row 4, SOUNDNESS — `move a[0]` nulls the slot, so the array's own
// scope-end disposal must skip it (the moved value is disposed exactly once,
// through `using var t`) while still disposing the REMAINING slot 1. Pre-fix
// slot 0 disposed via `using var` (unaffected by this bug) but slot 1 leaked
// (the array's own disposal never armed) — `total=600 n=200`. Post-fix both
// dispose exactly once: not `n=-200` (double free of slot 0), not `n=200`
// (slot 1 still leaked). 200 * 3 = 600. per §15.3 (issue #3738)
BEZEL_TEST(sync_lambda_inline_owned_array_disposal_3738,
           slot_move_then_null_no_double_free_spec_15_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Run(): int "
        "    var f := lambda() => do "
        "      var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "      using var t: Cell? := move a[0]; "
        "      a[0] := null; "
        "      return 3; "
        "    end; "
        "    return f(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Run(); end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 5, SOUNDNESS — the critical double-free-prevention row named by the
// brief and the issue's own "Notes for whoever fixes it": `var b := a` is a
// whole-array move-only TRANSFER (§15.3) — b's slots alias a's pointers. Both
// `a` and `b` are `[N owned]Cell?` LOCALS of the SAME sync lambda body, so
// BOTH are gated by the SAME `moveOnlyArrayTransferredFn_` token: proving this
// fix RE-RUNS `collectMoveOnlyArrayTransferred` over the lambda's own body
// (rather than merely re-pointing a stale/empty token) — with a stale set
// (or one that doesn't know about this lambda's locals at all) `a` would
// NOT be marked transferred and its disposal would ALSO fire alongside `b`'s
// — a double free of the same two Cells. 200 * 3 = 600.
// per §15.3, §27.5 (issue #3738)
BEZEL_TEST(sync_lambda_inline_owned_array_disposal_3738,
           transferred_out_local_is_not_double_freed_spec_15_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Run(): int "
        "    var f := lambda() => do "
        "      var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "      var b: [2 owned]Cell? := a; "
        "      return 3; "
        "    end; "
        "    return f(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Run(); end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 6, the EXPR-BODY smoke test: `lambda(v) => v + 1` (no `do…end`, so
// `lowerExpr(*lam.exprBody)` is the whole body — no `LocalDecl` statement
// exists, so this fix's `moveOnlyArrayTransferred_.clear()` branch for
// expression bodies is provably a no-op here; the row exists to prove it does
// not disturb the completing VALUE). 200 * 2 = 400.
// per §15.1.1 (issue #3738)
BEZEL_TEST(sync_lambda_inline_owned_array_disposal_3738,
           expr_bodied_lambda_unaffected_spec_15_1_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Run(x: int): int "
        "    var f := lambda(v: int) => v + 1; "
        "    return f(x); "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Run(1); end; " +
        kTail),
        "total=400 n=0\n");
}

// ── Row 7, a sync lambda NESTED INSIDE A SYNC ITERATOR BODY: `Gen`'s own
// state-machine body is already correctly pinned (#3699, `moveNextFn`), but
// the nested lambda's `currentFn_` is the LAMBDA's own function, not
// `moveNextFn` — this row proves the fix applies inside a state-machine host
// too, independent of #3699's unrelated fix to the HOST's own locals.
// 200 * 3 = 600. per §15.1.1, §29.7 (issue #3738)
BEZEL_TEST(sync_lambda_inline_owned_array_disposal_3738,
           lambda_inside_iterator_body_disposes_spec_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    var f := lambda() => do "
        "      var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "      return 3; "
        "    end; "
        "    yield return f(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      for var v in C.Gen() do total := total + v; end; "
        "    end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 8, the SAVE/RESTORE half (LANE_RULES §8 / issue's own "Notes for
// whoever fixes it"): the ENCLOSING method declares its OWN `[2 owned]Cell?`
// AFTER calling a nested sync lambda that ALSO declares one. Without this
// fix's `MoveOnlyArrayTransferScope` restoring the pair on return from the
// lambda, the enclosing method's OWN array would be tested against the
// LAMBDA's stale token/set after the call returns — a leak in the ENCLOSING
// body traded for the lambda's own fix (pre-fix here: the lambda's own `a`
// leaks as always, but the enclosing method's pin was never even pinned BY a
// sync lambda pre-fix, so `ys` alone was already disposing correctly —
// `total=2000 n=400`, all from the lambda's own `a`; a fix that forgot the
// restore half would instead read `n=800`, doubling it with `ys` too). Each
// call returns 3 + 3 + 4 = 10 (both of `ys`'s slots read, not just one, so a
// half-disposed `ys` cannot pass silently); 200 * 10 = 2000.
// per §15.3, §27.5, §28.3 (issue #3738)
BEZEL_TEST(sync_lambda_inline_owned_array_disposal_3738,
           enclosing_method_restores_its_own_transfer_set_after_the_lambda_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Run(): int "
        "    var f := lambda() => do "
        "      var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "      return 3; "
        "    end; "
        "    var r := f(); "
        "    var ys: [2 owned]Cell? := [new Cell(3), new Cell(4)]; "
        "    return r + ys[0]!.V + ys[1]!.V; "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Run(); end; " +
        kTail),
        "total=2000 n=0\n");
}

// ── Row 9, §6b — CAPTURED BY A NESTED LAMBDA: `a` is referenced inside a
// SECOND, more deeply nested lambda `g` (a genuine capture, not a borrow).
// `MoveOnlyArrayTransferWalker`'s LENS 1 (LlvmBackend_Setup.cpp) marks EVERY
// local referenced inside a nested lambda body as TRANSFERRED regardless of
// position, specifically so this fix's newly-armed disposal never targets a
// capture-boxed array (the plain `[N x ptr]` alloca GEP the disposal would
// use is invalid once the array is boxed into a capture cell) — direction of
// danger is a DOUBLE FREE / crash, and this row proves there is neither. This
// reuses `collectMoveOnlyArrayTransferred` completely unmodified (LANE_RULES
// §6): the walker already special-cases this shape for the three
// state-machine emitters and the async lambda, so this row is a regression
// guard on the REUSE, not new logic.
//
// FLIPPED by fix #3757 (§27.5 / §15.3, v0.286): this row was a KNOWN GAP —
// `total=600 n=400` — because the SIBLING Fn-keyed token family
// (`cellFreeEligibleFn_`/`boxCaptureLocalsFn_`, the closure-capture-cell
// disposal family) was, like the §15.3 transfer token this file's own fix
// pins, never re-pinned for the sync-lambda venue either. #3757 pins it
// (`lowerLambda`'s sync branch, reusing the existing `CaptureReclaimSetScope`
// RAII guard, LlvmBackend_impl.hpp — added by fix #3500, no new predicate),
// so `a`'s cell-free-through-capture-cell disposal now registers correctly
// and `n` is `0`. Now `runAotChecked` (ASan/LSan) instead of plain `runAot`:
// with the gap closed there is no longer a known leak to dodge, and this is
// exactly the shape (`[N owned]T` disposed through a capture cell) LSan must
// certify clean. 200 * 3 = 600.
// per §15.3, §27.5 (issue #3738; flipped by #3757)
BEZEL_TEST(sync_lambda_inline_owned_array_disposal_3738,
           captured_by_nested_lambda_no_double_free_spec_27_5) {
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
