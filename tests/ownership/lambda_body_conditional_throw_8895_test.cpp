// fix #8895 / §31.1 *The throw operand* (spec v0.359): a COMPOSITE operand of
// a closure's `throw` — `throw (if c then b else b2)`.
//
// §31.1: "The operand is consumed as an owned value under the ownership rules
// below, so a *borrowed* class reference is not a valid operand." The rule
// names no scope. The METHOD body rejects the shape — its classifier
// (`ownedArgSourceIsBorrow`) recurses into a conditional expression's arms
// and answers "borrow" iff EITHER arm is a borrow (fix #4008) — and draws
// "the thrown value must be an OWNED exception" at the operand.
//
// A closure body never asked: `checkLambdaBodyThrowOperandNotBorrowed`
// classifies a BARE IDENTIFIER operand only (fix #8802's own local, fix
// #8859's captured one), so a conditional's arms were never asked the borrow
// question. The program compiled (rc=0) and core-dumped at run time
// (MEASURED on r30-c1 @ af51cca67: rc=139 — the borrow's owner and the
// runtime both disposed the instance).
//
// Each row is red on the base, asserts the specific diagnostic AND its
// location (the operand of the `throw`), and is the issue's own repro shape
// (row 1 verbatim). The admitting twins — the conditional over two OWNED
// arms, and the METHOD-body twin of row 1 — are corpus candidates, not rows:
// they are green on both arms.
//
// per §31.1, v0.359, #8895

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

// ROW 1 — the issue's own repro: a closure throws a CONDITIONAL whose arms
// are its OWN borrowed locals (the operand is at line 11, column 13).
BEZEL_TEST(lambda_body_conditional_throw_8895,
           block_closure_throw_conditional_of_own_borrowed_locals_rejected_spec_31_1_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var f := lambda(): int => do
      var c := new R(); var b := c; var b2 := c; var n := 1;
      throw (if n > 0 then b else b2);
    end;
    try f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":11:13: the thrown value must be an OWNED exception"));
}

// ROW 2 — the MIXED arms: the method arm's classifier answers "borrow" iff
// EITHER arm is a borrow (fix #4008's own OR rule), so one borrowed arm is
// enough (the operand is at line 11, column 13).
BEZEL_TEST(lambda_body_conditional_throw_8895,
           block_closure_throw_conditional_with_one_borrowed_arm_rejected_spec_31_1_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var f := lambda(): int => do
      var c := new R(); var b := c; var n := 1;
      throw (if n > 0 then b else c);
    end;
    try f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":11:13: the thrown value must be an OWNED exception"));
}

// ROW 3 — the CAPTURED spelling: the borrowed locals are declared in an
// ENCLOSING closure's body and thrown by a NESTED closure (fix #8859's
// captured-local case, in its conditional spelling). The operand is at
// line 12, column 15.
BEZEL_TEST(lambda_body_conditional_throw_8895,
           nested_closure_throw_conditional_of_enclosing_closures_borrowed_locals_rejected_spec_31_1_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var f := lambda(): int => do
      var c := new R(); var b := c; var b2 := c; var n := 1;
      var g := lambda(): int => do
        throw (if n > 0 then b else b2);
      end;
      _ := g();
      return 0;
    end;
    try _ := f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":12:15: the thrown value must be an OWNED exception"));
}

}  // namespace
