// Regression test for issue #6907 (§14.1 / §8.3 / §2.8.2 clause 2, v0.340).
//
// `x.GetHashCode()` on a bare `T` receiver inside an imported generic method
// returned 0 where the same-module instantiation returns 5. The defining
// module's template body (compiled against the `int` placeholder, the one
// binding the #1943 / #6779 / #6818 gates let cross the boundary) had no
// dispatch for a `T` receiver without an active substitution, and degraded the
// local to a zero slot (warnings #2477 / #2434 at the LIBRARY compile). The
// placeholder is `int` there, so the template now hashes it as one (§8.3: an
// `int`'s hash is its value).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;

namespace {

const char* kLib =
    "module L2;\n"
    "public static class G2\n"
    "  public static method Hash2<T>(x: T): int\n"
    "    var h := x.GetHashCode();\n"
    "    return h;\n"
    "  end Hash2;\n"
    "  public static method Hash3<T>(x: T): int => x.GetHashCode();\n"
    "end G2;\n";

JitResult run(const std::string& expr) {
    std::string app =
        "module A2;\n"
        "import L2;\n"
        "class P public static method Main(): int\n"
        "  Console.WriteLine($\"{" + expr + "}\");\n"
        "  return 0; end Main; end P;\n";
    return bezel::test::runAotModulesChecked({{"L2", kLib}, {"A2", app}}, "A2");
}

void expectOut(const std::string& expr, const std::string& want) {
    JitResult r = run(expr);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

}  // namespace

BEZEL_TEST(xmod_generic_method_hashcode_6907, local_bound_hash_prints_5_spec_8_3) {
    expectOut("G2.Hash2(5)", "5\n");
}

BEZEL_TEST(xmod_generic_method_hashcode_6907, expression_bodied_hash_prints_9_spec_8_3) {
    expectOut("G2.Hash3(9)", "9\n");
}

BEZEL_TEST(xmod_generic_method_hashcode_6907, explicit_int_argument_prints_5_spec_14_1) {
    expectOut("G2.Hash2<int>(5)", "5\n");
}
