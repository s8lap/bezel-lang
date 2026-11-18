// fix #8860 / §31.1 *The throw operand* (spec v0.359): the closure scan's
// ThrowExpr arm reports the SAME diagnostic TWICE for a `throw` in
// expression position.
//
// §31.1: "The operand is consumed as an owned value under the ownership rules
// below, so a *borrowed* class reference is not a valid operand." The rule
// names no scope; the diagnostic that expresses it ("the thrown value must be
// an OWNED exception") is the method arm's own, and it must appear ONCE.
//
// Two INDEPENDENT analyses reach the same operand of a lambda body:
//   1. the closure scan's ThrowExpr arm (`scanCapturedOwnedConsumeExpr`) ->
//      `checkLambdaBodyThrowOperandNotBorrowed` (fix #8802), and
//   2. the lambda-body pre-pass (`applyUsingVarBorrowPinInLambdaStmt` ->
//      `handleLocalDecl` -> `recordMovesIn` -> `handleDischarge` ->
//      `checkThrowOperandNotBorrowed`), which DOES track the lambda's own
//      locals in `states_`.
// Both print byte-identical text at the same offset, so the program was
// rejected with the diagnostic TWICE (MEASURED on r30-c1 @ 3577f70b8, which
// carries #8802: two identical "the thrown value must be an OWNED exception"
// lines at 11:27).
//
// The row is red on the base (two lines) and green with the fix (one line).
// The `throw`-in-STATEMENT spelling is NOT affected (the pre-pass only
// reaches a `throw` inside a LocalDecl initializer) and stays at one
// diagnostic — a corpus candidate, not a row.
//
// per §31.1, v0.359, #8860

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::diagnosticMessageCount;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

const char* kR = R"(
module M;
public class R: Exception
  public constructor() end constructor;
end R;
)";

// ROW 1 — the issue's own repro: `var v := n ?? throw b;` in a lambda, with
// `b` the lambda's own borrowed local (the operand `b` is at line 11,
// column 27). Exactly ONE diagnostic, at that exact place.
BEZEL_TEST(throw_expr_duplicate_diagnostic_8860,
           throw_expression_of_lambda_local_borrow_reported_exactly_once_spec_31_1_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var f := lambda(): int => do
      var c := new R(); var b := c; var n: int? := null;
      var v := n ?? throw b;
      return v;
    end;
    try f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":11:27: the thrown value must be an OWNED exception"));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageCount(r, "the thrown value must be an OWNED exception") ==
        1);
}

}  // namespace
