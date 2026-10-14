// Spec-derived regression tests for issues #6001 and #6158 -- an overload set
// declared on a GENERIC class or interface, called on an instantiated
// receiver, was "no overload ... is applicable" whenever a parameter was
// spelled with the type's own parameter (`L(e: Animal, x: T)` on `G<T>`,
// `Put(x: T)` on `IL<T>`).
//
// Spec lines (docs/Bezel_Language_Spec.md, v0.338):
//   §22.6 *Generic candidates*: "Surviving candidates enter the ranking below
//     with their substituted signatures".
//   §22.6 *Implicit conversions and tie-breaking*: "one candidate is
//     **better** than another when the better candidate requires *no*
//     implicit conversion (the argument's static type matches the parameter
//     type exactly) and the other requires an implicit conversion".
//   §11.1: "A generic interface may be listed at **two or more
//     instantiations** ... §11.2's matching rule is applied once per
//     obligation, each under its own instantiation's substitution".
//
// ONE CAUSE on the sema side: `resolveMethodOverload` ranked each candidate's
// parameter types as DECLARED, so the bare `T` was a declaration-less name no
// argument converts to. It now ranks them at the receiver's instantiation.
//
// #6158 has a second half the rejection was hiding: the itable witness of a
// generic interface's method was the first same-arity overload (the bare `T`
// matched nothing), so once the call compiles `ib.Put("a")` would have RUN
// `Put(x: bool)`. Every interface row below therefore declares the
// implementing overloads in the order that makes the arity pick wrong, and
// prints which body ran.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kTypes =
    "class Animal end Animal; class Dog: Animal end Dog; ";

// `IL<T>` with the two overloads of the issue, in the issue's order.
const char* kIL =
    "interface IL<T> method Put(x: T): int; method Put(x: bool): int; end IL; ";

std::string mainOf(const std::string& body) {
    return "class P public static method Main(): int " + body +
           " return 0; end Main; end P;";
}

void expectRuns(const std::string& prog, const char* out) {
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

void expectRejects(const std::string& prog, const char* needle) {
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

}  // namespace

// -- #6001's repro, verbatim: `Dog` is `L(e: Dog, x: T)`'s exact match.
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           issue_6001_repro_generic_class_exact_overload_spec_22_6) {
    expectRuns(
        "module M; class Animal end Animal; class Dog: Animal end Dog;  "
        "class G<T> public method L(e: Animal, x: T): int => 1; "
        "public method L(e: Dog, x: T): int => 2; end G;\n"
        "class P public static method Main(): int \n"
        "  using var g := new G<int>(); using var a := new Dog();\n"
        "  Console.WriteLine(g.L(a, 1)); return 0; end Main; end P;\n",
        "2\n");
}

// -- #6158's repro, verbatim (IL<string>). `Both` declares `Put(bool)` first,
//    so an arity-picked witness prints 1 for `ib.Put("a")`.
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           issue_6158_repro_generic_interface_string_spec_22_6) {
    expectRuns(
        "module M;\n"
        "interface IL<T> method Put(x: T): int; method Put(x: bool): int; end IL;\n"
        "class Both: IL<string>\n"
        " public method Put(x: bool): int => 1;\n"
        " public method Put(x: string): int => 2;\n"
        "end Both;\n"
        "class P public static method Main(): int\n"
        " using var bo := new Both(); var ib: IL<string> := bo;\n"
        " Console.WriteLine(ib.Put(\"a\"));\n"
        " Console.WriteLine(ib.Put(true));\n"
        " return 0; end Main; end P;\n",
        "2\n1\n");
}

// -- #6158's second repro, verbatim (IL<int>, an `int` local).
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           issue_6158_repro_generic_interface_int_spec_22_6) {
    expectRuns(
        "module M;\n"
        "interface IL<T> method Put(x: T): int; method Put(x: bool): int; end IL;\n"
        "class Both: IL<int>\n"
        " public method Put(x: bool): int => 1;\n"
        " public method Put(x: int): int => 2;\n"
        "end Both;\n"
        "class P public static method Main(): int\n"
        " using var bo := new Both(); var i2: IL<int> := bo; var l: int := 3;\n"
        " Console.WriteLine(i2.Put(l));\n"
        " Console.WriteLine(i2.Put(true));\n"
        " return 0; end Main; end P;\n",
        "2\n1\n");
}

// -- Generic class: `Q(x: T)` beside `Q(x: string)` at `G<int>`; the other
//    `L` argument; a generic METHOD on the generic class (its own `U` is
//    inferred, the class's `T` substituted).
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           generic_class_type_parameter_overloads_spec_22_6) {
    expectRuns(
        "module M; " + std::string(kTypes) +
        "class G<T> "
        "  public method L(e: Animal, x: T): int => 1; "
        "  public method L(e: Dog, x: T): int => 2; "
        "  public method Q(x: T): int => 3; "
        "  public method Q(x: string): int => 4; "
        "  public method R<U>(x: T, y: U): int => 5; "
        "  public method R<U>(x: string, y: U): int => 6; "
        "end G; " +
        mainOf("using var g := new G<int>(); using var an := new Animal(); "
               "Console.WriteLine(g.L(an, 1)); "
               "Console.WriteLine(g.Q(1)); Console.WriteLine(g.Q(\"s\")); "
               "Console.WriteLine(g.R(1, true)); "
               "Console.WriteLine(g.R(\"s\", true)); "),
        "1\n3\n4\n5\n6\n");
}

// -- The instantiation reaches an inherited overload set: through a derived
//    receiver, and through `this.` and a bare call inside the derived class.
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           generic_base_class_overloads_through_derived_spec_22_6) {
    expectRuns(
        "module M; "
        "class G<T> "
        "  public method Q(x: T): int => 3; "
        "  public method Q(x: string): int => 4; "
        "end G; "
        "class D: G<int> "
        "  public method Via(): int => Q(5) * 10 + this.Q(\"s\"); "
        "end D; " +
        mainOf("using var d := new D(); "
               "Console.WriteLine(d.Q(1)); Console.WriteLine(d.Q(\"s\")); "
               "Console.WriteLine(d.Via()); "),
        "3\n4\n34\n");
}

// -- A `T?` parameter at a class instantiation: `N(x: T?)` admits the `Dog`
//    (a conversion), `N(x: bool)` does not.
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           generic_class_nullable_type_parameter_overload_spec_22_6) {
    expectRuns(
        "module M; " + std::string(kTypes) +
        "class G<T> "
        "  public method N(x: T?): int => 7; "
        "  public method N(x: bool): int => 8; "
        "end G; " +
        mainOf("using var g := new G<Dog>(); using var d := new Dog(); "
               "Console.WriteLine(g.N(d)); Console.WriteLine(g.N(true)); "),
        "7\n8\n");
}

// -- The witness is inherited: `B: A`, `A: IL<string>`.
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           generic_interface_witness_inherited_from_base_class_spec_11_2) {
    expectRuns(
        "module M; " + std::string(kIL) +
        "class A: IL<string> "
        "  public method Put(x: bool): int => 1; "
        "  public method Put(x: string): int => 2; "
        "end A; "
        "class B: A end B; " +
        mainOf("using var b := new B(); var ib: IL<string> := b; "
               "Console.WriteLine(ib.Put(\"a\")); "
               "Console.WriteLine(ib.Put(true)); "),
        "2\n1\n");
}

// -- The receiver is an interface that INHERITS the generic one
//    (`IJ: IL<string>`); the implementation widens `string` to `string?`
//    (§11.2), which is still the `Put(x: T)` witness.
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           generic_interface_reached_through_derived_interface_spec_22_6) {
    expectRuns(
        "module M; " + std::string(kIL) +
        "interface IJ: IL<string> method Z(): int; end IJ; "
        "class K: IJ "
        "  public method Put(x: bool): int => 3; "
        "  public method Put(x: string?): int => 4; "
        "  public method Z(): int => 0; "
        "end K; " +
        mainOf("using var k := new K(); var ij: IJ := k; "
               "var ik: IL<string> := k; "
               "Console.WriteLine(ij.Put(\"a\")); "
               "Console.WriteLine(ij.Put(true)); "
               "Console.WriteLine(ik.Put(\"a\")); "
               "Console.WriteLine(ik.Put(false)); "),
        "4\n3\n4\n3\n");
}

// -- A GENERIC class lists the interface at its own parameter
//    (`Gen<U>: IL<U>`): through the interface and directly.
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           generic_class_implements_generic_interface_spec_11_2) {
    expectRuns(
        "module M; " + std::string(kIL) +
        "class Gen<U>: IL<U> "
        "  public method Put(x: bool): int => 5; "
        "  public method Put(x: U): int => 6; "
        "end Gen; " +
        mainOf("using var g := new Gen<int>(); var ig: IL<int> := g; "
               "Console.WriteLine(ig.Put(4)); Console.WriteLine(ig.Put(true)); "
               "Console.WriteLine(g.Put(4)); Console.WriteLine(g.Put(true)); "),
        "6\n5\n6\n5\n");
}

// -- §11.1: one class, two instantiations, one member set; each receiver
//    reaches its own instantiation's witness.
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           generic_interface_two_instantiations_spec_11_1) {
    expectRuns(
        "module M; " + std::string(kIL) +
        "class Two: IL<int>, IL<string> "
        "  public method Put(x: bool): int => 7; "
        "  public method Put(x: string): int => 8; "
        "  public method Put(x: int): int => 9; "
        "end Two; " +
        mainOf("using var t := new Two(); var t1: IL<int> := t; "
               "var t2: IL<string> := t; "
               "Console.WriteLine(t1.Put(4)); Console.WriteLine(t2.Put(\"s\")); "
               "Console.WriteLine(t1.Put(true)); "
               "Console.WriteLine(t2.Put(true)); "),
        "9\n8\n7\n7\n");
}

// -- The receiver is a type parameter constrained to the instantiation.
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           constrained_type_parameter_receiver_spec_22_6) {
    expectRuns(
        "module M; " + std::string(kIL) +
        "class A: IL<string> "
        "  public method Put(x: bool): int => 1; "
        "  public method Put(x: string): int => 2; "
        "end A; "
        "static class S "
        "  public static method Use<X>(i: X, v: string): int "
        "    where X: IL<string> => i.Put(v) * 10 + i.Put(true); "
        "end S; " +
        mainOf("using var a := new A(); Console.WriteLine(S.Use(a, \"z\")); "),
        "21\n");
}

// -- Witness order: the interface's `F(x: Dog, y: T)` slot belongs to the
//    class's EXACT `F(x: Dog, y: int)`, not to the widened `F(x: Dog?, ...)`
//    declared before it; and two built-in exception classes are two types.
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           generic_interface_witness_exact_before_widened_spec_11_2) {
    expectRuns(
        "module M; " + std::string(kTypes) +
        "interface I<T> "
        "  method F(x: Dog, y: T): int; method F(x: Dog?, y: T): int; "
        "  method E(x: Exception, y: T): int; "
        "  method E(x: ArgumentException, y: T): int; "
        "end I; "
        "class C: I<int> "
        "  public method F(x: Dog?, y: int): int => 2; "
        "  public method F(x: Dog, y: int): int => 1; "
        "  public method E(x: ArgumentException, y: int): int => 4; "
        "  public method E(x: Exception, y: int): int => 3; "
        "end C; "
        "static class H public static method Mk(): owned Dog? => new Dog(); "
        "end H; " +
        mainOf("using var c := new C(); var i: I<int> := c; "
               "using var d := new Dog(); using var n := H.Mk(); "
               "var e := new Exception(\"e\"); "
               "var ae := new ArgumentException(\"a\"); "
               "Console.WriteLine(i.F(d, 7)); Console.WriteLine(i.F(n, 7)); "
               "Console.WriteLine(i.E(e, 7)); Console.WriteLine(i.E(ae, 7)); "),
        "1\n2\n3\n4\n");
}

// -- REJECT: an argument no substituted candidate admits.
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           generic_class_no_candidate_still_rejected_spec_22_6) {
    expectRejects(
        "module M; "
        "class G<T> "
        "  public method Q(x: T): int => 3; "
        "  public method Q(x: string): int => 4; "
        "end G; " +
        mainOf("using var g := new G<int>(); Console.WriteLine(g.Q(true)); "),
        "no overload of `Q` is applicable");
}

// -- REJECT: `Dog` converts to both `Animal` and `Object` (§22.6: "equally
//    ranked"). Pre-fix this was the no-candidate error.
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           generic_class_two_conversions_ambiguous_spec_22_6) {
    expectRejects(
        "module M; " + std::string(kTypes) +
        "class G<T> "
        "  public method L(e: Animal, x: T): int => 1; "
        "  public method L(e: Object, x: T): int => 2; "
        "end G; " +
        mainOf("using var g := new G<int>(); using var d := new Dog(); "
               "Console.WriteLine(g.L(d, 1)); "),
        "ambiguous call to `L`");
}

// -- UNCHANGED, pending #6220(c) (interpretation recorded in the fixing
//    commit): at `G<string>` the substituted signatures of `Q(x: T)` and
//    `Q(x: string)` coincide. Before #6001 the unsubstituted `T` matched
//    nothing and `Q(x: string)` ran; that selection is kept until the spec
//    rules on coinciding substituted signatures.
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           generic_class_overloads_coinciding_unchanged_pending_6220) {
    expectRuns(
        "module M; "
        "class G<T> "
        "  public method Q(x: T): int => 3; "
        "  public method Q(x: string): int => 4; "
        "end G; " +
        mainOf("using var g := new G<string>(); "
               "Console.WriteLine(g.Q(\"s\")); "),
        "4\n");
}

// -- UNCHANGED, pending #6220(c): the static spelling of the row above.
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           generic_class_static_overloads_coinciding_unchanged_pending_6220) {
    expectRuns(
        "module M; "
        "class G<T> public static method Q(x: T): int => 3; "
        "public static method Q(x: string): int => 4; end G; " +
        mainOf("Console.WriteLine(G<string>.Q(\"s\")); "),
        "4\n");
}

// -- UNCHANGED, pending #6220(c): the interface spelling. `I<string>`'s
//    `F(x: T)` and `F(x: string)` coincide and `C` implements both with one
//    method.
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           generic_interface_overloads_coinciding_unchanged_pending_6220) {
    expectRuns(
        "module M; "
        "interface I<T> method F(x: T): int; method F(x: string): int; end I; "
        "class C: I<string> public method F(x: string): int => 1; end C; " +
        mainOf("using var c := new C(); var i: I<string> := c; "
               "Console.WriteLine(i.F(\"s\")); Console.WriteLine(c.F(\"s\")); "),
        "1\n1\n");
}

// -- §12.2 (v0.338): "a derived class cannot add an overload to an inherited
//    method's name, and an inherited overload set does not extend across the
//    inheritance boundary". An override of a GENERIC base's member replaces
//    it at the instantiation the derived class names: `D: B<int>`'s
//    `Q(x: int)` IS `B<T>.Q(x: T)`, not a second candidate beside it. Ranking
//    at substituted signatures (#6001) made the two equally exact.
const char* kGenBase =
    "class B<T> public virtual method Q(x: T): int => 3; "
    "public virtual method Q(x: bool): int => 4; end B; ";

BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           override_of_generic_base_member_is_not_a_second_candidate_spec_12_2) {
    expectRuns(
        "module M; " + std::string(kGenBase) +
        "class D: B<int> public override method Q(x: int): int => 5; end D; " +
        mainOf("using var d := new D(); Console.WriteLine(d.Q(1)); "
               "Console.WriteLine(d.Q(true)); "),
        "5\n4\n");
}

BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           override_of_generic_base_member_single_overload_spec_12_2) {
    expectRuns(
        "module M; "
        "class B<T> public virtual method Q(x: T): int => 3; end B; "
        "class D: B<int> public override method Q(x: int): int => 5; end D; " +
        mainOf("using var d := new D(); Console.WriteLine(d.Q(1)); "),
        "5\n");
}

// -- The same, called bare and through `this.` inside the derived class.
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           override_of_generic_base_member_called_inside_derived_spec_12_2) {
    expectRuns(
        "module M; " + std::string(kGenBase) +
        "class D: B<int> public override method Q(x: int): int => 5; "
        "public method Via(): int => Q(1) * 10 + this.Q(true); end D; " +
        mainOf("using var d := new D(); Console.WriteLine(d.Via()); "),
        "54\n");
}

// -- An abstract generic base, both members overridden.
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           override_of_abstract_generic_base_members_spec_12_2) {
    expectRuns(
        "module M; "
        "abstract class B<T> public abstract method Q(x: T): int; "
        "public abstract method Q(x: bool): int; end B; "
        "class D: B<string> public override method Q(x: string): int => 5; "
        "public override method Q(x: bool): int => 6; end D; " +
        mainOf("using var d := new D(); Console.WriteLine(d.Q(\"s\")); "
               "Console.WriteLine(d.Q(true)); "),
        "5\n6\n");
}

// -- §12.1's nullability widening on the override (`B<Dog>`'s `Q(x: T)`
//    overridden as `Q(x: Dog?)`): the override's body runs, not the base's.
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           widened_override_of_generic_base_member_runs_override_spec_12_1) {
    expectRuns(
        "module M; class Animal end Animal; class Dog: Animal end Dog; " +
        std::string(kGenBase) +
        "class D: B<Dog> public override method Q(x: Dog?): int => 5; end D; " +
        mainOf("using var d := new D(); using var g := new Dog(); "
               "Console.WriteLine(d.Q(g)); Console.WriteLine(d.Q(true)); "),
        "5\n4\n");
}

// -- Two levels: `D<U>: B<U>` overrides at `U`, `E: D<int>` at `int`.
BEZEL_TEST(sema_overload_generic_receiver_6001_6158,
           override_through_two_generic_levels_spec_12_2) {
    expectRuns(
        "module M; " + std::string(kGenBase) +
        "class D<U>: B<U> public override method Q(x: U): int => 5; end D; "
        "class E: D<int> public override method Q(x: int): int => 7; end E; " +
        mainOf("using var d := new E(); Console.WriteLine(d.Q(1)); "
               "Console.WriteLine(d.Q(true)); "),
        "7\n4\n");
}
