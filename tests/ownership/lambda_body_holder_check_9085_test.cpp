// Regression tests for #9085 -- a LAMBDA body skipped the §28.2 "Use after a
// dominating explicit disposal" HOLDER check (#7435 / #7441): an alias or a
// closure capture of a lambda-local that was discarded or `Dispose()`d was read
// afterwards and used freed memory, while the byte-identical METHOD twin was
// rejected at compile time.
//
//   var f := lambda(flag: bool): int => do
//     var x := new Res(1);
//     var y := x;                    // y is an ALIAS of x: a local borrow
//     if flag then _ := x; return y.tag; end;   // the discard DOMINATES the use
//     return y.tag;
//   end;
//
// Language spec §28.2 (v0.359) "Use after a dominating explicit disposal":
//   "The same error reaches a use through a local borrow of the variable's
//    value: where a local holds a borrow of that value -- an alias
//    (`var b := a;`, `const b := a;`), ... a closure that captures such a
//    holder -- taken while the variable held that value, and every value in
//    the holder's source set (K2.2) has been so disposed on every path to the
//    use, a use of the holder -- a read or member access through it, passing
//    it, re-borrowing it, invoking the closure, ... -- is the same compile
//    error". And: "A closure that captures the variable itself, rather than a
//    holder of its value, reads the variable when it is invoked (§27.5): its
//    invocation after the kill is a use of the variable".
//    §28.3.10 (v0.359): "The rule is uniform across body kinds."
//
// Root cause: the ownership pass runs the lambda body through a separate
// pre-pass walk (`applyUsingVarBorrowPinInLambdaStmt`) that never called
// `bindHeldBorrows` -- the writer of `LocalOwnership::heldBorrows` -- so no
// lambda-body local was ever a holder and `checkUseThroughDisposedBorrowHolder`
// found nothing to report. A store into a source variable likewise never
// reached `dropHeldBorrowsOnSourceStore`. The closure-over-the-variable
// sentence's check (`checkTransitiveCapturesDisposed`) is asked from the method
// body's `scanCapturedOwnedConsumeExpr`, which never reaches a lambda body's own
// statements, so the invocation of a lambda-local closure was never asked either.
//
// The rows are COMPILE-TIME (`parseAndCheckOwnership`): the defect is a
// compile-time over-acceptance. Every REJECT row is RED on the base and the
// ADMIT row is a base-green control.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

bool hasError(const OwnershipResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

const std::string kHead =
    "module Probe;\n"
    "public class Res\n"
    "  public tag: int;\n"
    "  public constructor(t: int) this.tag := t; end constructor;\n"
    "end Res;\n"
    "public class C\n"
    "  public static method F(flag: bool): int\n"
    "    var f := lambda(flag: bool): int => do\n";

const std::string kTail =
    "    end;\n"
    "    return f(flag);\n"
    "  end F;\n"
    "  public static method Main(): int\n"
    "    return F(true);\n"
    "  end Main;\n"
    "end C;\n";

std::string lambdaProg(const std::string& body) { return kHead + body + kTail; }

}  // namespace

// REJECT: alias, discard spelling, conditional kill that dominates the use in
// its own arm (probe j4).
BEZEL_TEST(ownership_lambda_body_holder_check_9085,
           alias_discard_then_read_alias_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var x := new Res(1);\n"
        "      var y := x;\n"
        "      if flag then _ := x; return y.tag; end;\n"
        "      return y.tag;\n"));
    BEZEL_EXPECT_TRUE(hasError(r, "use of `y` after a dominating explicit "
                                  "Dispose() of `x`"));
}

// REJECT: alias, discard spelling, unconditional (probe j5).
BEZEL_TEST(ownership_lambda_body_holder_check_9085,
           alias_unconditional_discard_then_read_alias_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var x := new Res(1);\n"
        "      var y := x;\n"
        "      _ := x;\n"
        "      return y.tag;\n"));
    BEZEL_EXPECT_TRUE(hasError(r, "use of `y` after a dominating explicit "
                                  "Dispose() of `x`"));
}

// REJECT: alias, explicit `Dispose()` spelling (probe j4d).
BEZEL_TEST(ownership_lambda_body_holder_check_9085,
           alias_dispose_then_read_alias_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var x := new Res(1);\n"
        "      var y := x;\n"
        "      if flag then x.Dispose(); return y.tag; end;\n"
        "      return y.tag;\n"));
    BEZEL_EXPECT_TRUE(hasError(r, "use of `y` after a dominating explicit "
                                  "Dispose() of `x`"));
}

// REJECT: the `const` alias spelling (§28.2 names both).
BEZEL_TEST(ownership_lambda_body_holder_check_9085,
           const_alias_discard_then_read_alias_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var x := new Res(1);\n"
        "      const y := x;\n"
        "      _ := x;\n"
        "      return y.tag;\n"));
    BEZEL_EXPECT_TRUE(hasError(r, "use of `y` after a dominating explicit "
                                  "Dispose() of `x`"));
}

// REJECT: closure capture of the variable, invoked after the discard (probe j1).
BEZEL_TEST(ownership_lambda_body_holder_check_9085,
           closure_capture_discard_then_invoke_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var x := new Res(1);\n"
        "      var g := lambda(): int => x.tag;\n"
        "      if flag then _ := x; return g(); end;\n"
        "      return g();\n"));
    BEZEL_EXPECT_TRUE(hasError(r, "closure captures `x` which was disposed "
                                  "before this invocation"));
}

// REJECT: closure capture of the variable, invoked after `Dispose()` (probe j1d).
BEZEL_TEST(ownership_lambda_body_holder_check_9085,
           closure_capture_dispose_then_invoke_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var x := new Res(1);\n"
        "      var g := lambda(): int => x.tag;\n"
        "      if flag then x.Dispose(); return g(); end;\n"
        "      return g();\n"));
    BEZEL_EXPECT_TRUE(hasError(r, "closure captures `x` which was disposed "
                                  "before this invocation"));
}

// ADMIT (base-green control): the holder's last use precedes the kill, so
// "a disposal after the borrow's last use changes no verdict" (§28.2).
BEZEL_TEST(ownership_lambda_body_holder_check_9085,
           alias_last_used_before_discard_admitted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var x := new Res(1);\n"
        "      var y := x;\n"
        "      var n := y.tag;\n"
        "      _ := x;\n"
        "      return n;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
