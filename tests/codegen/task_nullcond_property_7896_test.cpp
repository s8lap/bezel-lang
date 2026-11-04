// Issue #7896 / #7981 -- `?.` property reads on a nullable `Task` / `Task<T>`.
//
// spec §30.3 (v0.355): `public class Task` declares `public readonly property
// IsCompleted: bool;` (and `IsFaulted: bool` / `IsCancelled: bool`);
// `public class Task<T>: Task` declares `public readonly property Result: T;`.
// spec §7.5 (v0.355): "`x?.M` returns null if `x` is null; otherwise it
// performs the access. The result type is always nullable". spec §7.4
// (v0.355): member access on a nullable value is a compile error unless
// narrowed, and `?.` is the sanctioned spelling -- it yields the nullable of
// the member type.
//
// Before the fix, `t?.IsCompleted` / `t?.Result` / `t?.IsFaulted` on a
// `Task<int>?` receiver lowered to NOTHING: a `?.` access is a distinct AST
// node, so the §30.3 Task-property arms the `.` form reaches (bz_task_is_done
// / bz_task_is_error / bz_task_await_sync) never ran, and the access fell
// through to the generic storage-field path, whose `storageMemberOf` is null
// for a builtin Task. The enclosing declaration then kept a ZERO-INITIALISED
// value (the #2434 "lowerLocalDecl's initializer expression failed to lower"
// warning) -- a silent wrong value at exit 0.
//
// §7 forbids printing a `T?` directly, so each read is consumed with `??`.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

}  // namespace

// §30.3 / §7.5: the four §30.3 Task properties over a NON-NULL receiver --
// `?.` performs the access and lifts the result to the nullable of the member
// type. Before the fix every read below was a zero-initialised `T?`.
BEZEL_TEST(task_nullcond_property_7896, spec_7_5_present_receiver_reads_the_task_properties) {
    auto r = runAot(prog(
        "using var t0 := Task.FromResult(7); "
        "var t: Task<int>? := t0; "
        "Console.WriteLine(t?.IsCompleted ?? false); "
        "Console.WriteLine(t?.Result ?? -1); "
        "Console.WriteLine(t?.IsFaulted ?? true); "
        "Console.WriteLine(t?.IsCancelled ?? true); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n7\nFalse\nFalse\n");
}

// §7.5: a NULL receiver yields null for every one of the four properties, so
// the `??` default is taken. (Before the fix the read lowered to nothing at
// all, so even this row was wrong.)
BEZEL_TEST(task_nullcond_property_7896, spec_7_5_null_receiver_yields_null) {
    auto r = runAot(prog(
        "var t: Task<int>? := null; "
        "Console.WriteLine(t?.IsCompleted ?? false); "
        "Console.WriteLine(t?.Result ?? -1); "
        "Console.WriteLine(t?.IsFaulted ?? true); "
        "Console.WriteLine(t?.IsCancelled ?? true); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n-1\nTrue\nTrue\n");
}

// §7.4 / #7981: the `?.` result is a real `T?` -- it stores into a `T?`
// local, is testable against null, and carries the value (not a
// zero-initialised placeholder).
BEZEL_TEST(task_nullcond_property_7896, spec_7_4_result_is_a_nullable_local_carrying_the_value) {
    auto r = runAot(prog(
        "using var t0 := Task.FromResult(7); "
        "var t: Task<int>? := t0; "
        "var r: int? := t?.Result; "
        "var b: bool? := t?.IsCompleted; "
        "var f: bool? := t?.IsFaulted; "
        "if r <> null then Console.WriteLine(r); end if; "
        "if b = null then Console.WriteLine(\"null\"); else Console.WriteLine(b); end if; "
        "if f = null then Console.WriteLine(\"null\"); else Console.WriteLine(f); end if; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\nTrue\nFalse\n");
}

// §30.3: `Result` on a `Task<string>` -- the member type is a REFERENCE, so
// the §7.5 nullable is the null-pointer form, not a tagged value struct.
BEZEL_TEST(task_nullcond_property_7896, spec_7_5_reference_result_member) {
    auto r = runAot(prog(
        "using var t0 := Task.FromResult(\"hi\"); "
        "var t: Task<string>? := t0; "
        "var r: string? := t?.Result; "
        "Console.WriteLine(r ?? \"<null>\"); "
        "var n: Task<string>? := null; "
        "var rn: string? := n?.Result; "
        "Console.WriteLine(rn ?? \"<null>\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "hi\n<null>\n");
}

// §7.4 / §7.5: the `?.` read agrees with the narrowed `.` read, in a loop
// (the lowering runs per iteration), for a present receiver.
BEZEL_TEST(task_nullcond_property_7896, spec_7_4_nullcond_read_agrees_in_a_loop) {
    auto r = runAot(prog(
        "using var t0 := Task.FromResult(7); "
        "var t: Task<int>? := t0; "
        "var n := 0; var i := 0; "
        "while i < 100 do "
        "  if (t?.Result ?? -1) = 7 then n := n + 1; end if; "
        "  if (t?.IsCompleted ?? false) then n := n + 1; end if; "
        "  i := i + 1; "
        "end while; "
        "Console.WriteLine(n); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}

// #7981's literal repro: the INFERRED local form (`var r := t2?.Result;`)
// over a `Task<int>?` in a NON-async method. Before the fix the initializer
// degraded to a zero-initialised value (the #2434 note), so the read was 0.
BEZEL_TEST(task_nullcond_property_7896, spec_7_4_inferred_local_over_a_task_receiver) {
    auto r = runAot(prog(
        "using var t := Task.FromResult(1); "
        "var t2: Task<int>? := t; "
        "var r := t2?.Result; "
        "var b := t2?.IsCompleted; "
        "Console.WriteLine(r ?? -1); "
        "Console.WriteLine(b ?? false); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\nTrue\n");
}

// §7.5: the null-receiver arm of the same loop -- every read is null, so the
// `??` default is taken 100 times.
BEZEL_TEST(task_nullcond_property_7896, spec_7_5_null_receiver_arm_in_a_loop) {
    auto r = runAot(prog(
        "var t: Task<int>? := null; "
        "var n := 0; var i := 0; "
        "while i < 100 do "
        "  if (t?.Result ?? -1) = -1 then n := n + 1; end if; "
        "  if (t?.IsCompleted ?? true) then n := n + 1; end if; "
        "  i := i + 1; "
        "end while; "
        "Console.WriteLine(n); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}
