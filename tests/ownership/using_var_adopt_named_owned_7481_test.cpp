// An UNMARKED `using var x := src;` over a NAMED OWNED class or interface
// binding is a COMPILE ERROR (issue #7481; spec §28.10 *Adopting a named owned
// binding*, §28.12, v0.350).
//
// THE GOVERNING TEXT.
//
//   docs/Bezel_Language_Spec.md (§28.10, v0.350), *Adopting a named owned
//   binding*: "`using var y := move x;` transfers a named owned local or
//   `owned` parameter into a scope-bound binding … An **unmarked**
//   `using var y := x;` is a **compile error** when its right-hand side is a
//   named owned binding whose declared type, nullability stripped, is a class
//   or interface type. This includes the null-forgiving read `x!` …, a
//   parenthesized `(x)` …, the anonymous `using var _ := x;` and
//   `await using var y := x;` (§28.12)."
//
//   The same section, on the meet of an `if` / `??` / `case`: "When **every**
//   arm of the meet … is an unmarked read of a class- or interface-typed named
//   owned binding … the `using var` is **rejected** on that paragraph's
//   ground", and, on the other side of the line: "an unmarked read of a
//   **borrow-state** named binding (a plain parameter, a borrowing alias or
//   loop variable, a captured owned binding inside a closure)" is a borrow
//   and the `using var` pins it (no disposal obligation).
//
// THE BUG (#7481). The compiler read the unmarked shape as a borrow-pin (the
// v0.298 wording) and compiled it: the declaration neither adopted nor
// rejected, and the program's disposal behaviour depended on which of the two
// readings the reader had in mind. The grid `scripts/using_heads_grid.py`
// recorded 17 cells accepting the shape.
//
// THE ORACLE. Each reject row pins the SPECIFIC diagnostic (the declaration's
// own "is ambiguous" text and its `move` remedy), not a bare compile failure.
// Each accept row (the over-reject mirror) runs under ASan/LSan
// (`runAotChecked`) and pins the printed disposal ORDER, so a doubled and a
// suppressed disposal are distinguishable from the spec's answer.
//
// per §28.10, §28.12, §28.3, v0.350

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {

// A destructor that PRINTS, so the number and the position of disposals are
// observable.
const char* kDecls =
    "public class Res "
    "  public constructor() end constructor; "
    "  destructor Console.WriteLine(\"DTOR\"); end destructor; "
    "end Res; "
    "public interface IThing end IThing; "
    "public class Thing: IThing "
    "  public constructor() end constructor; "
    "  destructor Console.WriteLine(\"DTOR\"); end destructor; "
    "end Thing; ";

std::string mainProg(const std::string& body) {
    return std::string("module M; ") + kDecls +
           "public class Program "
           "  public static method Main(): int " + body +
           "    Console.WriteLine(\"END\"); return 0; "
           "  end Main; "
           "end Program;";
}

void expectRejectedAmbiguous(const JitResult& r) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "is ambiguous"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "write `move "));
}

void expectRunOutput(const JitResult& r, const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// --- REJECT: the adopted shapes (§28.10 *Adopting a named owned binding*) ----

BEZEL_TEST(ownership_using_var_adopt_named_owned_7481,
           spec_28_10_unmarked_using_var_over_an_owned_local_is_rejected) {
    expectRejectedAmbiguous(runAotChecked(mainProg(
        "var src := new Res(); using var x := src; ")));
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_7481,
           spec_28_10_unmarked_using_var_over_an_owned_param_is_rejected) {
    expectRejectedAmbiguous(runAotChecked(
        std::string("module M; ") + kDecls +
        "public class Util "
        "  public static method Take(owned src: Res) using var x := src; end Take; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    Util.Take(new Res()); return 0; "
        "  end Main; "
        "end Program;"));
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_7481,
           spec_28_10_unmarked_using_var_over_a_null_forgiving_read_is_rejected) {
    // The null-forgiving read `x!` is classified as the bare `x` is (§17.8).
    expectRejectedAmbiguous(runAotChecked(mainProg(
        "var src := new Res(); using var x := src!; ")));
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_7481,
           spec_28_10_unmarked_using_var_over_a_parenthesized_read_is_rejected) {
    // A parenthesized `(x)` groups and introduces no expression of its own.
    expectRejectedAmbiguous(runAotChecked(mainProg(
        "var src := new Res(); using var x := (src); ")));
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_7481,
           spec_28_10_anonymous_using_var_over_an_owned_local_is_rejected) {
    expectRejectedAmbiguous(runAotChecked(mainProg(
        "var src := new Res(); using var _ := src; ")));
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_7481,
           spec_28_12_await_using_var_over_an_owned_local_is_rejected) {
    // §28.12: `await using var` takes §28.10's admission unchanged.
    expectRejectedAmbiguous(runAotChecked(
        std::string("module M; ") + kDecls +
        "public class Program "
        "  public static async method Main(): Task "
        "    var src := new Res(); "
        "    await using var x := src; "
        "    Console.WriteLine(\"END\"); "
        "  end Main; "
        "end Program;"));
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_7481,
           spec_28_10_unmarked_using_var_over_an_owned_interface_binding_is_rejected) {
    expectRejectedAmbiguous(runAotChecked(mainProg(
        "var src: IThing := new Thing(); using var x := src; ")));
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_7481,
           spec_28_10_unmarked_using_var_over_an_owned_nullable_binding_is_rejected) {
    // "nullability stripped": `Res?` is a class type for this rule.
    expectRejectedAmbiguous(runAotChecked(mainProg(
        "var src: Res? := new Res(); using var x := src!; ")));
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_7481,
           spec_28_10_if_expression_with_every_arm_a_named_owned_read_is_rejected) {
    expectRejectedAmbiguous(runAotChecked(mainProg(
        "var a := new Res(); var b := new Res(); var c := true; "
        "using var x := (if c then a else b); ")));
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_7481,
           spec_28_10_coalesce_with_every_arm_a_named_owned_read_is_rejected) {
    expectRejectedAmbiguous(runAotChecked(mainProg(
        "var a: Res? := new Res(); var b := new Res(); "
        "using var x := a ?? b; ")));
}

// --- ACCEPT: the over-reject mirror (each must stay a clean, ASan-clean run) --

BEZEL_TEST(ownership_using_var_adopt_named_owned_7481,
           spec_28_10_marked_adoption_of_an_owned_local_is_accepted) {
    // `using var y := move x;`: `y` takes the obligation; the disposal happens
    // at the end of Main, after END.
    expectRunOutput(runAotChecked(mainProg(
        "var src := new Res(); using var x := move src; ")),
        "END\nDTOR\n");
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_7481,
           spec_28_10_marked_adoption_of_an_owned_param_is_accepted) {
    // The callee's `using var` carries the obligation: DTOR prints when Take
    // returns, before the caller's END.
    expectRunOutput(runAotChecked(
        std::string("module M; ") + kDecls +
        "public class Util "
        "  public static method Take(owned src: Res) using var x := move src; end Take; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    Util.Take(new Res()); Console.WriteLine(\"END\"); return 0; "
        "  end Main; "
        "end Program;"),
        "DTOR\nEND\n");
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_7481,
           spec_28_10_view_bindings_of_an_owned_local_are_accepted) {
    // The spec's remedies for a view: `var` and `const`. Exactly one disposal,
    // by the owner `src`, at the end of Main.
    expectRunOutput(runAotChecked(mainProg(
        "var src := new Res(); var v := src; const w := src; ")),
        "END\nDTOR\n");
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_7481,
           spec_28_10_using_var_over_a_borrowing_alias_is_a_pin_and_disposes_nothing) {
    // "an unmarked read of a **borrow-state** named binding (… a borrowing
    // alias …)" is a borrow: the pin disposes nothing, the owner disposes once.
    expectRunOutput(runAotChecked(mainProg(
        "var src := new Res(); var a := src; using var x := a; ")),
        "END\nDTOR\n");
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_7481,
           spec_28_10_using_var_over_a_plain_parameter_is_a_pin_and_disposes_nothing) {
    // A plain (non-`owned`) parameter is borrow-state; the caller disposes.
    expectRunOutput(runAotChecked(
        std::string("module M; ") + kDecls +
        "public class Util "
        "  public static method Look(src: Res) using var x := src; end Look; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    var r := new Res(); Util.Look(r); Console.WriteLine(\"END\"); return 0; "
        "  end Main; "
        "end Program;"),
        "END\nDTOR\n");
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_7481,
           spec_28_3_28_10_captured_owned_binding_in_a_closure_is_borrow_state) {
    // "a captured owned binding inside a closure" is borrow-state: the
    // unmarked `using var` over it is a pin, not the rejected adoption.
    expectRunOutput(runAotChecked(mainProg(
        "var src := new Res(); "
        "var f: () -> int := lambda() => do using var y := src; return 1; end; "
        "var r := f(); ")),
        "END\nDTOR\n");
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_7481,
           spec_28_10_if_expression_with_marked_arms_is_accepted) {
    // The rule's own remedy: `(if c then move a else move b)`. The selected
    // arm is adopted; the other binding keeps its obligation: two disposals.
    expectRunOutput(runAotChecked(mainProg(
        "var a := new Res(); var b := new Res(); var c := true; "
        "using var x := (if c then move a else move b); ")),
        "END\nDTOR\nDTOR\n");
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_7481,
           spec_28_10_fresh_producer_is_adopted_not_rejected) {
    expectRunOutput(runAotChecked(mainProg(
        "using var x := new Res(); ")),
        "END\nDTOR\n");
}
