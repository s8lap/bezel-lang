// Regression tests for #6777 -- interpolating a null `StringBuilder?` rendered
// an empty string.
//
// Language spec §8.5 (v0.350), verbatim: "a `null` value of a nullable-typed
// hole renders as the text `null` with no dispatch"; §18.5 holes format by
// §8.5. Pre-fix: $"{n}" for `n: StringBuilder? := null` printed an empty line.
// The loop row runs on runAotChecked (ASan + LSan), 100 iterations.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;


BEZEL_TEST(stringbuilder_null_render_6777,
           null_stringbuilder_interpolation_renders_null_spec_8_5) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var n: StringBuilder? := null;\n"
        "    Console.WriteLine($\"{n}\");\n"
        "    Console.WriteLine($\"[{n}]\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "null\n[null]\n");
}

BEZEL_TEST(stringbuilder_null_render_6777,
           present_stringbuilder_interpolation_renders_content_loop_100) {
    JitResult r = runAotChecked(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 0..<100 do\n"
        "      var a: StringBuilder? := new StringBuilder(\"hi\");\n"
        "      var b: StringBuilder? := null;\n"
        "      var s := $\"{a}/{b}\";\n"
        "      if s = \"hi/null\" then total := total + 1; end;\n"
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
