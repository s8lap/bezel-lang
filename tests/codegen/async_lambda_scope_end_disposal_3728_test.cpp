// Codegen / semantics + leak — issue #3728 (spec §15.1.1 "Owning inline
// arrays", §15.3 "Move-only value types", §28.3 "Local variables, ownership,
// and borrows", §28.10 "`using var`", §28.11 "Collection element ownership",
// §30.1 "Async methods", §30.5 "Result lifetime and Task disposal", §30.6
// "Async lambdas", v0.286): an ASYNC LAMBDA body never ran its scope-end
// disposals.
//
// `emitAsyncLambda`'s "Emit the body from state 0" block (LlvmBackend_Async.cpp)
// pushed a `disposeStack_` scope, lowered the body, and popped the scope with
// NO `emitDisposals` call between push and pop — every `using var` / owned
// local / borrowing-`Array<T>` literal registered into that scope was
// discarded on the NORMAL (fall-through) completion edge. `emitAsyncMethod`'s
// identical block (~L1610, issue #938) already ran `emitDisposals` there; this
// is the exact same fix applied to the LAMBDA venue's block-body AND
// expr-body normal edges, gated by the same `term` / block-terminator checks
// `emitAsyncMethod` uses so a body that already terminated (an explicit
// `return`, which ran `emitAllDisposals` via the return handler, or a throw,
// which diverted to the fault boundary) is not double-disposed.
//
// A SECOND, separate gap shared the same root cause: `[N owned]T` /
// `[N owned]T?` locals never registered a disposal at all in an async lambda,
// because `moveOnlyArrayTransferredFn_` (the #3699/§15.3 move-only-array
// TRANSFER token) was never re-pinned to the lambda's resume fn — issue #3699
// gave `emitAsyncMethod` / `emitIteratorMethod` / `emitAsyncIteratorMethod`
// this re-establishment but explicitly declined the lambda venue as
// "measured to have ZERO effect" (nothing consumed the registration until
// this commit's `emitDisposals` fix landed). This commit adds the same
// re-establishment (`collectMoveOnlyArrayTransferred` + pin, mirroring
// `emitAsyncMethod`'s block) plus the LlvmBackend_Exprs2.cpp save/restore
// `lowerLambda` needs so a lambda/iterator NESTED inside the async lambda's
// own body does not leave `moveOnlyArrayTransferredFn_` pointing at the wrong
// function when control returns to the enclosing body (LANE_RULES §8 — a
// `clear()`/pin and a save/restore around nested emission are two halves of
// one contract).
//
// `Cell` bumps a static counter in its constructor and decrements it in its
// destructor, so `n` at exit is a direct oracle for "did the destructor run",
// per §15.1.1's "the array's auto-dispose pass disposes each non-null slot."
// Every row is `runAotChecked` (generated-code ASan with LeakSanitizer ON):
// a double free / use-after-free crashes the row before `n` is compared
// (ran=false -> "<did-not-run>", which fails the string match), and any
// leftover allocation (a leak) also fails the SAME way via LSan at exit
// (detect_leaks=1) — no separate leak assertion is needed. 200 activations,
// full drain, matching the issue's own measured grid.
//
// Measured on r12-cycle-1 + #3699/#3705 (5a0aaf2), BEFORE this commit's fix,
// `bezelc --sanitize=address`:
//
//     async lambda, `using var`, with await         total=600 n=200   3200 B / 200
//     async lambda, `using var`, no internal await   total=600 n=200   3200 B / 200
//     async lambda, `Array<int>` literal, with await total=600         24000 B / 400 (LSan only, no dtor)
//     async lambda, `[2 owned]Cell?`, with await     total=600 n=400   6400 B / 400
//     async METHOD twin of row 1 (control)           total=600 n=0     clean
//
// per §15.1.1, §15.3, §28.3, §28.10, §28.11, §30.1, §30.5, §30.6 (issue #3728)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// Cell: a disposable class with a static live-instance counter. ctor +1,
// destructor -1. Mirrors tests/codegen/state_machine_inline_owned_array_disposal_3699_test.cpp.
const char* kHdr =
    "module M; import System; import System.Threading; "
    "import System.Threading.Tasks; "
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

// ── Row 1, the PRIMARY repro from the issue: `using var` in an async lambda
// body, WITH an internal `await`. Pre-fix `total=600 n=200`, 3200 B / 200
// allocs — the destructor never ran. 200 * 3 = 600.
// per §28.10, §30.6 (issue #3728)
BEZEL_TEST(async_lambda_scope_end_disposal_3728,
           using_var_with_await_disposes_on_normal_edge_spec_28_10) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Run(): Task<int> "
        "    var f := async lambda() => do "
        "      using var r := new Cell(1); "
        "      await Task.Delay(1); "
        "    end; "
        "    await f(); "
        "    return 3; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + await C.Run(); end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 2, the SYNCHRONOUS-DRIVE variant: `using var` with NO internal await
// at all, so Resume completes within the initial synchronous drive (§30.1).
// Pre-fix `total=600 n=200`, identical numbers, different reason the resume
// never reaches an `await` at all — it is the fall-through terminator path,
// not the suspend/resume dispatch, that was broken.
// per §28.10, §30.1, §30.6 (issue #3728)
BEZEL_TEST(async_lambda_scope_end_disposal_3728,
           using_var_no_internal_await_disposes_on_normal_edge_spec_30_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Run(): Task<int> "
        "    var f := async lambda() => do "
        "      using var r := new Cell(1); "
        "    end; "
        "    await f(); "
        "    return 3; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + await C.Run(); end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 3, the CONTROL: the async METHOD twin of row 1. Already clean
// pre-fix (this file does not touch emitAsyncMethod); included so the lambda
// rows above are read against a same-shape baseline, per the issue's own
// measurement table. 200 * 3 = 600. // per §28.10, §30.1 (issue #3728)
BEZEL_TEST(async_lambda_scope_end_disposal_3728,
           async_method_twin_using_var_control_spec_28_10) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method RunBody(): Task<int> "
        "    using var r := new Cell(1); "
        "    await Task.Delay(1); "
        "    return 1; "
        "  end RunBody; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + await C.RunBody() + 2; end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 4, the `Array<int>` BORROWING LITERAL row from the issue's own
// table. There is no destructor to count (int has none) — the backing
// allocation's fate is observable only through the sanitizer, which is why
// `runAotChecked` (LSan ON) is the whole oracle here: pre-fix the program
// still prints `total=600`, but LSan reports 24000 B / 400 at exit and the
// row fails via `ran=false` ("<did-not-run>"), not via a value mismatch.
// 200 * 3 = 600. // per §28.11, §30.6 (issue #3728)
BEZEL_TEST(async_lambda_scope_end_disposal_3728,
           array_literal_backing_reclaimed_on_normal_edge_spec_28_11) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Run(): Task<int> "
        "    var f := async lambda() => do "
        "      var xs: Array<int> := [10, 20, 30]; "
        "      await Task.Delay(1); "
        "    end; "
        "    await f(); "
        "    return 3; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + await C.Run(); end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 5, `[2 owned]Cell?` — the SECOND root cause this commit fixes:
// `moveOnlyArrayTransferredFn_` was never pinned to the lambda resume fn at
// all (§15.3), so `registerInlineOwnedArrayDisposal`'s
// `if (currentFn_ != moveOnlyArrayTransferredFn_) return true;` guard skipped
// registration entirely — pre-fix `total=600 n=400`, 6400 B / 400 allocs.
// 200 * 3 = 600. // per §15.1.1, §15.3, §30.6 (issue #3728)
BEZEL_TEST(async_lambda_scope_end_disposal_3728,
           inline_owned_array_disposes_both_slots_on_normal_edge_spec_15_1_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Run(): Task<int> "
        "    var f := async lambda() => do "
        "      var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "      await Task.Delay(1); "
        "    end; "
        "    await f(); "
        "    return 3; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + await C.Run(); end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 6, OVER-RELEASE probe 1 — `using var` THROW AFTER AN AWAIT. The
// issue's own text records this edge as already clean pre-fix (the unwind
// path has its own discharge, independent of the block this commit changes);
// this row pins that it STAYS clean and does not become a double free now
// that the normal edge also disposes. 200 * (1 + 1) = 400.
// per §28.3, §28.10, §30.6 (issue #3728)
BEZEL_TEST(async_lambda_scope_end_disposal_3728,
           using_var_throw_after_await_disposes_exactly_once_spec_28_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Run(): Task<int> "
        "    var f := async lambda() => do "
        "      using var r := new Cell(1); "
        "      await Task.Delay(1); "
        "      throw new InvalidOperationException(\"pqr\"); "
        "    end; "
        "    try "
        "      await f(); "
        "      return 0; "
        "    catch "
        "      var e: Exception => do return 1; end "
        "    end try; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + await C.Run(); end; " +
        kTail),
        "total=200 n=0\n");
}

// ── Row 7, OVER-RELEASE probe 2 — `[2 owned]Cell?` THROW AFTER AN AWAIT. The
// dangerous direction the brief calls out explicitly: arming a disposal that
// never ran before means the throw edge must not now run it a SECOND time
// alongside whatever the fault-boundary / unwind machinery already does.
// 200 * (1 + 1) = 400. // per §15.1.1, §15.3, §30.6 (issue #3728)
BEZEL_TEST(async_lambda_scope_end_disposal_3728,
           inline_owned_array_throw_after_await_disposes_exactly_once_spec_15_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Run(): Task<int> "
        "    var f := async lambda() => do "
        "      var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "      await Task.Delay(1); "
        "      throw new InvalidOperationException(\"pqr\"); "
        "    end; "
        "    try "
        "      await f(); "
        "      return 0; "
        "    catch "
        "      var e: Exception => do return 1; end "
        "    end try; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + await C.Run(); end; " +
        kTail),
        "total=200 n=0\n");
}

// ── Row 8, TRANSFERRED OUT: `var b := a` is a whole-array move-only
// transfer (§15.3) — `a`'s disposal must be SKIPPED while `b`'s scope-end
// disposal runs, across an `await` in between (so the two locals are
// restored from the frame on different activations). This is the row that
// proves the new `collectMoveOnlyArrayTransferred` call RE-RUNS the transfer
// analysis over the lambda's own body (rather than merely re-pointing a
// stale token): with a stale set this is a double free, not a leak.
// 200 * 3 = 600. // per §15.3, §30.6 (issue #3728)
BEZEL_TEST(async_lambda_scope_end_disposal_3728,
           inline_owned_array_transferred_out_is_not_double_freed_spec_15_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Run(): Task<int> "
        "    var f := async lambda() => do "
        "      var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "      await Task.Delay(1); "
        "      var b: [2 owned]Cell? := a; "
        "    end; "
        "    await f(); "
        "    return 3; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + await C.Run(); end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 9, ABANDONMENT via the task being dropped: `using var t := f();`
// dropped without an `await` is one of spec §30.1/§30.5's named abandonment
// idioms ("The shapes that carry a live residual are the abandonment idioms
// — `using var t := call();` dropped without an await, and
// `Task.FireAndForget(call())`"). This row uses the deterministic form: `f`'s
// body has NO internal await, so Resume completes within the initial
// synchronous drive (§30.1) and the returned Task is already SETTLED at the
// point it is dropped — no scheduler timing is involved. The body's own
// scope-end disposal (this commit's fix) is independent of what the caller
// later does with the returned Task handle: it fires when the RESUME
// function itself falls off its own end, not when the caller touches the
// Task. 200 * 1 = 200. // per §17.13, §28.10, §30.1, §30.5 (issue #3728)
// NOTE (fix #3835, §17.13 v0.288, axiom A6): the binding is NAMED (`t`, unread)
// rather than anonymous (`_`) — v0.288 made the anonymous `using var _` form
// a compile error over a Task-family initializer (nothing can await an
// unnamed binding, so it is the discard's abandonment deferred to scope end).
// This row's own subject is the scope-end abandonment semantics, which the
// named form exercises identically; only the anonymous SPELLING is banned.
BEZEL_TEST(async_lambda_scope_end_disposal_3728,
           using_var_disposes_before_the_returned_task_is_abandoned_spec_30_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Run(): Task<int> "
        "    var f := async lambda() => do "
        "      using var r := new Cell(1); "
        "    end; "
        "    using var t := f(); "
        "    return 1; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + await C.Run(); end; " +
        kTail),
        "total=200 n=0\n");
}

// ── Row 10, the §6b COMBINED row: `using var`, a borrowing `Array<int>`
// literal, AND a `[2 owned]Cell?` inline array, all in the SAME async lambda
// body, all crossing the SAME `await`. Proves the three categories this
// block registers are each disposed EXACTLY once together, not just in
// isolation. 200 * 3 = 600. // per §28.10, §28.11, §15.1.1, §30.6 (issue #3728)
BEZEL_TEST(async_lambda_scope_end_disposal_3728,
           combined_using_var_array_literal_and_inline_owned_array_spec_28_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Run(): Task<int> "
        "    var f := async lambda() => do "
        "      using var r := new Cell(1); "
        "      var xs: Array<int> := [10, 20, 30]; "
        "      var a: [2 owned]Cell? := [new Cell(2), new Cell(3)]; "
        "      await Task.Delay(1); "
        "    end; "
        "    await f(); "
        "    return 3; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + await C.Run(); end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 11, the SAVE/RESTORE half: an ENCLOSING async method declares its
// OWN `[2 owned]Cell?` local, invokes an async lambda that ALSO declares one,
// and continues past the `await f();` to use its own array afterward. Without
// this commit's save/restore in `lowerLambda` (LlvmBackend_Exprs2.cpp) around
// `emitAsyncLambda`, the enclosing method's `moveOnlyArrayTransferredFn_`
// would be left pointing at the LAMBDA's resume fn after the lambda returns,
// so the enclosing method's OWN array would silently skip its disposal — a
// leak in the outer body traded for the inner fix, the exact hazard
// LANE_RULES §8 exists to prevent. Each call returns 3 + 3 + 4 = 10
// (the lambda's own 3, plus both of the enclosing method's OWN cells --
// reading only ys[0] would not prove ys[1] survived too); 200 * 10 = 2000.
// per §15.3, §28.3, §30.6 (issue #3728)
BEZEL_TEST(async_lambda_scope_end_disposal_3728,
           enclosing_async_method_restores_its_own_transfer_set_after_the_lambda_spec_15_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Run(): Task<int> "
        "    var f := async lambda() => do "
        "      var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "      await Task.Delay(1); "
        "    end; "
        "    await f(); "
        "    var ys: [2 owned]Cell? := [new Cell(3), new Cell(4)]; "
        "    return 3 + ys[0]!.V + ys[1]!.V; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + await C.Run(); end; " +
        kTail),
        "total=2000 n=0\n");
}

// ── Row 12, the EXPR-BODY smoke test: `async lambda(v) => v + 1` (no
// `do…end`, so `lowerExpr(*lam.exprBody)` is the whole body — no LocalDecl
// statement exists to register into `disposeStack_.back()`, so this branch's
// new `emitDisposals` call is provably a no-op here; the row exists to prove
// it does not disturb the completing VALUE (`v`) or crash across a genuine
// suspend). 200 * 2 = 400. // per §30.1, §30.6 (issue #3728)
BEZEL_TEST(async_lambda_scope_end_disposal_3728,
           expr_bodied_lambda_completes_correctly_across_an_await_spec_30_6) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Bump(v: int): Task<int> "
        "    var f := async lambda(x: int) => await C.Add1(x); "
        "    return await f(v); "
        "  end Bump; "
        "  public static async method Add1(x: int): Task<int> "
        "    await Task.Delay(1); "
        "    return x + 1; "
        "  end Add1; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + await C.Bump(1); end; " +
        kTail),
        "total=400 n=0\n");
}
