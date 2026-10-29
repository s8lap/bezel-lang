// R37 campaign-2 lane C2-07, issue 6295 part 3 (spec §14.2, §27.6, v0.350;
// stdlib §2.2).
//
// `Math.Min/Max/Clamp<T>` are declared `where T: IComparable<T>`, and §14.2
// says a nullable type does not satisfy an interface constraint. The
// reference `Math.Max<int?>` formed with a NULLABLE primitive passed sema's
// formation check and died in codegen with an internal "could not lower a
// comparison" error located at 0:0. It is now rejected at the formation, at a
// real source location, as the nullable non-primitive case already was.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

BEZEL_TEST(sema_math_max_nullable_reference_6295,
           nullable_type_argument_rejected_at_formation_14_2) {
    const char* refs[][2] = {
        {"(int?, int?) -> int?", "Math.Max<int?>"},
        {"(int?, int?) -> int?", "Math.Min<int?>"},
        {"(int?, int?, int?) -> int?", "Math.Clamp<int?>"},
        {"(double?, double?) -> double?", "Math.Max<double?>"},
    };
    for (auto& ref : refs) {
        auto r = runAot(std::string("module Conf; public class Program "
            "public static method Main() var m: ") + ref[0] + " := " + ref[1] +
            "; end Main; end Program;");
        BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
        BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "IComparable<T>"));
    }
}

// Mirror: the non-nullable references stay formed and invoked.
BEZEL_TEST(sema_math_max_nullable_reference_6295,
           non_nullable_type_arguments_still_formed_14_2) {
    auto r = runAotChecked(
        "module Conf; public class Program "
        "public static method Main() "
        "  var a: (int, int) -> int := Math.Max<int>; "
        "  var b: (double, double) -> double := Math.Min<double>; "
        "  var c: (int, int, int) -> int := Math.Clamp<int>; "
        "  Console.WriteLine(a(3, 5)); "
        "  Console.WriteLine(b(3.5, 1.25)); "
        "  Console.WriteLine(c(9, 1, 5)); "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n1.25\n5\n"));
}
