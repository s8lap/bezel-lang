// #6761 (stdlib §2.2, v0.340): Math.Round lowered to llvm.roundeven, which
// becomes a libm call to `roundeven`; musl 1.2.5 (the static release bundle)
// has no such symbol, so every Math.Round program failed to link there. The
// lowering now uses llvm.rint (same round-half-to-even in the default FP
// environment, which Bezel cannot change; `rint` exists in glibc and musl).
// Rows pin the exact banker's semantics of both overloads, exact stdout.
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

BEZEL_TEST(math_round_6761, one_arg_halves_and_edges_spec_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(R"(
module M;
class C
  public static method Main(): int
    var inf := 1.0 / 0.0;
    var nan := inf - inf;
    var nz := 0.0 * (0.0 - 1.0);
    Console.WriteLine(Math.Round(0.5));
    Console.WriteLine(Math.Round(1.5));
    Console.WriteLine(Math.Round(2.5));
    Console.WriteLine(Math.Round(0.0 - 0.5));
    Console.WriteLine(Math.Round(0.0 - 2.5));
    Console.WriteLine(Math.Round(0.0 - 3.5));
    Console.WriteLine(Math.Round(4503599627370497.0));
    Console.WriteLine(Math.Round(inf));
    Console.WriteLine(Math.Round(0.0 - inf));
    Console.WriteLine(Math.Round(nan));
    Console.WriteLine(1.0 / Math.Round(nz));
    Console.WriteLine(1.0 / Math.Round(0.0 - 0.4));
    return 0;
  end Main;
end C;
)"), "0\n2\n2\n-0\n-2\n-4\n4503599627370497\ninf\n-inf\nnan\n-inf\n-inf");
}

BEZEL_TEST(math_round_6761, digits_overload_spec_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(R"(
module M;
class C
  public static method Main(): int
    Console.WriteLine(Math.Round(0.125, 2));
    Console.WriteLine(Math.Round(0.375, 2));
    Console.WriteLine(Math.Round(2.5, 0));
    Console.WriteLine(Math.Round(3.5, 0));
    Console.WriteLine(Math.Round(0.0 - 2.5, 0));
    Console.WriteLine(Math.Round(3.14159, 2));
    return 0;
  end Main;
end C;
)"), "0.12\n0.38\n2\n4\n-2\n3.14");
}
