// Spec-derived regression tests for #6182 — §35.3, §15.3, §20 Ownership,
// §35.4 (v0.338).
//
//   §35.3: "a move-only value payload argument transfers by value-move
//   (§15.3, K3.12), the argument expression moved-from after the
//   construction".
//   §15.3 Parameter passing: "for a move-only V, it is by value-move
//   (transfer ownership). The caller's argument expression is in moved-from
//   state after the call."
//   §20 Ownership: "in an owned-consuming position — a binding, an
//   assignment, a plain or `owned` argument, a `return`, an owning slot — the
//   **selected** branch's value moves".
//   §35.4: "a move-only value carrying no borrows may be a class field (the
//   class's destructor disposes it)".
//
// The ownership pass already treats these positions as moves (a second read
// of the argument is rejected). Codegen did not: an owning choice named as a
// case-constructor argument or as a constructor argument was copied and the
// source binding's scope-exit disposal still ran, so the payload was disposed
// twice — by the new owner and by the old binding (ASan heap-use-after-free).
// An if-expression branch in a consuming position had the same defect for the
// branch that was selected. Separately, the owned-choice field store's
// self-assignment identity guard compared old and new payload with a pointer
// `icmp`; for a payload that is itself an owning choice that is an aggregate
// compare the verifier refuses ("Invalid operand types for ICmp instruction
// %ocfs.ptrSame"), so every such class did not compile.
//
// Every run row uses the checked oracle (`runAotChecked`, ASan/LSan), loops
// 200 times and asserts stdout: `drops=200` is one disposal per iteration, a
// double disposal is a sanitizer report and a missed one a leak report.
//
// Shapes deliberately not covered (open, separate issues): a by-value owning
// choice a callee never consumes (#6168) — including a constructor that
// ignores such a parameter; a `W<Own>` class field whose declared payload
// names the parameter inside another generic (#6181).
//
// per §35.3, §15.3, §20, §35.4, §28.3, v0.338

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// `Own` is an owning choice (an `owned` class payload); `NG` holds an `Own`
// by value and `NG2` an `NG` — owning "transitively" (§35.4 rule 1), with no
// generics involved. `InnerU<T>` / `W1<T>` are the generic counterparts.
const char* const kPrelude = R"(module M;
class Stats public static Drops: int := 0; end Stats;
class Disp
  public N: int;
  public constructor(n: int) N := n; end constructor;
  destructor Stats.Drops := Stats.Drops + 1; end destructor;
end Disp;
choice Own Held(owned r: Disp), Empty; end Own;
choice NG Has(v: Own), No; end NG;
choice NG2 Has(v: NG), No; end NG2;
choice InnerU<T> Has(v: T), No; end InnerU;
choice W1<T> Ok(v: InnerU<T>), No; end W1;
)";

// `decls`, then `class Program` with `helpers` and a `Step(k)` whose body is
// `body`, called 200 times; prints `200 drops=<destructor runs>`.
std::string program(const std::string& decls, const std::string& helpers,
                    const std::string& body) {
    return std::string(kPrelude) + decls + "class Program\n" + helpers +
           "  static method Step(k: int): int\n" + body + R"(
    return 1;
  end Step;
  public static method Main(): int
    var total := 0;
    for var k in 0..<200 do total := total + Program.Step(k); end;
    Console.WriteLine(total.ToString() + " drops=" + Stats.Drops.ToString());
    return 0;
  end Main;
end Program;
)";
}

// One owner per iteration, disposed exactly once.
void expectDisposedOncePerIteration(const std::string& source) {
    JitResult r = runAotChecked(source);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=200\n"));
}

}  // namespace

// The issue's first repro, verbatim: `new Bx(w)` moves `w` into the constructor
// parameter, whose store moves it into the field; `Bx`'s destructor is the
// only disposer. Before the fix `Step` disposed `w` again.
BEZEL_TEST(owning_choice_argument_move_6182,
           issue_repro_1_owning_choice_moved_into_class_field_through_constructor_parameter_spec_15_3) {
    expectDisposedOncePerIteration(program(
        R"(class Bx public V: Own; public constructor(v: Own) V := v; end constructor; end Bx;
)",
        "",
        R"(    var w := Own.Held(new Disp(k));
    var x1 := new Bx(w);)"));
}

// The issue's second repro, verbatim: the field's payload is itself an owning
// choice. Before the fix: malformed IR (`%ocfs.ptrSame = icmp eq %choice.Own`).
BEZEL_TEST(owning_choice_argument_move_6182,
           issue_repro_2_nested_owning_choice_class_field_compiles_spec_35_4) {
    expectDisposedOncePerIteration(program(
        R"(choice W3 Ok(v: Own), No; end W3;
class Bx public V: W3; public constructor(v: W3) V := v; end constructor; end Bx;
)",
        "",
        R"(    var w := W3.Ok(Own.Held(new Disp(k)));
    var x1 := new Bx(w);)"));
}

// The issue's first repro with the generic `Inner<Disp>` it names.
BEZEL_TEST(owning_choice_argument_move_6182,
           issue_repro_1_generic_owned_payload_spec_15_3) {
    expectDisposedOncePerIteration(program(
        R"(choice Inner<T> Has(owned v: T), No; end Inner;
class Bx public V: Inner<Disp>; public constructor(v: Inner<Disp>) V := v; end constructor; end Bx;
)",
        "",
        R"(    var w := Inner<Disp>.Has(new Disp(k));
    var x1 := new Bx(w);)"));
}

// A named owning choice as a case-constructor argument is moved into the
// new value (§35.3); the binding it named is not disposed at scope exit.
// `NG.Has(...)` — local binding.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG_local_binding_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := Own.Held(new Disp(k));
    var w1 := NG.Has(w0);)"));
}

// `NG.Has(...)` — typed local binding.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG_typed_local_binding_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0: Own := Own.Held(new Disp(k));
    var w1: NG := NG.Has(w0);)"));
}

// `NG.Has(...)` — using binding.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG_using_binding_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := Own.Held(new Disp(k));
    using var w1 := NG.Has(w0);)"));
}

// `NG.Has(...)` — returned directly.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG_returned_directly_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        R"(  static method Make(k: int): NG var w0 := Own.Held(new Disp(k)); return NG.Has(w0); end Make;
)",
        R"(    var w1 := Program.Make(k);)"));
}

// `NG.Has(...)` — consumed by owning match.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG_consumed_by_owning_match_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := Own.Held(new Disp(k));
    var w1 := NG.Has(w0);
    case move w1 of _ => do end end case;)"));
}

// `NG.Has(...)` — passed through a call.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG_passed_through_a_call_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        R"(  static method Echo(x: NG): NG return x; end Echo;
)",
        R"(    var w0 := Own.Held(new Disp(k));
    var w1 := Program.Echo(NG.Has(w0));)"));
}

// `NG.Has(...)` — if expression branch selected.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG_if_expression_branch_selected_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := Own.Held(new Disp(k));
    var w1 := NG.Has((if k >= 0 then w0 else Own.Empty));)"));
}

// `NG.Has(...)` — if expression branch alternating.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG_if_expression_branch_alternating_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := Own.Held(new Disp(k));
    var w1 := NG.Has((if k mod 2 = 0 then w0 else Own.Empty));)"));
}

// `NG.Has(...)` — assigned over a binding.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG_assigned_over_a_binding_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w1 := NG.No;
    var w0 := Own.Held(new Disp(k));
    w1 := NG.Has(w0);)"));
}

// `NG.Has(...)` — returned through a binding.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG_returned_through_a_binding_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        R"(  static method Make(k: int): NG var w0 := Own.Held(new Disp(k)); var w1 := NG.Has(w0); return w1; end Make;
)",
        R"(    var w1 := Program.Make(k);)"));
}

// `NG.Has(...)` — in constructor from local into field.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG_in_constructor_from_local_into_field_spec_35_3) {
    expectDisposedOncePerIteration(program(
        R"(class Holder public F: NG; public constructor(k: int) var w0 := Own.Held(new Disp(k)); F := NG.Has(w0); end constructor; end Holder;
)",
        "",
        R"(    using var h := new Holder(k);)"));
}

// `NG.Has(...)` — in constructor from parameter into field.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG_in_constructor_from_parameter_into_field_spec_35_3) {
    expectDisposedOncePerIteration(program(
        R"(class Holder public F: NG; public constructor(x: Own) F := NG.Has(x); end constructor; end Holder;
)",
        "",
        R"(    var w0 := Own.Held(new Disp(k));
    using var h := new Holder(w0);)"));
}

// `NG.Has(...)` — in constructor from temporary into field.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG_in_constructor_from_temporary_into_field_spec_35_3) {
    expectDisposedOncePerIteration(program(
        R"(class Holder public F: NG; public constructor(k: int) F := NG.Has(Own.Held(new Disp(k))); end constructor; end Holder;
)",
        "",
        R"(    using var h := new Holder(k);)"));
}

// `NG.Has(...)` — assigned into field.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG_assigned_into_field_spec_35_3) {
    expectDisposedOncePerIteration(program(
        R"(class Holder public F: NG; public constructor() F := NG.No; end constructor; end Holder;
)",
        "",
        R"(    using var h := new Holder();
    var w0 := Own.Held(new Disp(k));
    h.F := NG.Has(w0);)"));
}

// `NG.Has(...)` — temporary assigned into field.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG_temporary_assigned_into_field_spec_35_3) {
    expectDisposedOncePerIteration(program(
        R"(class Holder public F: NG; public constructor() F := NG.No; end constructor; end Holder;
)",
        "",
        R"(    using var h := new Holder();
    h.F := NG.Has(Own.Held(new Disp(k)));)"));
}

// `NG2.Has(...)` — local binding.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG2_local_binding_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := NG.Has(Own.Held(new Disp(k)));
    var w1 := NG2.Has(w0);)"));
}

// `NG2.Has(...)` — typed local binding.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG2_typed_local_binding_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0: NG := NG.Has(Own.Held(new Disp(k)));
    var w1: NG2 := NG2.Has(w0);)"));
}

// `NG2.Has(...)` — using binding.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG2_using_binding_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := NG.Has(Own.Held(new Disp(k)));
    using var w1 := NG2.Has(w0);)"));
}

// `NG2.Has(...)` — returned directly.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG2_returned_directly_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        R"(  static method Make(k: int): NG2 var w0 := NG.Has(Own.Held(new Disp(k))); return NG2.Has(w0); end Make;
)",
        R"(    var w1 := Program.Make(k);)"));
}

// `NG2.Has(...)` — consumed by owning match.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG2_consumed_by_owning_match_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := NG.Has(Own.Held(new Disp(k)));
    var w1 := NG2.Has(w0);
    case move w1 of _ => do end end case;)"));
}

// `NG2.Has(...)` — passed through a call.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG2_passed_through_a_call_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        R"(  static method Echo(x: NG2): NG2 return x; end Echo;
)",
        R"(    var w0 := NG.Has(Own.Held(new Disp(k)));
    var w1 := Program.Echo(NG2.Has(w0));)"));
}

// `NG2.Has(...)` — if expression branch selected.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG2_if_expression_branch_selected_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := NG.Has(Own.Held(new Disp(k)));
    var w1 := NG2.Has((if k >= 0 then w0 else NG.No));)"));
}

// `NG2.Has(...)` — if expression branch alternating.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG2_if_expression_branch_alternating_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := NG.Has(Own.Held(new Disp(k)));
    var w1 := NG2.Has((if k mod 2 = 0 then w0 else NG.No));)"));
}

// `NG2.Has(...)` — assigned over a binding.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG2_assigned_over_a_binding_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w1 := NG2.No;
    var w0 := NG.Has(Own.Held(new Disp(k)));
    w1 := NG2.Has(w0);)"));
}

// `NG2.Has(...)` — returned through a binding.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG2_returned_through_a_binding_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        R"(  static method Make(k: int): NG2 var w0 := NG.Has(Own.Held(new Disp(k))); var w1 := NG2.Has(w0); return w1; end Make;
)",
        R"(    var w1 := Program.Make(k);)"));
}

// `NG2.Has(...)` — in constructor from local into field.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG2_in_constructor_from_local_into_field_spec_35_3) {
    expectDisposedOncePerIteration(program(
        R"(class Holder public F: NG2; public constructor(k: int) var w0 := NG.Has(Own.Held(new Disp(k))); F := NG2.Has(w0); end constructor; end Holder;
)",
        "",
        R"(    using var h := new Holder(k);)"));
}

// `NG2.Has(...)` — in constructor from parameter into field.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG2_in_constructor_from_parameter_into_field_spec_35_3) {
    expectDisposedOncePerIteration(program(
        R"(class Holder public F: NG2; public constructor(x: NG) F := NG2.Has(x); end constructor; end Holder;
)",
        "",
        R"(    var w0 := NG.Has(Own.Held(new Disp(k)));
    using var h := new Holder(w0);)"));
}

// `NG2.Has(...)` — in constructor from temporary into field.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG2_in_constructor_from_temporary_into_field_spec_35_3) {
    expectDisposedOncePerIteration(program(
        R"(class Holder public F: NG2; public constructor(k: int) F := NG2.Has(NG.Has(Own.Held(new Disp(k)))); end constructor; end Holder;
)",
        "",
        R"(    using var h := new Holder(k);)"));
}

// `NG2.Has(...)` — assigned into field.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG2_assigned_into_field_spec_35_3) {
    expectDisposedOncePerIteration(program(
        R"(class Holder public F: NG2; public constructor() F := NG2.No; end constructor; end Holder;
)",
        "",
        R"(    using var h := new Holder();
    var w0 := NG.Has(Own.Held(new Disp(k)));
    h.F := NG2.Has(w0);)"));
}

// `NG2.Has(...)` — temporary assigned into field.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_NG2_temporary_assigned_into_field_spec_35_3) {
    expectDisposedOncePerIteration(program(
        R"(class Holder public F: NG2; public constructor() F := NG2.No; end constructor; end Holder;
)",
        "",
        R"(    using var h := new Holder();
    h.F := NG2.Has(NG.Has(Own.Held(new Disp(k))));)"));
}

// `InnerU<Own>.Has(...)` — local binding.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_InnerU_Own_local_binding_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := Own.Held(new Disp(k));
    var w1 := InnerU<Own>.Has(w0);)"));
}

// `InnerU<Own>.Has(...)` — typed local binding.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_InnerU_Own_typed_local_binding_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0: Own := Own.Held(new Disp(k));
    var w1: InnerU<Own> := InnerU<Own>.Has(w0);)"));
}

// `InnerU<Own>.Has(...)` — using binding.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_InnerU_Own_using_binding_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := Own.Held(new Disp(k));
    using var w1 := InnerU<Own>.Has(w0);)"));
}

// `InnerU<Own>.Has(...)` — returned directly.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_InnerU_Own_returned_directly_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        R"(  static method Make(k: int): InnerU<Own> var w0 := Own.Held(new Disp(k)); return InnerU<Own>.Has(w0); end Make;
)",
        R"(    var w1 := Program.Make(k);)"));
}

// `InnerU<Own>.Has(...)` — consumed by owning match.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_InnerU_Own_consumed_by_owning_match_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := Own.Held(new Disp(k));
    var w1 := InnerU<Own>.Has(w0);
    case move w1 of _ => do end end case;)"));
}

// `InnerU<Own>.Has(...)` — passed through a call.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_InnerU_Own_passed_through_a_call_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        R"(  static method Echo(x: InnerU<Own>): InnerU<Own> return x; end Echo;
)",
        R"(    var w0 := Own.Held(new Disp(k));
    var w1 := Program.Echo(InnerU<Own>.Has(w0));)"));
}

// `InnerU<Own>.Has(...)` — if expression branch selected.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_InnerU_Own_if_expression_branch_selected_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := Own.Held(new Disp(k));
    var w1 := InnerU<Own>.Has((if k >= 0 then w0 else Own.Empty));)"));
}

// `InnerU<Own>.Has(...)` — if expression branch alternating.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_InnerU_Own_if_expression_branch_alternating_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := Own.Held(new Disp(k));
    var w1 := InnerU<Own>.Has((if k mod 2 = 0 then w0 else Own.Empty));)"));
}

// `InnerU<Own>.Has(...)` — in constructor from local into field.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_InnerU_Own_in_constructor_from_local_into_field_spec_35_3) {
    expectDisposedOncePerIteration(program(
        R"(class Holder public F: InnerU<Own>; public constructor(k: int) var w0 := Own.Held(new Disp(k)); F := InnerU<Own>.Has(w0); end constructor; end Holder;
)",
        "",
        R"(    using var h := new Holder(k);)"));
}

// `InnerU<Own>.Has(...)` — in constructor from parameter into field.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_InnerU_Own_in_constructor_from_parameter_into_field_spec_35_3) {
    expectDisposedOncePerIteration(program(
        R"(class Holder public F: InnerU<Own>; public constructor(x: Own) F := InnerU<Own>.Has(x); end constructor; end Holder;
)",
        "",
        R"(    var w0 := Own.Held(new Disp(k));
    using var h := new Holder(w0);)"));
}

// `InnerU<Own>.Has(...)` — in constructor from temporary into field.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_InnerU_Own_in_constructor_from_temporary_into_field_spec_35_3) {
    expectDisposedOncePerIteration(program(
        R"(class Holder public F: InnerU<Own>; public constructor(k: int) F := InnerU<Own>.Has(Own.Held(new Disp(k))); end constructor; end Holder;
)",
        "",
        R"(    using var h := new Holder(k);)"));
}

// `InnerU<Own>.Has(...)` — assigned into field.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_InnerU_Own_assigned_into_field_spec_35_3) {
    expectDisposedOncePerIteration(program(
        R"(class Holder public F: InnerU<Own>; public constructor() F := InnerU<Own>.No; end constructor; end Holder;
)",
        "",
        R"(    using var h := new Holder();
    var w0 := Own.Held(new Disp(k));
    h.F := InnerU<Own>.Has(w0);)"));
}

// `InnerU<Own>.Has(...)` — temporary assigned into field.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_InnerU_Own_temporary_assigned_into_field_spec_35_3) {
    expectDisposedOncePerIteration(program(
        R"(class Holder public F: InnerU<Own>; public constructor() F := InnerU<Own>.No; end constructor; end Holder;
)",
        "",
        R"(    using var h := new Holder();
    h.F := InnerU<Own>.Has(Own.Held(new Disp(k)));)"));
}

// `W1<Own>.Ok(...)` — local binding.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_W1_Own_local_binding_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := InnerU<Own>.Has(Own.Held(new Disp(k)));
    var w1 := W1<Own>.Ok(w0);)"));
}

// `W1<Own>.Ok(...)` — typed local binding.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_W1_Own_typed_local_binding_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0: InnerU<Own> := InnerU<Own>.Has(Own.Held(new Disp(k)));
    var w1: W1<Own> := W1<Own>.Ok(w0);)"));
}

// `W1<Own>.Ok(...)` — using binding.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_W1_Own_using_binding_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := InnerU<Own>.Has(Own.Held(new Disp(k)));
    using var w1 := W1<Own>.Ok(w0);)"));
}

// `W1<Own>.Ok(...)` — returned directly.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_W1_Own_returned_directly_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        R"(  static method Make(k: int): W1<Own> var w0 := InnerU<Own>.Has(Own.Held(new Disp(k))); return W1<Own>.Ok(w0); end Make;
)",
        R"(    var w1 := Program.Make(k);)"));
}

// `W1<Own>.Ok(...)` — consumed by owning match.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_W1_Own_consumed_by_owning_match_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := InnerU<Own>.Has(Own.Held(new Disp(k)));
    var w1 := W1<Own>.Ok(w0);
    case move w1 of _ => do end end case;)"));
}

// `W1<Own>.Ok(...)` — passed through a call.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_W1_Own_passed_through_a_call_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        R"(  static method Echo(x: W1<Own>): W1<Own> return x; end Echo;
)",
        R"(    var w0 := InnerU<Own>.Has(Own.Held(new Disp(k)));
    var w1 := Program.Echo(W1<Own>.Ok(w0));)"));
}

// `W1<Own>.Ok(...)` — if expression branch selected.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_W1_Own_if_expression_branch_selected_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := InnerU<Own>.Has(Own.Held(new Disp(k)));
    var w1 := W1<Own>.Ok((if k >= 0 then w0 else InnerU<Own>.No));)"));
}

// `W1<Own>.Ok(...)` — if expression branch alternating.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_W1_Own_if_expression_branch_alternating_spec_35_3) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := InnerU<Own>.Has(Own.Held(new Disp(k)));
    var w1 := W1<Own>.Ok((if k mod 2 = 0 then w0 else InnerU<Own>.No));)"));
}

// §20 Ownership: the SELECTED branch's value moves. `w0` is moved on even
// iterations and still owned by its binding on odd ones; either way one
// disposal.
BEZEL_TEST(owning_choice_argument_move_6182,
           if_expression_branch_bound_to_a_typed_local_spec_20) {
    expectDisposedOncePerIteration(program(
        "",
        "",
        R"(    var w0 := Own.Held(new Disp(k));
    var w1: Own := (if k mod 2 = 0 then w0 else Own.Empty);)"));
}

// Nested if-expressions: each binding is moved only on the path that selects it.
BEZEL_TEST(owning_choice_argument_move_6182,
           nested_if_expression_branches_bound_to_a_typed_local_spec_20) {
    // Two owners per iteration here, so 400 disposals.
    JitResult r = runAotChecked(program(
        "",
        "",
        R"(    var w0 := Own.Held(new Disp(k));
    var w2 := Own.Held(new Disp(k + 1000));
    var w1: Own := (if k mod 3 = 0 then w0 else (if k mod 3 = 1 then w2 else Own.Empty));)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=400\n"));
}

// The selected branch moves into the constructor parameter and on into the
// field.
BEZEL_TEST(owning_choice_argument_move_6182,
           if_expression_branch_as_constructor_argument_spec_20) {
    expectDisposedOncePerIteration(program(
        R"(class Bx public V: Own; public constructor(v: Own) V := v; end constructor; end Bx;
)",
        "",
        R"(    var w0 := Own.Held(new Disp(k));
    var x1 := new Bx((if k mod 2 = 0 then w0 else Own.Empty));)"));
}

// The ownership pass's half, pinned as a control: the argument IS moved, so a
// later read is rejected (§28.3). It was rejected before the fix too — the
// defect was that codegen did not perform the move the checker had recorded.
BEZEL_TEST(owning_choice_argument_move_6182,
           case_constructor_argument_read_after_the_move_is_rejected_spec_28_3) {
    JitResult r = runAot(program("", "", R"(    var w0 := Own.Held(new Disp(k));
    var w1 := NG.Has(w0);
    var z := w0;)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "owned value in `w0` was already moved out at this point (§28.3)"));
}

BEZEL_TEST(owning_choice_argument_move_6182,
           constructor_argument_read_after_the_move_is_rejected_spec_28_3) {
    JitResult r = runAot(program(
        "class Bx public V: Own; public constructor(v: Own) V := v; end "
        "constructor; end Bx;\n",
        "", R"(    var w := Own.Held(new Disp(k));
    var x1 := new Bx(w);
    var z := w;)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "owned value in `w` was already moved out at this point (§28.3)"));
}
