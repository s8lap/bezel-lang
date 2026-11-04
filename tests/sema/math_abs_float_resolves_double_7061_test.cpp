// Regression test for issue #7061.
//
// stdlib §2.2 (v0.355) declares Math.Abs for int, long, double and decimal
// only; spec §14.5 / changelog v0.290: `Math.Abs(floatVal)` resolves to
// `Abs(double)` because float->double is its one applicable conversion. The
// call is therefore typed `double`: `var g: float := Math.Abs(f)` is refused
// (no implicit double->float, §5.1) and `var g: double := Math.Abs(f)` is
// admitted. Min/Max/Clamp are generic <T> and are not touched.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
std::string prog(const std::string& body) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}
}  // namespace

BEZEL_TEST(math_abs_float_resolves_double_7061, float_target_is_refused) {
    auto r = runAot(prog("var f: float := -2.5f; var g: float := Math.Abs(f); "
                         "Console.WriteLine(g);"));
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("double") != std::string::npos &&
            d.message.find("float") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

BEZEL_TEST(math_abs_float_resolves_double_7061, double_target_negative) {
    auto r = runAotChecked(prog("var f: float := -2.5f; var g: double := Math.Abs(f); "
                                "Console.WriteLine(g);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2.5\n"));
}

BEZEL_TEST(math_abs_float_resolves_double_7061, double_target_negative_zero) {
    auto r = runAotChecked(prog(
        "var f: float := -0.0f; var g: double := Math.Abs(f); "
        "Console.WriteLine(Math.Sign(g)); Console.WriteLine(1.0 / g);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n") + "Infinity\n");
}

BEZEL_TEST(math_abs_float_resolves_double_7061, inferred_var_is_double) {
    auto r = runAotChecked(prog("var f: float := -1.5f; var g := Math.Abs(f); "
                                "var h: double := g; Console.WriteLine(h);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1.5\n"));
}

BEZEL_TEST(math_abs_float_resolves_double_7061, controls_unchanged) {
    auto r = runAotChecked(prog(
        "var i: int := -3; var l: long := -4L; var d: double := -2.5; "
        "var m: decimal := -1.25m; var fl: float := -7.5f; "
        "var a: int := Math.Abs(i); var b: long := Math.Abs(l); "
        "var c: double := Math.Abs(d); var e: decimal := Math.Abs(m); "
        "var mx: float := Math.Max(fl, 1.0f); "
        "Console.WriteLine(a); Console.WriteLine(b); Console.WriteLine(c); "
        "Console.WriteLine(e); Console.WriteLine(mx);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n4\n2.5\n1.25\n1\n"));
}
