// Codegen for CancellationToken / CancellationTokenSource (stdlib §6.1 /
// language spec §30.8).
//
// CancellationToken is a builtin record holding a borrowed pointer to a
// cancellation state cell (a thin handle).  CancellationTokenSource is a
// disposable owned class: Cancel() sets the cancellation flag; tokens derived
// from it then observe IsCancellationRequested == true.
//
// Spec reference: Bezel_Standard_Library_0_129.md §6.1,
//                 Bezel_Language_Spec_0_129.md §30.8.
//
// Deferred (not tested here, with reasons):
//   - Register(callback): owned Subscription — needs closure-list infra (a
//     per-source list of callbacks fired on Cancel).  Deferred pending the
//     async / callback-list milestone.
//   - WithCancelAfter / CancelAfter: needs timer/scheduler runtime support.
//     Deferred pending the async milestone.
//
// Each test cites the stdlib section and the spec-stated behaviour it verifies.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
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
