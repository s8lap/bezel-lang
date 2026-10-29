// Regression test for issue #7731 -- `await SlowS(..).Length` (a read of a
// member that `Task<string>` does not have) was ACCEPTED, degraded in codegen
// to the internal #2434 / #2961 warnings at exit 0 (the initializer or the
// named argument's store dropped, the argument's tag never traced), and a
// later `+` over the untyped local reported a location-less (0:0) error.
//
// spec §17.1 (v0.350), Precedence: "`await x.y` parses as `await (x.y)` --
// primary access binds tighter than unary `await`." So
// `await SlowS(..).Length` reads `.Length` off the TASK. spec §30.3 (v0.350)
// declares `Task` with IsCompleted / IsFaulted / IsCancelled and Wait(),
// `Task<T>` and `OwningTask<T>` with Result: there is no `Length`, so the
// read is a compile error. The valid spelling is `(await SlowS(..)).Length`
// (§17.1: "`(await x) + 1` is the explicit form"), which evaluates once, in
// written order (§22.5, v0.350).
//
// The issue's "evaluated twice" observation was the degraded lowering of the
// ill-formed program; the valid spelling is pinned below to evaluate once.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

const char* kHelpers =
    "  public static method T(sb: StringBuilder, s: string, v: int): int "
    "    sb.Append(s); "
    "    return v; "
    "  end T; "
    "  public static async method SlowS(sb: StringBuilder, s: string, v: string): Task<string> "
    "    await Task.Delay(5); "
    "    sb.Append(s); "
    "    return v; "
    "  end SlowS; ";

std::string prog(const std::string& body) {
    return std::string("module M; import System; import System.Text; "
                       "class C ") +
           kHelpers +
           "  public static async method Main(): Task<int> "
           "    using var sb := new StringBuilder(); "
           "    using var sb2 := new StringBuilder(); " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// True iff the program is rejected at COMPILE TIME with an Error naming the
// missing `Length` member of `Task`.
bool rejectedAsUnknownTaskMember(const std::string& program) {
    auto r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("`Task` has no member `Length`") != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// The issue's standalone repro: rejected, not degraded.
BEZEL_TEST(task_member_read_closed_surface_7731, standalone_await_then_member_rejected) {
    BEZEL_EXPECT_TRUE(rejectedAsUnknownTaskMember(prog(
        "    var n := await SlowS(sb, \"C\", \"xyz\").Length; "
        "    Console.WriteLine(n.ToString() + \"|\" + sb.ToString()); ")));
}

// The issue's named-argument repro: rejected, not degraded.
BEZEL_TEST(task_member_read_closed_surface_7731, named_argument_await_then_member_rejected) {
    BEZEL_EXPECT_TRUE(rejectedAsUnknownTaskMember(prog(
        "    sb2.Append(\"abcdef\"); "
        "    var r := sb2.Remove(length := await SlowS(sb, \"C\", \"xy\").Length, "
        "                        startIndex := T(sb, \"D\", 0)).Length; "
        "    Console.WriteLine(r.ToString()); ")));
}

// Any other member of a task is rejected the same way (a local, not an await).
BEZEL_TEST(task_member_read_closed_surface_7731, local_task_unknown_member_rejected) {
    BEZEL_EXPECT_TRUE(rejectedAsUnknownTaskMember(prog(
        "    using var t := SlowS(sb, \"C\", \"xyz\"); "
        "    var n: int := t.Length; "
        "    Console.WriteLine(n.ToString()); ")));
}

// The valid spelling: each argument once, in written order, value correct.
BEZEL_TEST(task_member_read_closed_surface_7731, parenthesized_await_member_standalone) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var n := (await SlowS(sb, \"C\", \"xyz\")).Length; "
            "    Console.WriteLine(n.ToString() + \"|\" + sb.ToString()); ")),
        std::string("3|C"));
}

BEZEL_TEST(task_member_read_closed_surface_7731, parenthesized_await_member_named_argument_once_in_order) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    sb2.Append(\"abcdef\"); "
            "    var r := sb2.Remove(length := (await SlowS(sb, \"C\", \"xy\")).Length, "
            "                        startIndex := T(sb, \"D\", 1)).ToString(); "
            "    Console.WriteLine(r + \"|\" + sb.ToString()); ")),
        std::string("adef|CD"));
}

// Controls: the documented Task surface (IsCompleted read, Wait() call, and
// Result off a completed task outside an async context is covered elsewhere)
// is still accepted.
BEZEL_TEST(task_member_read_closed_surface_7731, documented_surface_still_accepted) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var t := SlowS(sb, \"C\", \"xyz\"); "
            "    var s := await t; "
            "    var done := t.IsCompleted; "
            "    var faulted := t.IsFaulted; "
            "    Console.WriteLine(s + \"|\" + done.ToString() + \"|\" + faulted.ToString()); ")),
        std::string("xyz|True|False"));
}

// The valid spelling 100x under ASan + LSan (string temporaries as awaited
// results and reordered named arguments).
BEZEL_TEST(task_member_read_closed_surface_7731, parenthesized_await_loop_clean_under_asan) {
    JitResult r = runAotChecked(prog(
        "    var total := 0; "
        "    for var i in 0..<100 do "
        "      sb2.Clear(); sb2.Append(\"abcdef\"); "
        "      total += sb2.Remove(length := (await SlowS(sb, \"C\", \"xy\")).Length, "
        "                          startIndex := T(sb, \"D\", 1)).Length; "
        "      var n := (await SlowS(sb, \"E\", \"xyz\")).Length; "
        "      total += n; "
        "    end for; "
        "    Console.WriteLine(total.ToString() + \"|\" + sb.Length.ToString() + \"|\" + "
        "                      sb.ToString().Substring(0, 4)); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("700|300|CDEC\n"));
}

// A DECLARED extension property (spec §26.5, v0.350) on `Task<T>` /
// `OwningTask<T>` binds the read: the closed-surface reject must not
// shadow it (the fix-forward of the first #7731 cut, which over-rejected these).
BEZEL_TEST(task_member_read_closed_surface_7731, extension_property_on_task_t_binds) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; import System; "
            "extension Task<T> "
            "  public property Done: bool => this.IsCompleted; "
            "end extension; "
            "class C "
            "  public static method Main(): int "
            "    using var t := Task.FromResult(1); "
            "    Console.WriteLine(t.Done.ToString()); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("True"));
}

BEZEL_TEST(task_member_read_closed_surface_7731, extension_property_on_owning_task_t_binds) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; import System; "
            "extension OwningTask<T> where T: class "
            "  public property Done: bool => this.IsCompleted; "
            "end extension; "
            "class C "
            "  public static async method P(): OwningTask<Object> "
            "    return new Object(); "
            "  end P; "
            "  public static method Main(): int "
            "    using var t := P(); "
            "    t.Wait(); "
            "    Console.WriteLine(t.Done.ToString()); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("True"));
}
