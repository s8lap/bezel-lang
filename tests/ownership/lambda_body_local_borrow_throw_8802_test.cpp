// fix #8802 / §31.1 *The throw operand* (spec v0.359): `throw b` of a
// LAMBDA-BODY LOCAL borrow.
//
// §31.1: "The operand is consumed as an owned value under the ownership rules
// below, so a *borrowed* class reference is not a valid operand." The rule
// names no scope. The METHOD body rejects a borrowed operand ("the thrown
// value must be an OWNED exception"); a closure body never reaches that arm
// (ownership's statement walks do not enter a lambda body), so `throw b` of
// the closure's OWN borrowed local (`var c := new R(); var b := c; throw b;`)
// compiled (rc=0) and core-dumped at run time — the borrow's owner and the
// runtime both disposed the instance (MEASURED on r30-c1 @ 80439bb9d: rc=139).
//
// Each row is red on the base, asserts the specific diagnostic AND its
// location (the operand of the `throw`), and is the issue's own repro shape
// (row 1 verbatim). The admitting twins — the closure's own OWNED local, a
// fresh `throw new`, the closure's own `owned` parameter, a local bound to an
// owned call, and a local REASSIGNED to an owned value before the throw — are
// corpus candidates, not rows: they are green on both arms.
//
// per §31.1, v0.359, #8802

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

// ROW 1 — the issue's own repro: a block-bodied closure that throws its OWN
// borrowed local (the operand `b` is at line 9, column 70).
BEZEL_TEST(lambda_body_local_borrow_throw_8802,
           block_closure_throw_own_borrowed_local_rejected_spec_31_1_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var f := lambda(): int => do var c := new R(); var b := c; throw b; end;
    try f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":9:70: the thrown value must be an OWNED exception"));
}

// ROW 2 — the `!` spelling: §17.8 classifies `x!` by its position exactly as
// the bare read `x`, so `throw b!` is the same question (the operand `b` is
// at line 9, column 70).
BEZEL_TEST(lambda_body_local_borrow_throw_8802,
           block_closure_throw_own_borrowed_local_bang_rejected_spec_31_1_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var f := lambda(): int => do var c := new R(); var b := c; throw b!; end;
    try f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":9:70: the thrown value must be an OWNED exception"));
}

// ROW 3 — the `const` spelling: §21.1's runtime `const` is a SEPARATE AST
// node, and a class-typed `const` always holds a borrow (its initializer can
// never be an owned production), so `const b := c; throw b;` is the same
// question (the operand `b` is at line 9, column 72).
BEZEL_TEST(lambda_body_local_borrow_throw_8802,
           block_closure_throw_own_borrowed_const_local_rejected_spec_31_1_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var f := lambda(): int => do var c := new R(); const b := c; throw b; end;
    try f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":9:72: the thrown value must be an OWNED exception"));
}

}  // namespace
