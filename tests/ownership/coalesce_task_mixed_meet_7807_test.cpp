// Regression test for #7807 -- `using var u := t ?? W();` over a named owned
// `Task?` local compiled (warning only) and the program HUNG at run time: the
// unmarked `t` and the fresh `W()` are a mixed `??` meet.
//
// Language spec §28.3 (v0.357), producer table `a ?? b` row and K7.3 / K3.13:
// "mixed = rejected -- equalize the arms first. A named owned binding written
// as an arm is an unmarked read and therefore a borrow arm." §28.10 (using
// var over a mixed meet). The check required a class/interface-typed borrow
// arm, which a `Task` is not.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// Issue repro, verbatim.
BEZEL_TEST(coalesce_task_mixed_meet_7807, using_var_task_mixed_coalesce_rejected_spec_28_3) {
    JitResult jr = runAot(R"BZL(module M; import System.Threading;
public class C
  public static async method W(): Task
  end W;
  public static async method Main(): Task<int>
    var t: Task? := W();
    using var u := t ?? W();
    await u;
    Console.WriteLine("done");
    return 0;
  end Main;
end C;
)BZL");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(jr, "mix an owned value with a borrowed arm"));
}
