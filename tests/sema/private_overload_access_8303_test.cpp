// Spec-derived regression test for issue #8303 (sema leg).
//
// SPEC §13.1 (v0.355), *Levels*: `private` is visible to "The declaring class
// only"; `protected` to "The declaring class and all classes derived from it".
// §22.6 (v0.355): a call resolves among the candidates it can name, so a
// member the call site cannot see (§13.1) takes no part in resolution.
//
// THE DEFECT. With a PUBLIC overload of the same name present, the private
// overload was still selected from another class (`v.Len()` compiled and ran
// `private Len()`); with no public sibling the call was correctly refused.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

std::string program(const std::string& members, const std::string& call) {
    return "module M;\n"
           "class V\n"
           "  public constructor() end constructor;\n" +
           members +
           "end V;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var v := new V();\n"
           "    " + call + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

BEZEL_TEST(sema_13_1_private_overload_access_8303,
           control_single_private_candidate_is_refused) {
    // §13.1: the existing diagnostic for a lone private overload
    auto r = runAot(program("  private method Len(): int => 42;\n",
                            "Console.WriteLine(v.Len());"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "`Len` is private to type `V` and not "
                                  "accessible from `C` (\xC2\xA7""13.1"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

BEZEL_TEST(sema_13_1_private_overload_access_8303,
           issue_repro_private_zero_arity_beside_public_other_arity) {
    // the issue's literal `v.Len()`: only the inaccessible overload has the
    // call's arity, so the call is refused rather than run on `private Len()`
    auto r = runAot(program("  private method Len(): int => 42;\n"
                            "  public method Len(x: int): int => x + 100;\n",
                            "Console.WriteLine(v.Len());"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasError(r, "Len"));
}

BEZEL_TEST(sema_13_1_private_overload_access_8303,
           private_beside_inaccessible_protected_is_refused) {
    // a `protected` sibling is no more visible to an unrelated class than the
    // private one: it must not suppress the §13.1 refusal
    auto r = runAot(program("  private method Len(x: int): int => 42;\n"
                            "  protected method Len(x: long): int => 7;\n",
                            "Console.WriteLine(v.Len(1));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasError(r, "is private to type `V`"));
}

BEZEL_TEST(sema_13_1_private_overload_access_8303,
           method_reference_without_target_over_private_and_public) {
    // §27.6 + §13.1: only the visible overload is a candidate of the
    // reference, so a lone visible one is not ambiguous and binds without a
    // target
    auto r = runAot(program("  private method Len(): int => 42;\n"
                            "  public method Len(x: int): int => x + 100;\n",
                            "var f := v.Len;\n"
                            "    Console.WriteLine(f(1));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "101\n");
}
