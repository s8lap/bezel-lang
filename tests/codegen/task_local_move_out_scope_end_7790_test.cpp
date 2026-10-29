// Codegen/runtime - issue #7790 (spec §28.8.1, §28.10, §30.5, v0.350):
// moving a named plain-`var` Task-family local (`var u := move t;`,
// `using var t2 := move t;`) leaves the SOURCE moved-from, so its scope-end
// disposal skips it and the one `bz_task_t*` is disposed exactly once.
//
// THE GOVERNING TEXT.
//
//   docs/Bezel_Language_Spec.md (§28.10, v0.350): "An **unmarked**
//   `using var y := x;` is a **compile error** when its right-hand side is a
//   named owned binding ..."; its remedy is the marked adoption
//   `using var y := move x;`, which "leaves `x` moved-from" (§28.8.1: "a
//   moved-from binding is unreadable until reassigned"; its scope-end
//   disposal is skipped, §28.7 *Moved-from and null*).
//
// THE BUG. The scope-end disposal of a plain `var t := Producer();` task local
// (lowerStmt's LocalDecl arm, `d.task`) is null-guarded, but the `move t`
// lowering (lowerUnary, "move") vacated only `using var` / auto-drop /
// arm-transfer locals. A plain Task local kept the pointer, so BOTH `t` and
// the new binding ran `bz_task_fire_and_forget` on the one frame: the second
// call locked a freed task mutex and the process never exited after `END`
// (MEASURED: exit 124 under `timeout`, plain and under ASan).
//
// THE ORACLE. runAotChecked: stdout and exit code are pinned (a hang is a
// harness timeout, a double free an ASan abort), and the OwningTask rows pin
// the produced Res's single `DTOR`.
//
// per §28.8.1, §28.10, §30.5, v0.350

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

const char* kDecls =
    "public class Res "
    "  public constructor() end constructor; "
    "  destructor Console.WriteLine(\"DTOR\"); end destructor; "
    "end Res; ";

const char* kProducers =
    "public static async method W(): Task Console.WriteLine(\"w\"); end W; "
    "public static async method WI(): Task<int> "
    "  Console.WriteLine(\"wi\"); return 7; end WI; "
    "public static async method WO(): OwningTask<Res> return new Res(); end WO; ";

std::string asyncMain(const std::string& body) {
    return std::string("module M; ") + kDecls +
           "public class Program " + kProducers +
           "  public static async method Main(): Task " + body +
           "    Console.WriteLine(\"END\"); "
           "  end Main; "
           "end Program;";
}

std::string syncMain(const std::string& body) {
    return std::string("module M; ") + kDecls +
           "public class Program " + kProducers +
           "  public static method Main(): int " + body +
           "    Console.WriteLine(\"END\"); return 0; "
           "  end Main; "
           "end Program;";
}

void expectRunOutput(const JitResult& r, const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

BEZEL_TEST(task_local_move_out_scope_end_7790,
           spec_28_8_1_var_move_of_a_task_local_in_an_async_body_disposes_once) {
    // The issue's first repro: the new binding is never awaited.
    expectRunOutput(runAotChecked(asyncMain(
        "var t := Program.W(); var u := move t; ")),
        "w\nEND\n");
}

BEZEL_TEST(task_local_move_out_scope_end_7790,
           spec_28_8_1_var_move_of_a_task_local_in_a_sync_body_disposes_once) {
    expectRunOutput(runAotChecked(syncMain(
        "var t := Program.W(); var u := move t; ")),
        "w\nEND\n");
}

BEZEL_TEST(task_local_move_out_scope_end_7790,
           spec_28_10_using_var_move_of_a_task_local_then_await_disposes_once) {
    // The issue's second repro, and the §28.10 remedy the v0.350 diagnostic
    // offers: `using var t2 := move t; await t2;`.
    expectRunOutput(runAotChecked(asyncMain(
        "var t := Program.W(); using var t2 := move t; await t2; ")),
        "w\nEND\n");
}

BEZEL_TEST(task_local_move_out_scope_end_7790,
           spec_28_10_using_var_move_of_a_task_local_never_awaited_disposes_once) {
    expectRunOutput(runAotChecked(asyncMain(
        "var t := Program.W(); using var t2 := move t; ")),
        "w\nEND\n");
}

BEZEL_TEST(task_local_move_out_scope_end_7790,
           spec_28_10_using_var_move_of_a_task_local_in_a_sync_body_disposes_once) {
    expectRunOutput(runAotChecked(syncMain(
        "var t := Program.W(); using var t2 := move t; ")),
        "w\nEND\n");
}

BEZEL_TEST(task_local_move_out_scope_end_7790,
           spec_28_10_using_var_move_of_a_task_of_t_local_awaits_the_value) {
    expectRunOutput(runAotChecked(asyncMain(
        "var t := Program.WI(); using var t2 := move t; "
        "var v := await t2; Console.WriteLine(v); ")),
        "wi\n7\nEND\n");
}

BEZEL_TEST(task_local_move_out_scope_end_7790,
           spec_28_10_using_var_move_of_an_owningtask_local_disposes_result_once) {
    // The OwningTask's produced Res is disposed exactly once: by `r` at scope
    // end, after END.
    expectRunOutput(runAotChecked(asyncMain(
        "var t := Program.WO(); using var t2 := move t; "
        "using var r := await t2; ")),
        "END\nDTOR\n");
}

BEZEL_TEST(task_local_move_out_scope_end_7790,
           spec_28_8_1_var_move_of_an_unawaited_owningtask_local_disposes_result_once) {
    // `u` owns the never-awaited task and its produced Res: one DTOR.
    expectRunOutput(runAotChecked(asyncMain(
        "var t := Program.WO(); var u := move t; ")),
        "END\nDTOR\n");
}

BEZEL_TEST(task_local_move_out_scope_end_7790,
           spec_28_8_1_move_of_a_task_local_held_across_a_suspend_disposes_once) {
    // `t` lives in the async frame across the Delay suspend; the move after
    // it must vacate the FRAME slot, not a per-resume copy.
    expectRunOutput(runAotChecked(
        std::string("module M; import System.Threading; ") + kDecls +
        "public class Program " + kProducers +
        "  public static async method Main(): Task "
        "    var t := Program.W(); "
        "    await Task.Delay(1); "
        "    using var t2 := move t; "
        "    await Task.Delay(1); "
        "    await t2; "
        "    Console.WriteLine(\"END\"); "
        "  end Main; "
        "end Program;"),
        "w\nEND\n");
}

BEZEL_TEST(task_local_move_out_scope_end_7790,
           spec_28_8_1_move_of_a_task_local_in_a_loop_100_times_disposes_once_each) {
    // Loop the move 100x: a per-iteration double dispose shows as a hang or an
    // abort, a leak as the ASan/LSan verdict runAotChecked folds in.
    std::string expected;
    for (int i = 0; i < 100; ++i) expected += "w\n";
    expected += "END\n";
    expectRunOutput(runAotChecked(asyncMain(
        "for var i in 1..100 do "
        "  var t := Program.W(); using var t2 := move t; await t2; "
        "end; ")),
        expected);
}

BEZEL_TEST(task_local_move_out_scope_end_7790,
           spec_28_3_task_local_view_binding_still_disposes_once_control) {
    // Control (the shape the issue says works): `var t2 := t;` is a borrowing
    // view; the owner `t` is the sole disposer. The fix must not touch it.
    expectRunOutput(runAotChecked(asyncMain(
        "var t := Program.W(); var t2 := t; await t2; ")),
        "w\nEND\n");
}
