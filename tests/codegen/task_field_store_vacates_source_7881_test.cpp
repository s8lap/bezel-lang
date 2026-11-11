// Codegen/runtime - issue #7881 (spec §28.3 K3.1, §17.8, §28.7, v0.357):
// the unmarked store `h.T := t;` of a named owned Task binding into an
// `owned` Task-family field transfers the frame into the field and leaves the
// SOURCE moved-from, so the one `bz_task_t*` is disposed exactly once (by the
// field's holder).
//
// THE GOVERNING TEXT.
//
//   docs/Bezel_Language_Spec.md (§28.3, the consume table, row K3.1, v0.357):
//   "store into an `owned` field or owning-collection slot ... | none — the
//   declared slot announces (marker-free) | **moved-from** — a later read is
//   an error until reassigned | frozen (K5.1) | none here — the new
//   holder's".
//
//   §17.8 (v0.357): "`sink._f := x!;` consumes `x` exactly as `sink._f := x;`
//   does."
//
//   §28.7 (v0.357): the displaced value of the owned field is disposed at the
//   store (drop-on-overwrite).
//
// THE BUG. `lowerOwnedTaskFieldStore` disposed the displaced task and stored
// the new one, but nothing vacated the source: a plain `var t := W();` Task
// local keeps its `d.task` scope-end disposal (registered under
// taskLocalOwnsFrame), which reloaded the same pointer and fired it a second
// time after the holder's owned-field disposal had already freed it
// (MEASURED on r30-c1 e49c32aca: the issue's 100x loop printed two lines and
// never exited — exit 124 under `timeout`).
//
// THE ORACLE. runAotChecked: stdout and exit code are pinned. A hang is the
// harness's run deadline (a failure, not a pass); a double free is an ASan
// abort under BEZEL_AOT_FORCE_SANITIZE=address. Every row loops 100x so a
// one-shot that happens to survive cannot pass.
//
// per §28.3, §17.8, §28.7, v0.357

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

const char* kHolder =
    "public class Holder public owned T: Task? := null; "
    "public constructor(owned x: Task) this.T := x; end constructor; "
    "end Holder; ";

}  // namespace

BEZEL_TEST(task_field_store_vacates_source_7881,
           spec_28_3_k3_1_issue_repro_unmarked_store_of_a_task_local_disposes_once) {
    // The issue's literal repro (r37-rev-taskmove/p/v01_2.bzl).
    auto r = runAotChecked(
        "module M; "
        "public class Holder public owned T: Task? := null; public constructor(owned x: Task) this.T := x; end constructor; end Holder; "
        "public class Program "
        "public static async method W(): Task Console.WriteLine(\"w\"); end W; "
        "public static async method WI(): Task<int> Console.WriteLine(\"wi\"); return 7; end WI; "
        "public static async method Main(): Task "
        " for var i in 0..<100 do "
        "  var t := Program.W(); using var h := new Holder(Program.W()); h.T := t; "
        " end; "
        " Console.WriteLine(\"END\"); end Main; "
        "end Program;");
    expectRunOutput(r, repeat("w\nw\n", 100) + "END\n");
}

BEZEL_TEST(task_field_store_vacates_source_7881,
           spec_28_3_k3_1_unmarked_store_of_a_task_local_in_a_sync_body_disposes_once) {
    auto r = runAotChecked(
        std::string("module M; ") + kHolder +
        "public class Program "
        "public static async method W(): Task Console.WriteLine(\"w\"); end W; "
        "public static method Main(): int "
        " for var i in 0..<100 do "
        "  var t := Program.W(); using var h := new Holder(Program.W()); h.T := t; "
        " end; "
        " Console.WriteLine(\"END\"); return 0; end Main; "
        "end Program;");
    expectRunOutput(r, repeat("w\nw\n", 100) + "END\n");
}

BEZEL_TEST(task_field_store_vacates_source_7881,
           spec_17_8_bang_store_of_a_nullable_task_local_consumes_it_like_the_bare_store) {
    // §17.8: `sink._f := x!;` consumes `x` exactly as `sink._f := x;` does.
    auto r = runAotChecked(
        std::string("module M; ") + kHolder +
        "public class Program "
        "public static async method W(): Task Console.WriteLine(\"w\"); end W; "
        "public static async method Main(): Task "
        " for var i in 0..<100 do "
        "  var t: Task? := Program.W(); using var h := new Holder(Program.W()); h.T := t!; "
        " end; "
        " Console.WriteLine(\"END\"); end Main; "
        "end Program;");
    expectRunOutput(r, repeat("w\nw\n", 100) + "END\n");
}

BEZEL_TEST(task_field_store_vacates_source_7881,
           spec_28_3_k3_1_store_into_this_field_from_a_method_local_disposes_once) {
    // The same K3.1 store through the receiver `this`, from a method body.
    auto r = runAotChecked(
        "module M; "
        "public class Holder public owned T: Task? := null; "
        "  public method Refresh() var t := Program.W(); this.T := t; end Refresh; "
        "end Holder; "
        "public class Program "
        "public static async method W(): Task Console.WriteLine(\"w\"); end W; "
        "public static async method Main(): Task "
        " for var i in 0..<100 do "
        "  using var h := new Holder(); h.Refresh(); h.Refresh(); "
        " end; "
        " Console.WriteLine(\"END\"); end Main; "
        "end Program;");
    expectRunOutput(r, repeat("w\nw\n", 100) + "END\n");
}

BEZEL_TEST(task_field_store_vacates_source_7881,
           spec_28_3_k3_1_store_of_a_task_local_then_await_through_the_field) {
    // After the transfer the FIELD owns the frame; a borrow-pin of it awaits
    // the same task, and the holder alone disposes it.
    auto r = runAotChecked(
        std::string("module M; ") + kHolder +
        "public class Program "
        "public static async method W(): Task Console.WriteLine(\"w\"); end W; "
        "public static async method Main(): Task "
        " for var i in 0..<100 do "
        "  var t := Program.W(); using var h := new Holder(Program.W()); h.T := t; "
        "  using var u := h.T!; await u; "
        " end; "
        " Console.WriteLine(\"END\"); end Main; "
        "end Program;");
    expectRunOutput(r, repeat("w\nw\n", 100) + "END\n");
}

BEZEL_TEST(task_field_store_vacates_source_7881,
           spec_28_3_k3_1_store_of_an_owning_task_local_disposes_its_result_once) {
    // OwningTask<Res>: the unconsumed result is disposed by the task's
    // teardown, exactly once per iteration (one DTOR per produced Res).
    auto r = runAotChecked(
        "module M; "
        "public class Res "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"DTOR\"); end destructor; "
        "end Res; "
        "public class Holder public owned T: OwningTask<Res>? := null; end Holder; "
        "public class Program "
        "public static async method WO(): OwningTask<Res> return new Res(); end WO; "
        "public static async method Main(): Task "
        " for var i in 0..<50 do "
        "  var t := Program.WO(); using var h := new Holder(); h.T := t; "
        " end; "
        " Console.WriteLine(\"END\"); end Main; "
        "end Program;");
    expectRunOutput(r, repeat("DTOR\n", 50) + "END\n");
}
