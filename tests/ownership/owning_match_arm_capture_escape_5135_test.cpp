// issue #5135: a lambda in a §35.5 owning-match arm that captures the arm's
// payload binding `w` and TRANSFERS it (`move w` in the lambda body), called
// twice, made r1 and r2 alias the same live Widget, never disposed (exit 0,
// no dispose line). MEASURED on the #4944 fix 087b86a0 by non-author review.
//
// THE RULE. Per §28.3 / §35.5 (v0.313) the arm payload binding is an owned
// local of the arm body's scope, so it is bound by the existing captured-
// owned-binding rule: a closure body may not transfer a captured owned
// binding ("cannot transfer captured owned binding `w` inside a closure").
// A closure that only BORROWS `w` stays legal.
//
// FIXED BY the #5134 change (seedOwningMatchArmBindings gives the binding a
// real states_ entry, which makes the existing isCapturedOwnedBinding check
// fire). FAIL-WITHOUT: with src/ownership/ reverted to origin/main both
// REJECT rows compile clean instead of drawing the diagnostic.
//
// per §28.3, §35.5, v0.313

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

bool hasErrorContaining(const JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

const char* kPrelude =
    "module M; "
    "public class Widget "
    "  private _n: string; "
    "  public constructor(n: string) _n := n; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_n}\"); end destructor; "
    "  public method Name(): string => _n; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer; "
    "public class Program "
    "  public static method Mk(): Outer => "
    "      Outer.W(new Widget(\"a\"), new Widget(\"b\")); ";

std::string prog(const std::string& armBody) {
    return std::string(kPrelude) +
           "  public static method Main(): int "
           "    var p := true; "
           "    var o := Mk(); "
           "    case move o of "
           "      W(var w, _) => do " + armBody + " end, "
           "      X => () "
           "    end case; "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

}  // namespace

// (REJECT 1 — issue repro p3) closure moves captured `w`, called twice.
BEZEL_TEST(owning_match_arm_capture_escape_5135,
           lambda_moves_captured_arm_binding_rejected_28_3) {
    JitResult r = runAot(prog(
        "var f := lambda() => (if p then move w else new Widget(\"z\")); "
        "var r1 := f(); "
        "Console.WriteLine($\"r1={r1.Name()}\"); "
        "var r2 := f(); "
        "Console.WriteLine($\"r2={r2.Name()}\"); "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "cannot transfer captured owned binding `w` inside a closure"));
}

// (REJECT 2 — issue repro p3b) same, results bound with `using var`.
BEZEL_TEST(owning_match_arm_capture_escape_5135,
           lambda_moves_captured_arm_binding_using_rejected_28_3) {
    JitResult r = runAot(prog(
        "var f := lambda() => (if p then move w else new Widget(\"z\")); "
        "using var r1 := f(); "
        "Console.WriteLine($\"r1={r1.Name()}\"); "
        "using var r2 := f(); "
        "Console.WriteLine($\"r2={r2.Name()}\"); "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "cannot transfer captured owned binding `w` inside a closure"));
}

// (ACCEPT control) a closure that only BORROWS `w` runs (twice); `w` is then
// moved OUTSIDE the closure into a `using` local, so the arm still discharges
// it exactly once.
BEZEL_TEST(owning_match_arm_capture_escape_5135,
           lambda_borrowing_captured_arm_binding_accepted) {
    JitResult r = runAot(prog(
        "var f := lambda() => w.Name(); "
        "Console.WriteLine($\"n1={f()}\"); "
        "Console.WriteLine($\"n2={f()}\"); "
        "using var r := move w; "
        "Console.WriteLine($\"r={r.Name()}\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "n1=a\nn2=a\nr=a\ndispose a\ndispose b\n");
}
