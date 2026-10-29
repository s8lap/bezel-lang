// Regression tests for issue #7869: `t.Wait(3)` -- spec §30.3 (v0.350)
// declares `public method Wait();` with no parameters; the call exited 0 with
// an internal degradation warning. `Wait` is declared, so §26.5 rule 1
// commits the lookup by name and a `Wait(k)` extension is never consulted.
// `t?.Wait(3)` resolves as `t.Wait(3)` does (spec §7.5, v0.350).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& ext, const std::string& body) {
    return std::string("module M; import System; ") + ext +
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

bool rejectedWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    if (!bezel::test::failedAtCompileTimeWithRealLocation(r)) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

const char* kExtWait =
    "extension Task<T> "
    "  public method Wait(k: int) "
    "  end Wait; "
    "end extension; ";

}  // namespace

BEZEL_TEST(task_call_surface_7869, wait_with_argument_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("", "    using var a := Task.FromResult(1); a.Wait(3); "),
        "`Wait` of `Task` takes no arguments"));
}

BEZEL_TEST(task_call_surface_7869, wait_with_argument_rejected_despite_extension) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog(kExtWait, "    using var a := Task.FromResult(1); a.Wait(3); "),
        "`Wait` of `Task` takes no arguments"));
}

BEZEL_TEST(task_call_surface_7869, null_conditional_wait_with_argument_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("", "    using var t0 := Task.FromResult(2); "
                 "    var t: Task<int>? := t0; t?.Wait(3); "),
        "`Wait` of `Task` takes no arguments"));
}

// Control: Wait() with no arguments is the documented call.
BEZEL_TEST(task_call_surface_7869, wait_without_argument_runs) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("", "    using var a := Task.FromResult(1); a.Wait(); "
                           "    Console.WriteLine(a.IsCompleted.ToString()); ")),
        std::string("True"));
}

