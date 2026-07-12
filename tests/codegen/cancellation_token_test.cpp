// Codegen for CancellationToken / CancellationTokenSource (stdlib §6.1 /
// language spec §30.8).
//
// CancellationToken is a builtin record holding a borrowed pointer to a
// cancellation state cell (a thin handle).  CancellationTokenSource is a
// disposable owned class: Cancel() sets the cancellation flag; tokens derived
// from it then observe IsCancellationRequested == true.
//
// Spec reference: Bezel_Standard_Library_0_130.md §6.1,
//                 Bezel_Language_Spec_0_130.md §30.8.
//
// Deferred (not tested here, with reasons):
//   - Register(callback): owned Subscription — needs closure-list infra (a
//     per-source list of callbacks fired on Cancel).  Deferred pending the
//     async / callback-list milestone.
//   - WithCancelAfter / CancelAfter: needs timer/scheduler runtime support.
//     Deferred pending the async milestone.
//
// Each test cites the stdlib section and the spec-stated behaviour it verifies.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

} // namespace

// stdlib §6.1: new CancellationTokenSource() / .Token / IsCancellationRequested.
// Before Cancel(): IsCancellationRequested on the token is false (returns 0).
BEZEL_TEST(codegen_cancellation_6_1, token_is_not_cancelled_before_cancel) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var cts := new CancellationTokenSource(); "
        "var tok := cts.Token; "
        "if tok.IsCancellationRequested then return 1; end; "
        "return 0; ")), 0);
}

// stdlib §6.1: Cancel() sets IsCancellationRequested on the source.
BEZEL_TEST(codegen_cancellation_6_1, source_is_cancelled_after_cancel) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var cts := new CancellationTokenSource(); "
        "cts.Cancel(); "
        "if cts.IsCancellationRequested then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §6.1: Cancel() propagates to the derived token.
// After source.Cancel(), the token's IsCancellationRequested is also true.
BEZEL_TEST(codegen_cancellation_6_1, token_reflects_cancel_on_source) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var cts := new CancellationTokenSource(); "
        "var tok := cts.Token; "
        "cts.Cancel(); "
        "if tok.IsCancellationRequested then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §6.1: CancellationToken.None — the "never-cancels" sentinel.
// IsCancellationRequested on None is always false (null ptr).
BEZEL_TEST(codegen_cancellation_6_1, none_token_is_never_cancelled) {
    BEZEL_EXPECT_EQ(run(prog(
        "var tok := CancellationToken.None; "
        "if tok.IsCancellationRequested then return 1; end; "
        "return 0; ")), 0);
}

// stdlib §6.1: token copy observes the same source cancellation.
// A copy of the token (passed to a helper method) sees the same flag.
BEZEL_TEST(codegen_cancellation_6_1, copied_token_observes_same_source) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Helper "
        "  public static method Check(tok: CancellationToken): bool "
        "    return tok.IsCancellationRequested; "
        "  end Check; "
        "end Helper; "
        "class C public static method Main(): int "
        "  using var cts := new CancellationTokenSource(); "
        "  var tok := cts.Token; "
        "  cts.Cancel(); "
        "  if Helper.Check(tok) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// stdlib §6.1 / spec §30.8: ThrowIfCancellationRequested() on a non-cancelled
// token is a no-op (no exception thrown).
BEZEL_TEST(codegen_cancellation_6_1, throw_if_cancelled_noop_when_not_cancelled) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var cts := new CancellationTokenSource(); "
        "var tok := cts.Token; "
        "tok.ThrowIfCancellationRequested(); "
        "return 7; ")), 7);
}

// stdlib §6.1 / spec §30.8: ThrowIfCancellationRequested() on a CANCELLED token
// throws OperationCanceledException, which is catchable.
BEZEL_TEST(codegen_cancellation_6_1, throw_if_cancelled_throws_oce_after_cancel) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var cts := new CancellationTokenSource(); "
        "var tok := cts.Token; "
        "cts.Cancel(); "
        "try "
        "  tok.ThrowIfCancellationRequested(); "
        "  return 0; "
        "catch OperationCanceledException => return 42; "
        "end; "
        "return 0; ")), 42);
}

// stdlib §6.1 / spec §30.8: ThrowIfCancellationRequested on CancellationToken.None
// is always a no-op.
BEZEL_TEST(codegen_cancellation_6_1, throw_if_cancelled_noop_for_none) {
    BEZEL_EXPECT_EQ(run(prog(
        "var tok := CancellationToken.None; "
        "tok.ThrowIfCancellationRequested(); "
        "return 5; ")), 5);
}

// stdlib §6.1: explicit Dispose on source (not using-var).
// After Dispose the source is freed per the spec destructor note.
BEZEL_TEST(codegen_cancellation_6_1, explicit_dispose_source) {
    BEZEL_EXPECT_EQ(run(prog(
        "var cts := new CancellationTokenSource(); "
        "cts.Dispose(); "
        "return 3; ")), 3);
}

// ===========================================================================
// Bug #463: CancellationTokenSource.WithCancelAfter() return type erasure.
//
// Root cause: sema did not assign a type to the WithCancelAfter() CallExpr,
// so `var cts := CancellationTokenSource.WithCancelAfter(...)` inferred `cts`
// as an unresolved type (erased to i32 by codegen).  Accessing `cts.Token`
// then lowered the token as `i32` instead of `ptr`, causing MALFORMED IR when
// passing the token to Task.Delay(int, CancellationToken).
//
// Fix: sema now assigns return type CancellationTokenSource to WithCancelAfter
// (the same TypeRef shape used by `new CancellationTokenSource()`), so
// `cts.Token` types correctly as CancellationToken (a ptr-wide value, §6.1).
//
// Timer note: WithCancelAfter schedules cancellation after a delay.  As of #1413
// the timer IS implemented (bz_cts_cancel_after on the #1378 BzTimer; the firing
// behaviour is covered in cts_register_timer_1413_test.cpp).  These SYNCHRONOUS
// #463 layout tests still take the "not cancelled" path and return 0: the token is
// checked immediately (well before the 10 ms deadline), and the `using var` source
// is disposed at scope end, which cancels the still-pending timer entry — so no
// cancellation is observed within the test.
//
// Spec reference: stdlib §6.1 (WithCancelAfter signature and return type),
//                 §6.1 (CancellationToken — a single pointer word).
// ===========================================================================

// #463 / §6.1: WithCancelAfter() compiles cleanly and the source's token
// has the right layout (ptr-wide).  IsCancellationRequested is false when checked
// immediately (well before the 10 ms deadline — see the timer note above).
// Regression for `new CancellationTokenSource()` path also verified.
BEZEL_TEST(cts_withcancelafter_token_typed_463,
           withcancelafter_token_is_ptr_not_erased) {
    BEZEL_EXPECT_EQ(run(prog(
        // §6.1: WithCancelAfter(TimeSpan) → owned CancellationTokenSource.
        "using var cts := CancellationTokenSource.WithCancelAfter("
        "    TimeSpan.FromMilliseconds(10)); "
        "var token := cts.Token; "
        // Checked immediately — before the 10 ms deadline — so not yet cancelled.
        "if token.IsCancellationRequested then return 1; end; "
        "return 0; ")),
        0);  // synchronous check precedes the deadline — cancellation not observed
}

// #463 / §6.1: Cancel() on a WithCancelAfter-created source works (the source
// is a real CancellationTokenSource, not an i32 phantom).
BEZEL_TEST(cts_withcancelafter_token_typed_463,
           withcancelafter_source_cancel_propagates_to_token) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var cts := CancellationTokenSource.WithCancelAfter("
        "    TimeSpan.FromMilliseconds(100)); "
        "var tok := cts.Token; "
        // Explicitly cancel the source (simulating what the timer would do).
        "cts.Cancel(); "
        // Token must now reflect cancellation.
        "if tok.IsCancellationRequested then return 1; end; "
        "return 0; ")),
        1);
}

// #463 / §6.1: Regression — `new CancellationTokenSource()` path still works.
// Ensures the fix did not disturb the working path.
BEZEL_TEST(cts_withcancelafter_token_typed_463,
           new_cts_regression_still_works) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var cts := new CancellationTokenSource(); "
        "var tok := cts.Token; "
        "cts.Cancel(); "
        "if tok.IsCancellationRequested then return 1; end; "
        "return 0; ")),
        1);
}

// ===========================================================================
// #1025 / §6.1: a CHAINED-receiver cancellation instance call — calling the
// method directly on `source.Token` (a MemberAccessExpr, not a bound
// identifier) rather than `var t := source.Token; t.M()`.
//
// Root cause: tryLowerBuiltinClassCall returned early at `if (!id) return
// nullptr` (the static-call guard requiring an IdentifierExpr receiver) BEFORE
// reaching the CancellationToken/CancellationTokenSource instance handlers, so
// the chained form silently lowered to a no-op — `cts.Token.ThrowIfCancellation
// Requested()` never threw on a cancelled token (a silent correctness hole).
// Fix: the instance handlers moved before that guard (they dispatch on the
// receiver's exprType, accepting any receiver expression).
// ===========================================================================

// #1025: chained ThrowIfCancellationRequested() on a CANCELLED token throws
// OperationCanceledException — before the fix it silently did nothing (returned
// the no-throw path). Mirrors throw_if_cancelled_throws_oce_after_cancel but
// WITHOUT binding the token to a local first.
BEZEL_TEST(codegen_cancellation_1025, chained_throw_if_cancelled_throws_after_cancel) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var cts := new CancellationTokenSource(); "
        "cts.Cancel(); "
        "try "
        "  cts.Token.ThrowIfCancellationRequested(); "
        "  return 0; "
        "catch OperationCanceledException => return 42; "
        "end; "
        "return 0; ")), 42);
}

// #1025: chained ThrowIfCancellationRequested() on a NOT-cancelled token is a
// no-op (the receiver is evaluated for effect but nothing is thrown).
BEZEL_TEST(codegen_cancellation_1025, chained_throw_if_cancelled_noop_when_not_cancelled) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var cts := new CancellationTokenSource(); "
        "cts.Token.ThrowIfCancellationRequested(); "
        "return 7; ")), 7);
}

// #1025: chained IsCancellationRequested property read on `cts.Token` after
// Cancel() — control that the instance-handler move preserves the property path.
BEZEL_TEST(codegen_cancellation_1025, chained_token_is_cancellation_requested_after_cancel) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var cts := new CancellationTokenSource(); "
        "cts.Cancel(); "
        "if cts.Token.IsCancellationRequested then return 1; end; "
        "return 0; ")), 1);
}
