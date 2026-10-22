// Regression test for issue #6899 (§14.5 / §14.1 / §2.8.2 clause 2, v0.340).
//
// `U.DefB<bool>()`: the imported generic method's type parameter appears in NO
// formal parameter (its body is `$"{default(T)}"`), so no argument carries the
// binding across the module boundary. Neither the #1943 width gate nor the
// #6779 per-argument arms saw it; the defining module's template, compiled
// against the `int` (i32) placeholder, ran `T` as `int` and printed 0, where
// the same-module call prints False. §2.8.2 clause 2 requires an external
// instantiation to behave identically to a same-module one, and lets an
// implementation that cannot carry the shape reject the import with a
// diagnostic instead. The explicit non-`int` binding is therefore reported at
// the type argument, as every other non-`int` binding already is (#6779).
//
// Controls: `DefB<int>()` prints 0 (the placeholder IS `int`); the same-module
// call prints False; a binding the formals DO mention keeps its own gate
// (`Count<long>(99)`, #1943 T5).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;

namespace {

const char* kLib =
    "module ULib;\n"
    "public class U\n"
    "  public static method DefB<T>(): string => $\"{default(T)}\";\n"
    "  public static method Count<T>(a: T): int => 7;\n"
    "end U;\n";

JitResult runImported(const std::string& call) {
    std::string app =
        "module App;\n"
        "import ULib;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(" + call + ");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    return bezel::test::runAotModulesChecked({{"ULib", kLib}, {"App", app}}, "App");
}

}  // namespace

// #6947 flip: the exporter now ships the body of a method with an unmentioned
// type parameter, so the call is instantiated per binding (same-module value,
// spec §2.8.2 clause 1) instead of refused at the type argument. These two rows
// pinned the #6899 reject; they now pin the same-module value.
BEZEL_TEST(xmod_generic_method_explicit_unmentioned_6899, bool_binding_runs_with_same_module_value_spec_14_5) {
    JitResult r = runImported("U.DefB<bool>()");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n"));
}

BEZEL_TEST(xmod_generic_method_explicit_unmentioned_6899, long_binding_runs_with_same_module_value_spec_14_5) {
    JitResult r = runImported("U.DefB<long>()");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// Control: the placeholder is `int`, so an `int` binding is exactly right.
BEZEL_TEST(xmod_generic_method_explicit_unmentioned_6899, int_binding_still_runs_spec_14_1) {
    JitResult r = runImported("U.DefB<int>()");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// Control (#1943 T5): a mentioned binding is the width gate's to judge.
BEZEL_TEST(xmod_generic_method_explicit_unmentioned_6899, mentioned_long_with_int_literal_still_runs_spec_14_1) {
    JitResult r = runImported("U.Count<long>(99)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}
