// Codegen/runtime - issue #7808 (spec §17.8, §28.3, §28.10, v0.357): a plain
// `var t := h.F!;` over an `owned F: Task?` field is a BORROW of the field's
// frame — the same storage read as `var t := h.F;` — so the local registers no
// scope-end disposal and the holder disposes the one `bz_task_t*` exactly once.
//
// THE GOVERNING TEXT.
//
//   docs/Bezel_Language_Spec.md (§17.8, v0.357): "The null-forgiving operator
//   preserves ownership: ... if `x` is borrowed, `x!` is borrowed with the
//   same source."
//
//   §28.3 (*Ownership state of locals*, v0.357): "When `expr` is a borrow —
//   for example a call without an owned return, a field or indexer read, or
//   an unmarked read of a **borrow-state** named binding — `x` is instead a
//   **borrow-pin** with no disposal obligation ([§28.10])".
//
// THE BUG. The plain-`var` Task disposal gate (taskLocalOwnsFrame ->
// taskLocalInitializerIsBorrowSource) recognised a storage-read initializer
// only as a bare MemberAccessExpr / NullCondMemberExpr / IndexExpr /
// IdentifierExpr, so the PostfixBangExpr `h.F!` looked like a MINT: `t`
// registered a `d.task`, and `t`'s scope end and the holder's owned-field
// disposal both disposed the field's frame (MEASURED on r30-c1 e49c32aca: the
// issue's `var t := h.F!; using var u := t; await u;` and the bare
// `var t := h.F!;` both hang after the first iteration, exit 124 under
// `timeout`; `using var u := h.F!; await u;` and `var t := h.F; await t!;`
// are clean).
//
// THE ORACLE. runAotChecked: stdout and exit code pinned; a hang is the
// harness's run deadline. Every row loops 100x.
//
// per §17.8, §28.3, §28.10, v0.357

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string repeat(const std::string& s, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

void expectRunOutput(const JitResult& r, const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// F is an owned Task? field started in the constructor (the issue's shape).
std::string asyncLoop(const std::string& body) {
    return std::string("module M; ") +
           "public class Holder "
           "  public owned F: Task? := null; "
           "  public owned G: Task<int>? := null; "
           "  public constructor() this.F := Program.W(); this.G := Program.WI(); end constructor; "
           "end Holder; "
           "public class Program "
           "public static async method W(): Task Console.WriteLine(\"w\"); end W; "
           "public static async method WI(): Task<int> return 7; end WI; "
           "public static async method Main(): Task "
           " for var i in 0..<100 do "
           "  using var h := new Holder(); " + body +
           " end; "
           " Console.WriteLine(\"END\"); end Main; "
           "end Program;";
}

}  // namespace

BEZEL_TEST(task_local_bang_field_read_is_borrow_7808,
           spec_17_8_issue_repro_pin_of_an_alias_of_a_bang_field_read_awaits_once) {
    // The issue's literal shape: `var t := h.F!; using var u := t; await u;`.
    auto r = runAotChecked(asyncLoop("var t := h.F!; using var u := t; await u; "));
    expectRunOutput(r, repeat("w\n", 100) + "END\n");
}

BEZEL_TEST(task_local_bang_field_read_is_borrow_7808,
           spec_17_8_bare_bang_field_read_local_registers_no_disposal) {
    // Wider than filed: the binding alone, never awaited, hangs on the base.
    auto r = runAotChecked(asyncLoop("var t := h.F!; "));
    expectRunOutput(r, repeat("w\n", 100) + "END\n");
}

BEZEL_TEST(task_local_bang_field_read_is_borrow_7808,
           spec_17_8_bang_field_read_local_awaited_directly) {
    auto r = runAotChecked(asyncLoop("var t := h.F!; await t; "));
    expectRunOutput(r, repeat("w\n", 100) + "END\n");
}

BEZEL_TEST(task_local_bang_field_read_is_borrow_7808,
           spec_17_8_bang_field_read_of_a_task_of_int_yields_its_result) {
    // Task<int>: the borrow-awaited result is read; the holder disposes G once.
    auto r = runAotChecked(asyncLoop(
        "var t := h.G!; var n := await t; Console.WriteLine(n); "));
    expectRunOutput(r, repeat("w\n7\n", 100) + "END\n");
}

BEZEL_TEST(task_local_bang_field_read_is_borrow_7808,
           spec_17_8_control_bang_at_the_await_over_a_plain_field_read_local) {
    // Positive control (green on the base too): the `!` at the await, over
    // the bare storage read `var t := h.F;`, which was already a borrow.
    auto r = runAotChecked(asyncLoop("var t := h.F; await t!; "));
    expectRunOutput(r, repeat("w\n", 100) + "END\n");
}

BEZEL_TEST(task_local_bang_field_read_is_borrow_7808,
           spec_17_8_bang_field_read_local_in_a_sync_body) {
    auto r = runAotChecked(
        "module M; "
        "public class Holder "
        "  public owned F: Task? := null; "
        "  public constructor() this.F := Program.W(); end constructor; "
        "end Holder; "
        "public class Program "
        "public static async method W(): Task Console.WriteLine(\"w\"); end W; "
        "public static method Main(): int "
        " for var i in 0..<100 do "
        "  using var h := new Holder(); var t := h.F!; "
        " end; "
        " Console.WriteLine(\"END\"); return 0; end Main; "
        "end Program;");
    expectRunOutput(r, repeat("w\n", 100) + "END\n");
}
