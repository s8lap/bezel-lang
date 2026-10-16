// Spec-derived regression tests for #6166 — §35.4 rule 1, §35.11 (v0.338).
//
//   §35.4 rule 1: "A choice is **owning** if any case has a payload field
//   marked `owned` (a class, interface, or — at a class or interface
//   instantiation — bare-type-parameter payload), or a payload field of a
//   move-only value type (§15.3 — an owning inline array or another owning
//   choice, transitively)."
//   §35.11: "Classification is evaluated **per instantiation**".
//
// `classifyChoiceCore`'s owning leg substitutes a generic payload at the
// enclosing instantiation (#6048). Its cycle guard then asked whether the
// payload's DECLARATION was already on the classification path and, if so,
// read the payload unsubstituted. `InnerU<Own>` reached from
// `InnerU<InnerU<Own>>` shares the declaration `InnerU` and is a different
// instantiation, so the nested one was read as the bare parameter `T` — not
// move-only — and `InnerU<InnerU<Own>>` came out value-like: copyable, storable
// twice, and never disposed (drops=0, 200 leaked objects). The guard is now
// keyed on the instantiation (declaration + type arguments).
//
// Every run row uses the checked oracle (`runAotChecked`, ASan/LSan), loops
// 200 times and asserts stdout, so a missed disposal (a leak fails the run)
// and a double disposal are both visible; the one exception is named at its
// row. Reject rows assert the diagnostic text.
//
// Shapes deliberately avoided (pre-existing, separate issues): a `v: Own?`
// payload (#6167), an owning choice passed by value twice or captured by a
// lambda (#6168), a value-recursive generic choice with an owning leg (#6169).
//
// per §35.4, §35.11, §35.1, §15.3, §28.3, v0.338

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The declarations every row shares. `Disp` counts its destructor runs;
// `Own` is an owning choice (rule 1, an `owned` class payload); `Inner<T>`
// marks its bare-parameter payload `owned`, `InnerU<T>` and `Outer<T>` leave
// it unmarked; `W<T>` nests `InnerU` in itself in a DECLARED payload type.
const char* const kPrelude = R"(module M;
class Stats public static Drops: int := 0; end Stats;
class Disp
  public N: int;
  public constructor(n: int) N := n; end constructor;
  destructor Stats.Drops := Stats.Drops + 1; end destructor;
end Disp;
choice Own Held(owned r: Disp), Empty; end Own;
choice Inner<T> Has(owned v: T), No; end Inner;
choice InnerU<T> Has(v: T), No; end InnerU;
choice Outer<T> Wrap(v: T), None; end Outer;
choice W<T> Ok(v: InnerU<InnerU<T>>), No; end W;
)";

// `extraDecls` + a `Step(k)` whose body is `stepBody`, called 200 times;
// prints `200 drops=<destructor runs>`.
std::string program(const std::string& extraDecls,
                    const std::string& stepBody) {
    return std::string(kPrelude) + extraDecls + R"(class Program
  static method Step(k: int): int
)" + stepBody + R"(
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

// `InnerU<InnerU<...<leaf>>>`, `depth` levels.
std::string innerUNest(int depth, const std::string& leaf) {
    std::string t = leaf;
    for (int i = 0; i < depth; ++i) t = "InnerU<" + t + ">";
    return t;
}

const char* const kMovedOut = "was already moved out at this point (§28.3)";

}  // namespace

// (1) The issue's shape, reject side. `InnerU<InnerU<Own>>` is owning: its
// payload `v: T` is `InnerU<Own>`, whose payload is the owning choice `Own`
// ("another owning choice, transitively"). An owning choice is move-only
// (§15.3), so the second read of `w` is a use after move (§28.3). Before the
// fix both copies were accepted.
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           innerU_in_itself_at_owning_choice_second_copy_rejected_spec_35_4_rule_1) {
    JitResult r = runAot(program("", R"(    var w := InnerU<InnerU<Own>>.Has(InnerU<Own>.Has(Own.Held(new Disp(k))));
    var w2 := w;
    var w3 := w;)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("owned value in `w` ") + kMovedOut).c_str()));
}

// (1) The issue's shape, run side: the single owner is disposed at scope end,
// once per iteration. Before the fix: `200 drops=0` and 200 leaked objects.
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           innerU_in_itself_at_owning_choice_single_owner_disposes_spec_35_4_rule_1) {
    JitResult r = runAotChecked(program("", R"(    var w := InnerU<InnerU<Own>>.Has(InnerU<Own>.Has(Own.Held(new Disp(k))));)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=200\n"));
}

// (2) The matrix cell that #6048 turned from rejected into accepted-and-
// leaking: `W<T>` DECLARES the nested payload `InnerU<InnerU<T>>`, and one
// `W<Own>` value is passed to two constructors. `W<Own>` is owning, a plain
// parameter of a move-only value type takes its argument by value-move
// (§15.3 Parameter passing), so the second `new Bx(a)` reads a moved-out
// binding (§28.3). Before the fix: accepted, `200 drops=0`, 200 leaked
// objects.
//
// The constructor deliberately does NOT store `v` in a field. A `W<Own>`
// class field is today rejected outright as "a borrow-bearing choice may not
// be stored in an instance field of a class" — an over-rejection of a valid
// program (#6181, open: the holds-borrows leg reads the declared payload
// unsubstituted) — and that rejection fires on this program's field store
// whether or not `W<Own>` is classified owning, so a row with the field would
// stay green with the classification broken. These rows have no store for
// #6181 to reject and accept exactly one diagnostic.
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           declared_innerU_in_itself_payload_at_owning_choice_passed_to_two_constructors_rejected_spec_35_4_rule_1) {
    JitResult r = runAot(program(R"(class Bx public N: int := 0; public constructor(v: W<Own>) N := 1; end constructor; end Bx;
)", R"(    var a := W<Own>.Ok(InnerU<InnerU<Own>>.Has(InnerU<Own>.Has(Own.Held(new Disp(k)))));
    var x1 := new Bx(a);
    var x2 := new Bx(a);)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("owned value in `a` ") + kMovedOut).c_str()));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "a borrow-bearing choice may not be stored"));
}

// (2) The same value passed twice to a plain by-value method parameter.
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           declared_innerU_in_itself_payload_at_owning_choice_passed_to_two_calls_rejected_spec_35_4_rule_1) {
    JitResult r = runAot(program(R"(class Sink public static method Take(v: W<Own>): int return 1; end Take; end Sink;
)", R"(    var a := W<Own>.Ok(InnerU<InnerU<Own>>.Has(InnerU<Own>.Has(Own.Held(new Disp(k)))));
    var x1 := Sink.Take(a);
    var x2 := Sink.Take(a);)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("owned value in `a` ") + kMovedOut).c_str()));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "a borrow-bearing choice may not be stored"));
}

// (2) The same declared payload without the class field: `W<Own>` is owning,
// so the second read of `a` is a use after move (§28.3). Before the fix both
// copies were accepted.
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           declared_innerU_in_itself_payload_at_owning_choice_second_copy_rejected_spec_35_4_rule_1) {
    JitResult r = runAot(program("", R"(    var a := W<Own>.Ok(InnerU<InnerU<Own>>.Has(InnerU<Own>.Has(Own.Held(new Disp(k)))));
    var b := a;
    var c := a;)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("owned value in `a` ") + kMovedOut).c_str()));
}

// (2) Run side of the declared payload: the single owner is disposed.
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           declared_innerU_in_itself_payload_at_owning_choice_single_owner_disposes_spec_35_4_rule_1) {
    JitResult r = runAotChecked(program("", R"(    var a := W<Own>.Ok(InnerU<InnerU<Own>>.Has(InnerU<Own>.Has(Own.Held(new Disp(k)))));)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=200\n"));
}

// (3) The other direction — the fix must not make a re-entered declaration
// owning. `InnerU<InnerU<int>>` holds only values, so it is value-like
// (§35.4 rule 3) and copies freely.
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           innerU_in_itself_at_int_stays_copyable_spec_35_11) {
    JitResult r = runAotChecked(program("", R"(    var w := InnerU<InnerU<int>>.Has(InnerU<int>.Has(k));
    var w2 := w;
    var w3 := w;)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=0\n"));
}

// (3) Same, through the declared payload: `W<int>` is value-like.
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           declared_innerU_in_itself_payload_at_int_stays_copyable_spec_35_11) {
    JitResult r = runAotChecked(program("", R"(    var w := W<int>.Ok(InnerU<InnerU<int>>.Has(InnerU<int>.Has(k)));
    var w2 := w;
    var w3 := w;)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=0\n"));
}

// (3) `string` is classification-neutral (§35.4), so `InnerU<InnerU<string>>`
// is value-like and copies freely. This row uses `runAot`, not the checked
// oracle: the string payload of a value-like choice nested in a value-like
// choice is never released (#6170, open, unrelated to classification), so the
// checked oracle reports that leak on the pre-#6048 compiler too.
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           innerU_in_itself_at_string_stays_copyable_spec_35_11) {
    JitResult r = runAot(program("", R"(    var w := InnerU<InnerU<string>>.Has(InnerU<string>.Has("s" + k.ToString()));
    var w2 := w;
    var w3 := w;)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=0\n"));
}

// (4) Three levels of the same declaration: every level is a different
// instantiation and is classified by recursion.
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           innerU_nested_three_deep_at_owning_choice_second_copy_rejected_spec_35_4_rule_1) {
    JitResult r = runAot(program("", R"(    var w := InnerU<InnerU<InnerU<Own>>>.Has(InnerU<InnerU<Own>>.Has(InnerU<Own>.Has(Own.Held(new Disp(k)))));
    var w2 := w;
    var w3 := w;)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("owned value in `w` ") + kMovedOut).c_str()));
}

// (4) Three levels, run side.
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           innerU_nested_three_deep_at_owning_choice_single_owner_disposes_spec_35_4_rule_1) {
    JitResult r = runAotChecked(program("", R"(    var w := InnerU<InnerU<InnerU<Own>>>.Has(InnerU<InnerU<Own>>.Has(InnerU<Own>.Has(Own.Held(new Disp(k)))));)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=200\n"));
}

// (4) Three DIFFERENT declarations, marked and unmarked: `Outer<InnerU<Inner<
// Own>>>`. A control for the rows above — no declaration repeats on the path,
// so this was owning before the fix as well and must stay so.
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           mixed_declarations_at_owning_choice_second_copy_rejected_spec_35_4_rule_1) {
    JitResult r = runAot(program("", R"(    var w := Outer<InnerU<Inner<Own>>>.Wrap(InnerU<Inner<Own>>.Has(Inner<Own>.Has(Own.Held(new Disp(k)))));
    var w2 := w;
    var w3 := w;)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("owned value in `w` ") + kMovedOut).c_str()));
}

// (4) Mixed declarations, run side (control).
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           mixed_declarations_at_owning_choice_single_owner_disposes_spec_35_4_rule_1) {
    JitResult r = runAotChecked(program("", R"(    var w := Outer<InnerU<Inner<Own>>>.Wrap(InnerU<Inner<Own>>.Has(Inner<Own>.Has(Own.Held(new Disp(k)))));)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=200\n"));
}

// (4) Mixed declarations at `int`: `Inner<int>`'s marker is erased (§35.11), so
// the whole instantiation is value-like (control).
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           mixed_declarations_at_int_stay_copyable_spec_35_11) {
    JitResult r = runAotChecked(program("", R"(    var w := Outer<InnerU<Inner<int>>>.Wrap(InnerU<Inner<int>>.Has(Inner<int>.Has(k)));
    var w2 := w;
    var w3 := w;)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=0\n"));
}

// (5) A self-recursive generic choice the way §35.1 admits one — "Recursive
// sums are expressed with a class node in the cycle": `L<T>` names itself only
// as the type argument of an `owned` class payload. It compiled before the
// fix and must keep compiling (control).
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           self_recursive_generic_choice_through_class_node_at_int_keeps_compiling_spec_35_1) {
    JitResult r = runAotChecked(program(R"(class Tree<T> public Kids: int := 0; end Tree;
choice L<T> Cons(h: InnerU<T>, owned t: Tree<L<T>>), Nil(e: InnerU<T>); end L;
)", R"(    var a := L<int>.Cons(InnerU<int>.Has(k), new Tree<L<int>>());)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=0\n"));
}

// (5) The same recursive choice at an owning-choice argument: the `InnerU<Own>`
// payload is disposed with the value (control).
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           self_recursive_generic_choice_through_class_node_at_owning_choice_disposes_spec_35_4) {
    JitResult r = runAotChecked(program(R"(class Tree<T> public Kids: int := 0; end Tree;
choice L<T> Cons(h: InnerU<T>, owned t: Tree<L<T>>), Nil(e: InnerU<T>); end L;
)", R"(    var a := L<Own>.Cons(InnerU<Own>.Has(Own.Held(new Disp(k))), new Tree<L<Own>>());)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=200\n"));
}

// (5) The recursive choice at an argument that nests `InnerU` in itself: the
// payload `e: InnerU<T>` is `InnerU<InnerU<Own>>`. Before the fix: drops=0 and
// 200 leaked objects.
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           self_recursive_generic_choice_with_innerU_in_itself_argument_disposes_spec_35_4_rule_1) {
    JitResult r = runAotChecked(program(R"(class Tree<T> public Kids: int := 0; end Tree;
choice L<T> Cons(h: InnerU<T>, owned t: Tree<L<T>>), Nil(e: InnerU<T>); end L;
)", R"(    var a := L<InnerU<Own>>.Nil(InnerU<InnerU<Own>>.Has(InnerU<Own>.Has(Own.Held(new Disp(k)))));)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=200\n"));
}

// (6) The single level: an UNMARKED bare-parameter payload at an owning-
// choice argument, `InnerU<Own>`, is "a payload field of a move-only value
// type" and so owning. #6048 fixed this level (the compiler before it accepted
// both copies); pinned here because the nested rows rest on it (control).
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           unmarked_bare_parameter_payload_at_owning_choice_second_copy_rejected_spec_35_4_rule_1) {
    JitResult r = runAot(program("", R"(    var w := InnerU<Own>.Has(Own.Held(new Disp(k)));
    var w2 := w;
    var w3 := w;)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("owned value in `w` ") + kMovedOut).c_str()));
}

// (6) Single level, run side (control).
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           unmarked_bare_parameter_payload_at_owning_choice_single_owner_disposes_spec_35_4_rule_1) {
    JitResult r = runAotChecked(program("", R"(    var w := InnerU<Own>.Has(Own.Held(new Disp(k)));)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=200\n"));
}

// (7) A DECLARED payload type is one TypeRef for every instantiation of its
// choice, so the classification cache is keyed on the instantiation (the type
// as written plus the type it denotes under the substitution env), never on
// the TypeRef: a verdict reached under one env must not answer for another
// (§35.11, "per instantiation"). `W<int>` declares `v: InnerU<InnerU<T>>`: value-like,
// so it is storable in a class field and copyable into two of them. With the
// instantiation-keyed guard alone the nested `InnerU<T>` was classified once
// with no env (conservatively borrow-bearing), cached, and that answer was
// then read at `T := int`: the store was rejected as "a borrow-bearing choice
// may not be stored in an instance field of a class".
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           declared_innerU_in_itself_payload_at_int_class_field_stored_twice_runs_spec_35_11) {
    JitResult r = runAotChecked(program(
        R"(class Hold public V: W<int>; public constructor(v: W<int>) V := v; end constructor; end Hold;
)",
        R"(    var b := W<int>.Ok(InnerU<InnerU<int>>.Has(InnerU<int>.Has(k)));
    var h := new Hold(b);
    var h2 := new Hold(b);)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=0\n"));
}

// (7) The same through two different declarations, `v: Outer<InnerU<T>>` at
// `int`. Rejected with the borrow-bearing text before the fix (and before
// #6048).
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           declared_mixed_payload_at_int_class_field_stored_twice_runs_spec_35_11) {
    JitResult r = runAotChecked(program(
        R"(choice W4<T> Ok(v: Outer<InnerU<T>>), No; end W4;
class Hold public V: W4<int>; public constructor(v: W4<int>) V := v; end constructor; end Hold;
)",
        R"(    var b := W4<int>.Ok(Outer<InnerU<int>>.Wrap(InnerU<int>.Has(k)));
    var h := new Hold(b);
    var h2 := new Hold(b);)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=0\n"));
}

// (7) The unsound direction of the same cache: `Both` holds `Two<int>` and
// `Two<Res>`, whose shared declared payload `x: InnerU<T>` is value-like at
// `int` and borrow-bearing at the class `Res` (§35.4 rule 2: a plain class
// payload is a borrow). `Both` therefore holds a borrow and may not be stored
// in a class field. Before the fix the `T := int` verdict was cached and read
// for `T := Res`, so the program compiled and the field held an untracked
// borrow of `r`.
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           one_declared_payload_at_int_and_at_class_keeps_the_borrow_spec_35_4_rule_2) {
    JitResult r = runAot(program(
        R"(class Res public N: int := 0; end Res;
choice Two<T> A(x: InnerU<T>), N; end Two;
choice Both X(a: Two<int>, b: Two<Res>), E; end Both;
class Hold public V: Both; public constructor(v: Both) V := v; end constructor; end Hold;
)",
        R"(    var r := new Res();
    var b := Both.X(Two<int>.A(InnerU<int>.Has(k)), Two<Res>.A(InnerU<Res>.Has(r)));
    var h := new Hold(b);)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a borrow-bearing choice may not be stored in an instance field of "
           "a class"));
}

// (8) Depth is no limit. A re-entry through type arguments alone strictly
// shrinks the type, so the guard classifies it by recursion however deep it
// goes: `InnerU` nested 40 times at `int` is value-like and may be a record
// field (§9.7 admits a value-like choice field); the same nesting at `Own` is
// owning and may not. The record-field rule is used as the probe because it
// reads the classification directly and needs no value of the type.
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           innerU_nested_forty_deep_at_int_is_value_like_record_field_spec_35_11) {
    JitResult r = runAotChecked(program(
        "record Holder(N: int, V: " + innerUNest(40, "int") + ");\n", ""));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=0\n"));
}

BEZEL_TEST(generic_choice_instantiation_guard_6166,
           innerU_nested_forty_deep_at_owning_choice_is_owning_record_field_rejected_spec_35_4_rule_1) {
    JitResult r = runAot(program(
        "record Holder(N: int, V: " + innerUNest(40, "Own") + ");\n", ""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "record `Holder` may not have an owning-classified choice field "
           "`V` (§35.4, §9.7)"));
}

namespace {

// `choice C0<T> X(v: T), N;` and `choice Ci<T> K(x: C(i-1)<T>), N;` for
// i = 1..n: every level's payload is a DECLARED type mentioning `T`.
std::string declaredChain(int n) {
    std::string d = "choice C0<T> X(v: T), N; end C0;\n";
    for (int i = 1; i <= n; ++i)
        d += "choice C" + std::to_string(i) + "<T> K(x: C" +
             std::to_string(i - 1) + "<T>), N; end C" + std::to_string(i) +
             ";\n";
    return d;
}

// `choice Opt<T>`, `choice C0<T> X(v: T), N;` and
// `choice Ci<T> K(x: Opt<C(i-1)<T>>), N;` for i = 1..n: classifying `Cn<T>`
// walks n DIFFERENT instantiations of `Opt` on one path (`Opt<C(n-1)<T>>`,
// ..., `Opt<C0<T>>`), none nested in another's type arguments.
std::string optWrappedChain(int n) {
    std::string d =
        "choice Opt<T> Some(v: T), None; end Opt;\n"
        "choice C0<T> X(v: T), N; end C0;\n";
    for (int i = 1; i <= n; ++i)
        d += "choice C" + std::to_string(i) + "<T> K(x: Opt<C" +
             std::to_string(i - 1) + "<T>>), N; end C" + std::to_string(i) +
             ";\n";
    return d;
}

}  // namespace

// (9) The cache must HIT under a substitution env. Every level of
// `C40<Own>` is a declared payload classified under `T := Own`; a cache that
// is bypassed whenever an env is present classifies level i twice per level
// above it (the owning leg, then the borrow leg), 2^40 walks. With the
// TypeRef-keyed cache disabled under an env this program did not compile
// within the harness's 120 s deadline (depth 20 alone: 14 s of compiler time);
// keyed on the instantiation it compiles in a few hundredths of a second.
// The verdict is checked too: `C40<Own>` is owning, single owner disposed.
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           declared_generic_chain_forty_deep_at_owning_choice_compiles_and_disposes_spec_35_11) {
    std::string value = "C0<Own>.X(Own.Held(new Disp(k)))";
    for (int i = 1; i <= 40; ++i)
        value = "C" + std::to_string(i) + "<Own>.K(" + value + ")";
    JitResult r = runAotChecked(
        program(declaredChain(40), "    var w := " + value + ";"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=200\n"));
}

// (9) The same chain at `int` is value-like: a record may hold it (§9.7).
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           declared_generic_chain_forty_deep_at_int_is_value_like_record_field_spec_35_11) {
    JitResult r = runAotChecked(program(
        declaredChain(40) + "record Holder(N: int, V: C40<int>);\n", ""));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=0\n"));
}

// (9) ... and at `Own` it is owning: a record may not.
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           declared_generic_chain_forty_deep_at_owning_choice_record_field_rejected_spec_35_4_rule_1) {
    JitResult r = runAot(program(
        declaredChain(40) + "record Holder(N: int, V: C40<Own>);\n", ""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "record `Holder` may not have an owning-classified choice field "
           "`V` (§35.4, §9.7)"));
}

// (10) No count of DISTINCT instantiations is a limit. Classifying
// `C34<int>` walks thirty-four different instantiations of `Opt` on one path;
// all hold values, the chain is value-like and a record may hold it. A guard
// that counted same-declaration instantiations on the path answered "owning"
// past thirty-two and rejected this program as "record `Holder` may not have
// an owning-classified choice field". (The chain is generic so that it
// compiles in about a second: the same walk through thirty-four NON-generic
// choices each holding `Opt<next>` takes over 20 s of compiler time on the
// base compiler too, about 120 s under the sanitizer, #6235.)
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           thirty_four_distinct_instantiations_on_one_path_at_int_are_value_like_record_field_spec_35_11) {
    JitResult r = runAotChecked(program(
        optWrappedChain(34) + "record Holder(N: int, V: C34<int>);\n", ""));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=0\n"));
}

// (10) The same chain stored in a class field and copied into two of them.
// Under the counting guard the value was taken for an owning choice and the
// field store lowered to IR the verifier refused.
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           thirty_four_distinct_instantiations_on_one_path_at_int_class_field_stored_twice_runs_spec_35_11) {
    JitResult r = runAotChecked(program(
        optWrappedChain(34) +
            "class Hold public V: C34<int>; public constructor(v: C34<int>) "
            "V := v; end constructor; end Hold;\n",
        R"(    var x := C34<int>.N;
    var h := new Hold(x);
    var h2 := new Hold(x);)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200 drops=0\n"));
}

// (10) The chain with an owning leaf is owning at every length (rule 1,
// "another owning choice, transitively"): a record may not hold it.
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           thirty_four_distinct_instantiations_on_one_path_at_owning_choice_record_field_rejected_spec_35_4_rule_1) {
    JitResult r = runAot(program(
        optWrappedChain(34) + "record Holder(N: int, V: C34<Own>);\n", ""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "record `Holder` may not have an owning-classified choice field "
           "`V` (§35.4, §9.7)"));
}

// (11) The implementation limit is loud. `E<T>` holds `D<E<Inner<T>>>`: its
// instantiation closure never closes (`E<int>`, `E<Inner<int>>`, ...), which
// §14.6 rejects at the declaration. Classification is queried before that
// stops the compilation, so the walk has to terminate on its own; it does so
// at thirty-two instantiations each nested in the next, and SAYS so — the
// limit is never answered with a classification the program then silently
// compiles under. Both diagnostics are reported.
BEZEL_TEST(generic_choice_instantiation_guard_6166,
           growing_instantiation_chain_reports_the_classification_limit_spec_14_6) {
    JitResult r = runAot(program(
        R"(choice D<X> A(v: X), B; end D;
choice E<T> X(v: D<E<Inner<T>>>), N; end E;
)",
        R"(    var e := E<int>.N;
    var f := e;)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "choice `E` is reached through more than 32 instantiations of "
           "itself, each nested in the next, while classifying a choice that "
           "holds it: the implementation limit on an instantiation chain "
           "that keeps growing (§14.6, §35.4)"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "requires `E` instantiated at a growing argument, whose "
           "instantiation closure has no bound (§14.6, #3271)"));
}
