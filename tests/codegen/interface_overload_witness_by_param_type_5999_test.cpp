// Regression tests for #5999: a call through an INTERFACE-typed receiver ran
// the wrong one of several same-named, same-arity implementing overloads.
//
// Spec §22.6 (v0.338): overload resolution picks the most specific applicable
// overload at the call, so `x.Log(a)` with `x: IL` and `a: Dog` resolves to
// the interface's `Log(e: Dog)`; §11.2 (v0.338): the implementing class's
// witness for an interface method is the member whose parameter types match
// that method's. Codegen's itable filler (findInterfaceImpl) matched by name
// and arity only, so both IL slots were wired to the first-declared overload.
// Each row prints the interface-typed call(s) and the impl-typed (direct)
// oracle on one line and asserts the exact stdout.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string out(const std::string& src) {
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    return r.stdoutText;
}
}  // namespace

// The issue's literal repro: expected `2 1 2 1` (was `1 1 2 1`).
BEZEL_TEST(interface_overload_witness_5999, issue_repro_class_hierarchy_spec_22_6) {
    BEZEL_EXPECT_EQ(out(
        "module M; class Animal end Animal; class Dog: Animal end Dog;  interface IL\n"
        " method Log(e: Animal): int;\n"
        " method Log(e: Dog): int;\n"
        "end IL;\n"
        "class Impl: IL\n"
        " public method Log(e: Animal): int => 1;\n"
        " public method Log(e: Dog): int => 2;\n"
        "end Impl;\n"
        "class P public static method Main(): int\n"
        " using var i := new Impl(); using var a := new Dog(); using var b := new Animal();\n"
        " var x: IL := i;\n"
        " Console.WriteLine(x.Log(a)); Console.WriteLine(x.Log(b)); Console.WriteLine(i.Log(a)); Console.WriteLine(i.Log(b)); return 0; end Main; end P;\n"),
        "2\n1\n2\n1\n");
}

// Primitive parameter types, impl declared in the opposite order.
BEZEL_TEST(interface_overload_witness_5999, primitives_int_long_string_spec_22_6) {
    BEZEL_EXPECT_EQ(out(
        "module M;\n"
        "interface IP method F(x: int): int; method F(x: long): int; method F(x: string): int; end IP;\n"
        "class C: IP\n"
        " public method F(x: string): int => 3;\n"
        " public method F(x: long): int => 2;\n"
        " public method F(x: int): int => 1;\n"
        "end C;\n"
        "class P public static method Main(): int\n"
        " using var c := new C(); var p: IP := c; var l: long := 5; var s: string := \"a\";\n"
        " Console.WriteLine(p.F(1).ToString() + p.F(l).ToString() + p.F(s).ToString() + \" \" + c.F(1).ToString() + c.F(l).ToString() + c.F(s).ToString());\n"
        " return 0; end Main; end P;\n"),
        "123 123\n");
}

// Built-in exception parameter types, inherited witnesses (class via base).
BEZEL_TEST(interface_overload_witness_5999, builtin_exceptions_via_base_class_spec_22_6) {
    BEZEL_EXPECT_EQ(out(
        "module M;\n"
        "interface IE method Log(e: ArgumentException): int; method Log(e: Exception): int; method Log(e: InvalidOperationException): int; end IE;\n"
        "class Base: IE\n"
        " public method Log(e: Exception): int => 11;\n"
        " public method Log(e: InvalidOperationException): int => 13;\n"
        " public method Log(e: ArgumentException): int => 12;\n"
        "end Base;\n"
        "class Derived: Base end Derived;\n"
        "class P public static method Main(): int\n"
        " using var ae := new ArgumentException(\"x\"); using var oe := new InvalidOperationException(\"y\"); var ex: Exception := oe;\n"
        " using var dd := new Derived(); var id: IE := dd;\n"
        " Console.WriteLine(id.Log(ae).ToString() + \" \" + id.Log(oe).ToString() + \" \" + id.Log(ex).ToString() + \" \" + dd.Log(ae).ToString() + \" \" + dd.Log(oe).ToString() + \" \" + dd.Log(ex).ToString());\n"
        " return 0; end Main; end P;\n"),
        "12 13 11 12 13 11\n");
}

// Call through a generic constraint `where T: IE` and an inherited interface.
BEZEL_TEST(interface_overload_witness_5999, generic_constraint_and_inherited_interface_spec_22_6) {
    BEZEL_EXPECT_EQ(out(
        "module M; class Animal end Animal; class Dog: Animal end Dog;\n"
        "interface IA method M(a: int, b: Animal): int; method M(a: int, b: Dog): int; end IA;\n"
        "interface IJ: IA method N(x: Dog): int; method N(x: Animal): int; end IJ;\n"
        "class K: IJ\n"
        " public method M(a: int, b: Dog): int => 22;\n"
        " public method M(a: int, b: Animal): int => 21;\n"
        " public method N(x: Animal): int => 31;\n"
        " public method N(x: Dog): int => 32;\n"
        "end K;\n"
        "class G public static method Use<T>(x: T, d: Dog): int where T: IJ => x.N(d) * 100 + x.M(1, d); end G;\n"
        "class P public static method Main(): int\n"
        " using var d := new Dog(); using var an := new Animal(); using var k := new K();\n"
        " var ij: IJ := k; var ia: IA := k;\n"
        " Console.WriteLine(ij.N(d).ToString() + \" \" + ij.N(an).ToString() + \" \" + ia.M(1, an).ToString() + \" \" + ia.M(1, d).ToString() + \" \" + G.Use(k, d).ToString());\n"
        " return 0; end Main; end P;\n"),
        "32 31 21 22 3222\n");
}

// A generic interface instantiated once with a same-arity non-generic
// overload beside it keeps the arity pick for the unsubstituted `T`.
BEZEL_TEST(interface_overload_witness_5999, generic_interface_instantiation_unchanged_spec_11_2) {
    BEZEL_EXPECT_EQ(out(
        "module M;\n"
        "interface IL<T> method Put(x: T): int; method Put(x: bool): int; end IL;\n"
        "class B2: IL<int>\n"
        " public method Put(x: bool): int => 4;\n"
        " public method Put(x: int): int => 3;\n"
        "end B2;\n"
        "class P public static method Main(): int\n"
        " using var b := new B2(); var i: IL<int> := b;\n"
        " Console.WriteLine(i.Put(true).ToString() + \" \" + b.Put(true).ToString());\n"
        " return 0; end Main; end P;\n"),
        "4 4\n");
}

// Two interfaces declaring a same-named member with different parameter types,
// both implemented by one class that declares both overloads. Each interface
// receiver must bind its own slot's witness (§11.2 v0.338: a member matches an
// interface member only on "parameter types that are exactly equal under the
// interface's substitution"); the direct call on C then resolves over all of
// C's overloads (§22.6 v0.338, "Exact-match ranking over the applicable
// candidates"). `a.F(g)` with `g: Dog` binds IA's F(Animal) (only applicable
// member of IA), so it prints 1. Expected `112 | 12`.
BEZEL_TEST(interface_overload_witness_5999, two_interfaces_same_member_name_each_receiver_spec_11_2) {
    BEZEL_EXPECT_EQ(out(
        "module M; class Animal end Animal; class Dog: Animal end Dog;\n"
        "interface IA method F(x: Animal): int; end IA;\n"
        "interface ID method F(x: Dog): int; end ID;\n"
        "class C: IA, ID\n"
        " public method F(x: Dog): int => 2;\n"
        " public method F(x: Animal): int => 1;\n"
        "end C;\n"
        "class P public static method Main(): int\n"
        " using var c := new C(); var a: IA := c; var d: ID := c; using var an := new Animal(); using var g := new Dog();\n"
        " Console.WriteLine(a.F(an).ToString() + a.F(g).ToString() + d.F(g).ToString() + \" | \" + c.F(an).ToString() + c.F(g).ToString());\n"
        " return 0; end Main; end P;\n"),
        "112 | 12\n");
}

// Interface member F(Dog) implemented by a class whose base declares virtual
// overloads F(Animal) and F(Dog), with a derived override of both. The witness
// for IF(Dog) must be the member whose parameter types match (§11.2 v0.338,
// "parameter types that are exactly equal under the interface's substitution"),
// so an interface receiver over a plain B prints B's F(Dog) = 20, not its
// F(Animal) = 10; over D it prints D's F(Dog) override = 200. The direct call
// on D resolves to the exact-match Dog overload (§22.6 v0.338, "Exact-match
// ranking over the applicable candidates"): 200. Expected `200 20 | 200`.
BEZEL_TEST(interface_overload_witness_5999, inherited_virtual_overloads_derived_override_spec_11_2) {
    BEZEL_EXPECT_EQ(out(
        "module M; class Animal end Animal; class Dog: Animal end Dog;\n"
        "interface I\n"
        " method F(x: Dog): int;\n"
        "end I;\n"
        "class B: I\n"
        " public virtual method F(x: Animal): int => 10;\n"
        " public virtual method F(x: Dog): int => 20;\n"
        "end B;\n"
        "class D: B\n"
        " public override method F(x: Dog): int => 200;\n"
        " public override method F(x: Animal): int => 100;\n"
        "end D;\n"
        "class P public static method Main(): int\n"
        " using var d := new D(); var j: I := d; using var b := new B(); var k: I := b;\n"
        " using var g := new Dog();\n"
        " Console.WriteLine(j.F(g).ToString() + \" \" + k.F(g).ToString() + \" | \" + d.F(g).ToString());\n"
        " return 0; end Main; end P;\n"),
        "200 20 | 200\n");
}
