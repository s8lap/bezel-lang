// Regression test for issue #7782 -- Task member shapes that exited 0 with an
// internal degradation warning instead of a diagnostic:
//   * `t?.Bogus` on a `Task<string>?` (the #7731 closed-surface check did not
//     run through `?.`);
//   * the method-group read `var x := t.Dispose;` (and `t?.Dispose`);
//   * the store `t.Result := 3` (#4045: store dropped).
//
// spec §30.3 (v0.350) declares the closed Task surface: `Task` has
// `readonly property` IsCompleted / IsFaulted / IsCancelled and `Wait()`;
// `Task<T>` / `OwningTask<T>` add `readonly property Result: T`. spec §7.5
// (v0.350): `x?.M` resolves `M` as `x.M` does. spec §23.1 (v0.350): a
// `readonly` auto-property "may be assigned only in a field initializer or in
// a constructor". spec §27.6 (v0.350): "No method reference to `Dispose` or
// `DisposeAsync`" -- bound over any receiver is a compile error at the
// formation, naming `d.Dispose();`, `_ := d;` and the owned-parameter lambda.
//
// NOT covered here (residual codegen work, see the commit body): the
// method-group read `var f := t.Wait;` (spec §27.6 admits it) and a valid
// `t?.IsCompleted` / `t?.Result` read still degrade in codegen.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return std::string("module M; import System; "
                       "extension Task "
                       "  public property Done: bool => this.IsCompleted; "
                       "end extension; "
                       "class C "
                       "  public static async method S(): Task<string> "
                       "    return \"abc\"; "
                       "  end S; "
                       "  public static method Main(): int ") +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// True iff the program is rejected at COMPILE TIME (not a crash / signal /
// timeout) at a REAL location, by an Error whose message contains `needle`.
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

// The `?.` twin of #7731: a member `Task<string>` does not have.
BEZEL_TEST(task_member_surface_7782, null_conditional_unknown_member_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("    var t: Task<string>? := null; "
             "    var n := t?.Bogus; "
             "    Console.WriteLine((n is null).ToString()); "),
        "`Task` has no member `Bogus`"));
}

// Same through an OwningTask<T> receiver.
BEZEL_TEST(task_member_surface_7782, null_conditional_unknown_member_on_owning_task_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; import System; "
        "class C "
        "  public static async method P(): OwningTask<Object> "
        "    return new Object(); "
        "  end P; "
        "  public static method Main(): int "
        "    var t: OwningTask<Object>? := null; "
        "    var n := t?.Bogus; "
        "    Console.WriteLine((n is null).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "`OwningTask` has no member `Bogus`"));
}

// Control: a declared extension property (spec §26.5) still binds through `?.`.
BEZEL_TEST(task_member_surface_7782, null_conditional_extension_property_still_binds) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var t0 := Task.FromResult(2); "
            "    var t: Task<int>? := t0; "
            "    var d := t?.Done; "
            "    if d <> null then Console.WriteLine(d!.ToString()); end; ")),
        std::string("True"));
}

// spec §27.6: no method reference to Dispose.
BEZEL_TEST(task_member_surface_7782, dispose_method_reference_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("    using var t := Task.FromResult(1); "
             "    var x := t.Dispose; "),
        "a method reference to `Dispose`"));
}

BEZEL_TEST(task_member_surface_7782, null_conditional_dispose_method_reference_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("    using var t0 := Task.FromResult(1); "
             "    var t: Task<int>? := t0; "
             "    var x := t?.Dispose; "),
        "a method reference to `Dispose`"));
}

// Control: the CALL `t.Dispose();` (the callee) is the spelling §27.6 points
// at, and stays accepted.
BEZEL_TEST(task_member_surface_7782, dispose_call_still_accepted) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var t := Task.FromResult(1); "
            "    t.Dispose(); "
            "    Console.WriteLine(\"ok\"); ")),
        std::string("ok"));
}

// spec §23.1 / §30.3: `Result` and the status properties are readonly.
BEZEL_TEST(task_member_surface_7782, result_store_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("    using var t := Task.FromResult(1); "
             "    t.Result := 3; "),
        "cannot assign to readonly property `Result`"));
}

BEZEL_TEST(task_member_surface_7782, is_completed_store_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("    using var t := Task.FromResult(1); "
             "    t.IsCompleted := true; "),
        "cannot assign to readonly property `IsCompleted`"));
}

BEZEL_TEST(task_member_surface_7782, result_compound_store_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("    using var t := Task.FromResult(1); "
             "    t.Result += 3; "),
        "cannot assign to readonly property `Result`"));
}

// Control: the documented reads through a `Task`-typed local are unchanged.
BEZEL_TEST(task_member_surface_7782, task_typed_local_is_completed_read) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var t := S(); "
            "    t.Wait(); "
            "    var tt: Task := t; "
            "    Console.WriteLine(tt.IsCompleted.ToString()); ")),
        std::string("True"));
}
