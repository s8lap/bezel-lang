// Spec-derived regression test for issue #8205 (residuals of #8121).
//
// SPEC §27.6 (v0.355), *Method references to overloaded methods*, verbatim:
//
//   "When the named method (or property) is overloaded, the reference is
//    resolved using the **target function type** at the use site."
//
//   "When no target type is available (e.g., `var f := Math.Abs;` with no
//    annotation), the reference is ambiguous and the compiler reports an error
//    pointing to the candidate overloads."
//
// SPEC §27.6 (v0.355), extension methods, verbatim:
//
//   "**Receiver-qualified** — `v.Scaled` over `extension Vec`, the member
//    resolved on the value exactly as the call `v.Scaled()` is (§26.5) …"
//
// and §26.5 rules 2/4: a call consults every applicable extension block, so
// overloads split across two blocks are ONE overload set.
//
// THE DEFECTS (measured on this fix's base):
//   A. `Console.WriteLine(v.Len)` over an OVERLOADED `Len` compiled and
//      printed `() -> int`: the `Object` parameter supplies no target function
//      type, so the no-target ambiguity rule applies and was never run there.
//   B. overloads split across TWO extension blocks: `a.Len(5)` resolved but
//      `var g: (int) -> int := a.Len;` was refused (§5.1) because the
//      reference's candidate set was the one block the bare lookup stopped at.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

const char kNoTarget[] =
    "the method reference `Len` is overloaded and has no target type to "
    "select by (\xC2\xA7""27.6 v0.307)";

const char kClassV[] =
    "class V\n"
    "  public constructor() end constructor;\n"
    "  public method Len(): int => 42;\n"
    "  public method Len(x: int): int => x + 100;\n"
    "end V;\n";

std::string classMain(const std::string& body) {
    return std::string("module M;\n") + kClassV +
           "class C\n"
           "  public static method Main(): int\n"
           "    var v := new V();\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

void expectRejected(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

void expectCheckedStdout(const std::string& src, const std::string& out) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// ── A ───────────────────────────────────────────────────────────────────────

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           overloaded_reference_to_console_writeline_is_ambiguous) {
    // per §27.6 (v0.355): the issue's literal repro
    expectRejected(classMain("Console.WriteLine(v.Len);"), kNoTarget);
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           overloaded_extension_reference_to_console_writeline_is_ambiguous) {
    // per §27.6 (v0.355): the extension form of the same repro
    expectRejected(
        std::string("module M;\n"
                    "class V\n"
                    "  public constructor() end constructor;\n"
                    "end V;\n"
                    "extension V\n"
                    "  public method Len(): int => 42;\n"
                    "  public method Len(x: int): int => x + 100;\n"
                    "end;\n"
                    "class C\n"
                    "  public static method Main(): int\n"
                    "    var v := new V();\n"
                    "    Console.WriteLine(v.Len);\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n"),
        kNoTarget);
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           non_overloaded_reference_to_console_writeline_still_prints_type) {
    // control: a single candidate is not ambiguous (§27.6, §8.5)
    expectCheckedStdout(
        "module M;\n"
        "class V\n"
        "  public constructor() end constructor;\n"
        "  public method Len(): int => 42;\n"
        "end V;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var v := new V();\n"
        "    Console.WriteLine(v.Len);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "() -> int\n");
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           overloaded_reference_with_a_target_still_selects) {
    // control: the target-typed selection is untouched
    expectCheckedStdout(classMain("var g: (int) -> int := v.Len;\n"
                                  "    Console.WriteLine(g(5));"),
                        "105\n");
}

