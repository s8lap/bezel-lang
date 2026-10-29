// An UNMARKED `using var t2 := t;` over a NAMED OWNED `Task`-family binding
// (`Task`, `Task<T>`, `OwningTask<T>`) is a COMPILE ERROR, exactly as over any
// other class-typed named owned binding (issues #7686, #7614; spec §28.10
// *Adopting a named owned binding*, §28.12, §17.13, v0.350).
//
// THE GOVERNING TEXT.
//
//   docs/Bezel_Language_Spec.md (§28.10, v0.350): "An **unmarked**
//   `using var y := x;` is a **compile error** when its right-hand side is a
//   named owned binding whose declared type, nullability stripped, is a class
//   or interface type." The rule names no Task exception. The one Task
//   sentence in the subsection only ORDERS the diagnostics: "Over a
//   `Task`-family source the anonymous form is reported under §17.13's
//   rejection (axiom A6) first, and `using var _ := move x;` is never offered
//   as its remedy." `Task` is a class (stdlib §6.7 `public class Task`), so the
//   named form `using var t2 := t;` is the covered, ambiguous spelling, and
//   `using var t2 := move t;` is its remedy (the named `using var t := call();`
//   stays "the unsupervised idiom", §28.10 *The anonymous binding*).
//
// THE BUG. isNamedOwnedClassOrInterfaceBinding (src/ownership/Ownership.cpp)
// answered `false` for every `Task`-family head, on a source comment that
// misquoted §28.10 as excluding the family ("NOT covered, by the spec's own
// text: ... and the `Task` family (axiom A6 governs it)"), so the unmarked
// rebind of an owned Task was silently accepted.
//
// THE ORACLE. Each reject row pins the declaration's own "is ambiguous" text
// and its `move` remedy through the AOT driver (a bare compile failure would
// not distinguish this rule from any other). The accept rows that run
// (a view binding, a borrow-state pin) run under ASan/LSan (`runAotChecked`)
// and pin the printed order. The `move t` adoption rows are pinned at the
// OWNERSHIP pass (`parseAndCheckOwnership`): running a program that moves a
// named Task local hangs at scope end, an independent codegen defect reported
// with this change (it is not this rule), so those rows assert the pass's
// verdict only.
//
// per §28.10, §28.12, §17.13, §30.3, v0.350

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// `W`: a Task producer; `WI`: a Task<int>; `WO`: an OwningTask<Res>.
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

// An async Main whose body is `body`; the producers sit in the same class.
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

void expectRejectedAmbiguous(const JitResult& r) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "is ambiguous"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "write `move t`"));
}

void expectRunOutput(const JitResult& r, const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// --- REJECT: the unmarked rebind of an owned Task-family binding -------------

BEZEL_TEST(ownership_using_var_adopt_named_owned_task_7686,
           spec_28_10_unmarked_using_var_over_an_owned_task_is_rejected_7614) {
    // The #7614 repro: `var t := W(); using var t2 := t;`, then an await of
    // the new binding.
    expectRejectedAmbiguous(runAotChecked(asyncMain(
        "var t := Program.W(); using var t2 := t; await t2; ")));
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_task_7686,
           spec_28_10_unmarked_using_var_over_an_owned_task_then_await_of_source_is_rejected) {
    // The #7686 repro shape: the await names the SOURCE after the rebind.
    expectRejectedAmbiguous(runAotChecked(asyncMain(
        "var t := Program.W(); using var t2 := t; await t; ")));
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_task_7686,
           spec_28_10_unmarked_using_var_over_an_owned_task_of_t_is_rejected) {
    expectRejectedAmbiguous(runAotChecked(asyncMain(
        "var t := Program.WI(); using var t2 := t; var v := await t2; ")));
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_task_7686,
           spec_28_10_unmarked_using_var_over_an_owned_owningtask_is_rejected) {
    expectRejectedAmbiguous(runAotChecked(asyncMain(
        "var t := Program.WO(); using var t2 := t; using var r := await t2; ")));
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_task_7686,
           spec_28_10_unmarked_using_var_over_a_nullable_task_null_forgiving_read_is_rejected) {
    // "nullability stripped": `Task?` is a class type for this rule, `t!`
    // classified as the bare `t` (§17.8).
    expectRejectedAmbiguous(runAotChecked(asyncMain(
        "var t: Task? := Program.W(); using var t2 := t!; await t2; ")));
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_task_7686,
           spec_28_10_unmarked_using_var_over_an_owned_task_in_a_sync_body_is_rejected) {
    // The rule does not depend on the enclosing body being async.
    expectRejectedAmbiguous(runAotChecked(syncMain(
        "var t := Program.W(); using var t2 := t; ")));
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_task_7686,
           spec_28_10_unmarked_using_var_over_an_owned_task_parameter_in_an_async_method_is_rejected) {
    // An `owned` Task parameter of an async method is a named owned binding
    // (§28.10's second bullet).
    expectRejectedAmbiguous(runAotChecked(
        std::string("module M; ") + kDecls +
        "public class Program " + kProducers +
        "  public static async method H(owned t: Task): Task "
        "    using var t2 := t; await t2; "
        "  end H; "
        "  public static async method Main(): Task "
        "    await H(Program.W()); Console.WriteLine(\"END\"); "
        "  end Main; "
        "end Program;"));
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_task_7686,
           spec_28_12_await_using_var_over_an_owned_task_is_rejected) {
    // §28.12: `await using var` takes §28.10's admission unchanged.
    expectRejectedAmbiguous(runAotChecked(asyncMain(
        "var t := Program.W(); await using var t2 := t; ")));
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_task_7686,
           spec_28_10_if_expression_with_every_arm_a_named_owned_task_is_rejected) {
    // The meet rule: every arm an unmarked read of an owned class-typed
    // binding. The diagnostic names the first such arm.
    const auto r = runAotChecked(asyncMain(
        "var a := Program.W(); var b := Program.W(); var c := true; "
        "using var t2 := (if c then a else b); await t2; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "is ambiguous"));
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_task_7686,
           spec_28_10_anonymous_using_var_over_an_owned_task_is_reported_under_17_13_only) {
    // "Over a `Task`-family source the anonymous form is reported under
    // §17.13's rejection (axiom A6) first, and `using var _ := move x;` is
    // never offered as its remedy": A6's diagnostic is the report, and the
    // ambiguity text (whose remedy names `move`) is not issued.
    const auto r = runAotChecked(asyncMain(
        "var t := Program.W(); using var _ := t; await t; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "cannot bind a `Task` to the anonymous `using var _`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "is ambiguous"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "write `move t`"));
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_task_7686,
           spec_28_10_await_of_a_task_after_the_marked_adoption_is_use_after_move) {
    // The remedy's semantics: `move t` leaves `t` moved-from, so an await of
    // `t` afterwards is the ordinary §28.3 use-after-move error (the await
    // of the new binding, `t2`, is the valid spelling).
    const auto r = parseAndCheckOwnership(asyncMain(
        "var t := Program.W(); using var t2 := move t; await t; "));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "already moved out"));
}

// --- ACCEPT: the marked adoption and the views (the over-reject mirror) ------

BEZEL_TEST(ownership_using_var_adopt_named_owned_task_7686,
           spec_28_10_marked_adoption_of_an_owned_task_is_accepted) {
    auto r = parseAndCheckOwnership(asyncMain(
        "var t := Program.W(); using var t2 := move t; await t2; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_task_7686,
           spec_28_10_marked_adoption_of_an_owned_task_of_t_is_accepted) {
    auto r = parseAndCheckOwnership(asyncMain(
        "var t := Program.WI(); using var t2 := move t; var v := await t2; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_task_7686,
           spec_28_10_marked_adoption_of_an_owned_owningtask_is_accepted) {
    auto r = parseAndCheckOwnership(asyncMain(
        "var t := Program.WO(); using var t2 := move t; "
        "using var r := await t2; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_task_7686,
           spec_28_10_named_using_var_over_a_task_call_is_still_the_unsupervised_idiom) {
    // "the **named** `using var t := call();` remains the unsupervised idiom":
    // the initializer is a call (a fresh production), not a named read.
    expectRunOutput(runAotChecked(asyncMain(
        "using var t := Program.W(); await t; ")),
        "w\nEND\n");
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_task_7686,
           spec_28_10_view_binding_of_an_owned_task_is_accepted) {
    // The spec's view remedy: `var`. The owner `t` keeps the obligation.
    expectRunOutput(runAotChecked(asyncMain(
        "var t := Program.W(); var t2 := t; await t2; ")),
        "w\nEND\n");
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_task_7686,
           spec_28_10_using_var_over_a_plain_task_parameter_is_a_pin_and_disposes_nothing) {
    // A plain (non-`owned`) Task parameter is borrow-state: the unmarked
    // `using var` over it is the borrow-pin, not the rejected adoption.
    expectRunOutput(runAotChecked(
        std::string("module M; ") + kDecls +
        "public class Program " + kProducers +
        "  public static async method H(t: Task): Task "
        "    using var t2 := t; await t2; "
        "  end H; "
        "  public static async method Main(): Task "
        "    using var t := Program.W(); await H(t); "
        "    Console.WriteLine(\"END\"); "
        "  end Main; "
        "end Program;"),
        "w\nEND\n");
}
