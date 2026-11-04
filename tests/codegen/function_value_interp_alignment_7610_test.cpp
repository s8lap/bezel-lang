// Regression tests for #7610 -- an alignment on a function-valued hole
// (`{g,14}`) was dropped, and so was the alignment of a null
// `((int) -> int)?` hole.
//
// Language spec §18.5 (v0.355), "Nullable holes": "The alignment applies to
// the rendered text `null` exactly as to any other rendered text (it
// participates in field padding)"; a function value renders as its formed type
// (§8.5 / §27.7.1): `(int) -> int`, 12 characters.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

BEZEL_TEST(function_value_interp_alignment_7610,
           alignment_applies_to_function_and_null_function_holes_spec_18_5) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var g: (int) -> int := lambda(x: int) => x;\n"
        "    var f1: ((int) -> int)? := lambda(x: int) => x;\n"
        "    var f0: ((int) -> int)? := null;\n"
        "    var n := 42;\n"
        "    Console.WriteLine($\"[{g,14}]\");\n"
        "    Console.WriteLine($\"[{g,-14}]\");\n"
        "    Console.WriteLine($\"[{f1,14}]\");\n"
        "    Console.WriteLine($\"[{f0,6}]\");\n"
        "    Console.WriteLine($\"[{f0,-6}]\");\n"
        "    Console.WriteLine($\"[{n,6}]\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "[  (int) -> int]\n"
                    "[(int) -> int  ]\n"
                    "[  (int) -> int]\n"
                    "[  null]\n"
                    "[null  ]\n"
                    "[    42]\n");
}
