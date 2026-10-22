// Regression test for issue #6947 (§17.11 / §2.8.2 / §14.1, v0.340).
//
// `U.DefB<string>()` (body `$"{default(T)}"`): T appears in no formal, so the
// defining module's template ran T as the `int` (i32) placeholder and printed
// 0, where the same-module call is rejected "type argument `string` is not
// defaultable (§17.11)". §2.8.2 clause 1: an external instantiation must behave
// as a same-module one. The exporter now ships the body of a method with an
// unmentioned type parameter, so the importer instantiates it and reaches the
// same-module verdict.

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
    "end U;\n"
    "public class Sq\n"
    "  public x: int;\n"
    "  public constructor(x: int) this.x := x; end constructor;\n"
    "end Sq;\n";

std::string mainOf(const std::string& call) {
    return "public class Program\n"
           "  public static method Main(): int\n"
           "    Console.WriteLine(" + call + ");\n"
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

JitResult runImported(const std::string& call) {
    std::string app = "module App;\nimport ULib;\n" + mainOf(call);
    return bezel::test::runAotModulesChecked({{"ULib", kLib}, {"App", app}}, "App");
}

}  // namespace

BEZEL_TEST(xmod_generic_method_reference_unmentioned_6947, string_binding_rejected_as_not_defaultable_spec_17_11) {
    JitResult r = runImported("U.DefB<string>()");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "type argument `string` is not defaultable (\xC2\xA7""17.11)"));
}

BEZEL_TEST(xmod_generic_method_reference_unmentioned_6947, class_binding_rejected_as_not_defaultable_spec_17_11) {
    JitResult r = runImported("U.DefB<Sq>()");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "is not defaultable (\xC2\xA7""17.11)"));
}

// Control: the same call in ONE module is rejected with the same text.
BEZEL_TEST(xmod_generic_method_reference_unmentioned_6947, same_module_string_binding_rejected) {
    std::string src =
        "module One;\n"
        "public class U\n"
        "  public static method DefB<T>(): string => $\"{default(T)}\";\n"
        "end U;\n" + mainOf("U.DefB<string>()");
    JitResult r = bezel::test::runAotModulesChecked({{"One", src}}, "One");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "type argument `string` is not defaultable (\xC2\xA7""17.11)"));
}

// Control: a value-type (`int`) binding still runs across the boundary.
BEZEL_TEST(xmod_generic_method_reference_unmentioned_6947, int_binding_still_runs) {
    JitResult r = runImported("U.DefB<int>()");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// Control: a value-type non-int binding runs with the same-module value.
BEZEL_TEST(xmod_generic_method_reference_unmentioned_6947, bool_binding_runs_same_module_value) {
    JitResult r = runImported("U.DefB<bool>()");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n"));
}
