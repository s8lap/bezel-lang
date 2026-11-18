// fix #8859 / §31.1 *The throw operand* (spec v0.359): `throw b` in a NESTED
// lambda, where `b` is a BORROWED local declared in the ENCLOSING lambda's
// body.
//
// §31.1: "The operand is consumed as an owned value under the ownership rules
// below, so a *borrowed* class reference is not a valid operand." The rule
// names no scope. The METHOD body rejects a borrowed operand ("the thrown
// value must be an OWNED exception"); a closure body never reaches that arm
// (ownership's statement walks do not enter a lambda body), so a lambda's own
// local is tracked in NO frame.
//
// #8802 closed the ONE-LEVEL case (a lambda throwing its OWN borrowed local).
// The remaining hole is the CAPTURED case: `b` is declared in the ENCLOSING
// lambda's body and CAPTURED by the throwing (inner) lambda. #8781's arm
// cannot cover it — it classifies a captured borrowed binding through the
// METHOD frame's `states_`, and an outer-lambda local has no entry in any
// frame. MEASURED on r30-c1 @ 3577f70b8: the program's own repro compiled
// (rc=0) and core-dumped at run time (rc=139, 50/50 runs).
//
// Each row is red on the base, asserts the specific diagnostic AND its
// location (the operand of the `throw`), and is the issue's own repro shape
// (row 1 verbatim). The admitting twins — the enclosing lambda's OWN owned
// local, a fresh `throw new`, the closure's own `owned` parameter, the
// one-level case, and the METHOD-body twin — are corpus candidates, not rows:
// they are green on both arms.
//
// per §31.1, v0.359, #8859

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

const char* kR = R"(
module M;
public class R: Exception
  public constructor() end constructor;
end R;
)";

// ROW 1 — the issue's own repro: a NESTED closure that throws a borrowed
// local declared in the ENCLOSING closure's body (the operand `b` is at line
// 11, column 42).
BEZEL_TEST(nested_lambda_captured_borrow_throw_8859,
           nested_closure_throw_enclosing_closures_borrowed_local_rejected_spec_31_1_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var f := lambda(): int => do
      var c := new R(); var b := c;
      var g := lambda(): int => do throw b; end;
      g();
      return 0;
    end;
    try f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":11:42: the thrown value must be an OWNED exception"));
}

// ROW 2 — the same shape with ONE more level of nesting: the innermost
// closure throws a borrowed local of the OUTERMOST closure's body, so the
// binding's owner is not the closure that throws it and not the closure that
// encloses the thrower (the operand `b` is at line 12, column 44).
BEZEL_TEST(nested_lambda_captured_borrow_throw_8859,
           three_level_nested_closure_throw_outermost_closures_borrowed_local_rejected_spec_31_1_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var f := lambda(): int => do
      var c := new R(); var b := c;
      var g := lambda(): int => do
        var h := lambda(): int => do throw b; end;
        h();
        return 0;
      end;
      g();
      return 0;
    end;
    try f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":12:44: the thrown value must be an OWNED exception"));
}

}  // namespace
