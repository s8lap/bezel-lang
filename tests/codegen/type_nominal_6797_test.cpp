// fix #6797 control (spec §22.6 "Overloading", stage 3: applicability under the
// §5.1 implicit conversions; stdlib §2.13 "The `Type` class"). Under the nominal
// rule an int argument is not applicable to `Take(t: Type)`, so `Take(i: int)`
// is the candidate that remains and the program returns 2. Measured with the
// probe `x_overload_type_vs_int.bzl`: the base compiler also returns 2, so the
// fix does not change the pick. This row pins the observable pick at runtime,
// which sema-only rows cannot see.
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

BEZEL_TEST(type_nominal_6797_codegen, overload_int_chosen_over_nominal_type_spec_22_6) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "public class P "
        "  public static method Take(t: Type): int return 1; end Take; "
        "  public static method Take(i: int): int return 2; end Take; "
        "end P; "
        "public class Program "
        "  public static method Main(): int "
        "    return P.Take(5); "
        "  end Main; "
        "end Program; "),
        2);
}
