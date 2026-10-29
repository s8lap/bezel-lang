// Regression tests for #7564 -- interpolating a null nullable function value
// (`((int) -> int)?`) dereferenced the null closure cell (SEGV, read at 0x8).
//
// Language spec §8.5 (v0.350), verbatim: "a `null` value of a nullable-typed
// hole renders as the text `null` with no dispatch"; §18.5 holes format by
// §8.5. A non-null function value renders as its formed type (§8.5 / §27.7.1,
// pinned by #5943): `(int) -> int`. The loop row runs on runAotChecked
// (ASan + LSan), 100 iterations.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;


BEZEL_TEST(nullable_function_render_7564,
           null_and_present_function_interpolation_spec_8_5) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var f0: ((int) -> int)? := null;\n"
        "    var f1: ((int) -> int)? := lambda(x: int) => x;\n"
        "    var g: (int) -> int := lambda(x: int) => x;\n"
        "    Console.WriteLine($\"<{f0}>\");\n"
        "    Console.WriteLine($\"<{f1}>\");\n"
        "    Console.WriteLine($\"<{g}>\");\n"
        "    Console.WriteLine(g);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "<null>\n<(int) -> int>\n<(int) -> int>\n(int) -> int\n");
}

BEZEL_TEST(nullable_function_render_7564,
           nullable_function_interpolation_loop_100) {
    JitResult r = runAotChecked(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 0..<100 do\n"
        "      var a: ((int) -> int)? := lambda(x: int) => x + i;\n"
        "      var b: ((int) -> int)? := null;\n"
        "      var s := $\"{a}/{b}\";\n"
        "      if s = \"(int) -> int/null\" then total := total + 1; end;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}
