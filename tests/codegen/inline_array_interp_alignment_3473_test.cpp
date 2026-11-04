// Regression test for #3473 -- the alignment of an inline-array interpolation
// hole (`{a,10}`) was dropped. Spec §18.5 (v0.355): positive alignment
// right-aligns in a field of that width, negative left-aligns; the hole
// renders as the type's name (§8.5): `[2]int`, 6 characters.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

BEZEL_TEST(inline_array_interp_alignment_3473,
           alignment_applies_to_inline_array_hole_spec_18_5) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a: [2]int := [1, 2];\n"
        "    Console.WriteLine($\"[{a,10}]\");\n"
        "    Console.WriteLine($\"[{a,-10}]\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "[    [2]int]\n[[2]int    ]\n");
}
