// fix #9043 / §28.3 *Captured owned bindings* + §31.1 *The throw operand*
// (spec v0.359): a CAPTURED owned binding thrown through a `case ... of`
// EXPRESSION operand inside a closure. Sibling of #8958 (conditional operand):
// the case arms are the operand's values exactly as a conditional's branches
// are, so an arm naming a captured owned binding is an owned-consuming use of
// it (double dispose / heap-use-after-free at run time, MEASURED under ASan on
// the base). The diagnostic is the direct form's own. Admitting twins (fresh
// arms with `c` only in the scrutinee or a guard; an arm that calls a method
// on `c`) are corpus candidates, not rows.
//
// per §28.3, §31.1, v0.359, #9043

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

BEZEL_TEST(captured_owned_case_throw_9043, case_operand_of_captured_owned_local_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var c := new R(); var n := 1;
    var f := lambda(): int => do
      throw (case n of 1 => c, _ => c);
    end;
    try f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "cannot throw captured owned binding `c` inside a closure"));
}

BEZEL_TEST(captured_owned_case_throw_9043, case_arm_nested_in_conditional_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var c := new R(); var n := 1;
    var f := lambda(): int => do
      throw (if n > 0 then (case n of 1 => c, _ => new R()) else new R());
    end;
    try f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "cannot throw captured owned binding `c` inside a closure"));
}

BEZEL_TEST(captured_owned_case_throw_9043, case_operand_of_captured_owned_parameter_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Thrower(owned c: R): int
    var n := 1;
    var f := lambda(): int => do
      throw (case n of 1 => c, _ => c);
    end;
    try f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Thrower;
  public static method Main(): int
    _ := Thrower(new R());
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "cannot throw captured owned binding `c` inside a closure"));
}

}  // namespace
