// fix #8781 / §31.1 *The throw operand* (spec v0.359): `throw e` of a BORROWED
// exception inside a closure body.
//
// §31.1: "The operand is consumed as an owned value under the ownership rules
// below, so a *borrowed* class reference is not a valid operand." The rule names
// no scope. The method-body arms reject a borrowed operand ("the thrown value
// must be an OWNED exception"); a closure body never reaches them, so a captured
// borrowed parameter or a captured borrowed local thrown from a closure compiled
// (rc=0) and core-dumped at run time: the caller's owner and the runtime both
// disposed the instance (MEASURED on 227d95a17).
//
// Each row is red on the base, asserts the specific diagnostic AND its location
// (the operand of the `throw`), and is the issue's own repro shape. The admitting
// twins (a closure's own owned local, `throw move` of it, a fresh `throw new`, the
// closure's own `owned` parameter, a bare re-raise) are corpus candidates.
//
// per §31.1, v0.359, #8781

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

// ROW 1 — a block-bodied closure throwing a captured borrowed PARAMETER
// (the `throw e` operand is at line 9, column 40).
BEZEL_TEST(captured_borrowed_throw_in_closure_8781,
           block_closure_throw_captured_borrowed_param_rejected_spec_31_1_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Thrower(e: R): int
    var f := lambda(): int => do throw e; end;
    try f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Thrower;
  public static method Main(): int
    return Thrower(new R());
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":9:40: the thrown value must be an OWNED exception"));
}

// ROW 2 — an expression-bodied closure throwing a captured borrowed LOCAL.
BEZEL_TEST(captured_borrowed_throw_in_closure_8781,
           expression_closure_throw_captured_borrowed_local_rejected_spec_31_1_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var c := new R();
    var b := c;
    var f := lambda(): int => throw b;
    try f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":11:37: the thrown value must be an OWNED exception"));
}

}  // namespace
