// Regression tests for issue #7868: `t?.Bogus()` -- a method the closed
// spec §30.3 (v0.350) surface of Task / Task<T> / OwningTask<T> does not
// declare, called through `?.` (spec §7.5: `x?.M` resolves `M` as `x.M`
// does), was accepted silently. The `.` form degraded in codegen. Extension
// members (spec §26.5) on a task still bind when the instance surface does
// not declare the name.

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

}  // namespace

BEZEL_TEST(task_call_surface_7868, null_conditional_unknown_method_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("", "    using var t0 := Task.FromResult(2); "
                 "    var t: Task<int>? := t0; t?.Bogus(); "),
        "`Task` has no method `Bogus`"));
}

BEZEL_TEST(task_call_surface_7868, dot_unknown_method_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("", "    using var a := Task.FromResult(1); a.Bogus(); "),
        "`Task` has no method `Bogus`"));
}

// Control: a declared extension method (spec §26.5) on a task still binds
// through `.` and `?.` when the instance surface does not declare the name.
BEZEL_TEST(task_call_surface_7868, extension_method_still_binds) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "extension Task<T> "
            "  public method Twice(k: int): int "
            "    return k * 2; "
            "  end Twice; "
            "end extension; ",
            "    using var a := Task.FromResult(1); "
            "    Console.WriteLine(a.Twice(4).ToString()); ")),
        std::string("8"));
}

// Review fix (spec §8.1, v0.350): `GetType` is declared on Object, so the
// closed task call surface must not claim `Task` has no method `GetType`.
// The reflection-deferred diagnostic (issue #487) is the only error expected.
namespace {
bool reportsFalseGetTypeError(const std::string& program) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("has no method `GetType`") != std::string::npos)
            return true;
    return false;
}
}  // namespace

BEZEL_TEST(task_call_surface_7868, dot_get_type_is_not_an_undeclared_method) {
    BEZEL_EXPECT_TRUE(!reportsFalseGetTypeError(
        prog("", "    using var a := Task.FromResult(1); a.GetType(); ")));
}

BEZEL_TEST(task_call_surface_7868, null_conditional_get_type_is_not_an_undeclared_method) {
    BEZEL_EXPECT_TRUE(!reportsFalseGetTypeError(
        prog("", "    using var t0 := Task.FromResult(2); "
                 "    var t: Task<int>? := t0; t?.GetType(); ")));
}
