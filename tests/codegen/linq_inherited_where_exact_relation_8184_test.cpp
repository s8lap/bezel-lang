// fix #8184 (spec §14.2 *Constraints on overrides and interface
// implementations*, §12.2 *The blocked-name set*, §26.5.1, v0.355): the
// inherited-`where` lookup #8181 added (`ExprTC::currentMethodTypeParamMayInherit`)
// must follow the ACTUAL override / implementation relation, not a same-name,
// same-arity method found anywhere in the base closure.
//
// §14.2: "An `override` of a generic method, and a method implementing a
// generic interface method ... inherits the overridden or implemented
// declaration's constraints". A method that overrides and implements NOTHING
// inherits nothing, so its `T` is unconstrained (§14.2 *A type parameter as the
// type argument*) and the stdlib §5 `Sum()` (whose `T` is bound by
// `INumber<T>`, stdlib §2.19) is not an
// applicable candidate on its `Array<T>` receiver: the user's
// `extension Object { Sum() }` answers (§26.5.1). §12.2: "a `private` base
// member never blocks ... the derived declaration creates a **distinct
// member**: no override relationship".
//
// MEASURED ON THE PARENT (r30-c1 @ a7babe11): every RED row below printed the
// §5 `Sum()` (11 per call) — a private base member, a sibling overload of an
// implemented interface method's name, and the implementation of a SECOND,
// unconstrained interface each kept the §5 candidate because they share the
// constrained method's name and arity. The CONTROL rows printed the same
// output on the parent.
//
// The rows are LOOPED (100 iterations) so a per-call leak cannot hide.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string looped(const std::string& decls, const std::string& loopBody) {
    return "module M;\n"
           "extension Object\n"
           "  public method Sum(): int => 4040;\n"
           "end;\n" + decls +
           "class Prog\n"
           "  public static method Main(): int\n"
           "    var i := 0;\n"
           "    var t := 0;\n"
           "    while i < 100 do\n"
           "      var a := new Array<int>();\n"
           "      a.Add(5);\n"
           "      a.Add(6);\n" +
           loopBody +
           "      i := i + 1;\n"
           "    end;\n"
           "    Console.WriteLine(t);\n"
           "    return 0;\n"
           "  end Main;\n"
           "end Prog;\n";
}

}  // namespace

// The issue's w1: a PRIVATE base `S<T> where T: INumber<T>` and a derived,
// distinct, non-override `S<T>` of the same shape (§12.2: no override
// relationship). 100 x 4040 = 404000. PARENT: 1100 (11 per call).
BEZEL_TEST(codegen_linq_inherited_where_exact_relation_8184,
           w1_private_base_same_shape_distinct_member_answers_the_extension) {
    JitResult r = runAotChecked(looped(
        "class B\n"
        "  private method S<T>(a: Array<T>): int where T: INumber<T> =>"
        " a.Sum();\n"
        "  public method K(): int => 0;\n"
        "end B;\n"
        "class D: B\n"
        "  public method S<T>(a: Array<T>): int => a.Sum();\n"
        "end D;\n",
        "      var d := new D();\n"
        "      t := t + d.S(a);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "404000\n");
}

// The issue's w11: position 1 of a private-base distinct member with two type
// parameters. Both positions are unconstrained: 4040 * 100000 + 4040.
// PARENT: 404000011 (position 1 kept the §5 `Sum()`).
BEZEL_TEST(codegen_linq_inherited_where_exact_relation_8184,
           w11_private_base_distinct_member_second_position_answers_the_extension) {
    JitResult r = runAotChecked(looped(
        "class B\n"
        "  private method S<A, Z>(a: Array<A>, b: Array<Z>): int"
        " where Z: INumber<Z> => 0;\n"
        "  public method K(): int => 0;\n"
        "end B;\n"
        "class D: B\n"
        "  public method S<X, Y>(a: Array<X>, b: Array<Y>): int =>"
        " a.Sum() * 100000 + b.Sum();\n"
        "end D;\n",
        "      var d := new D();\n"
        "      t := d.S(a, a);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "404004040\n");
}

// The issue's w2: the class implements IA's constrained `S<T>(s: string)` and
// ALSO overloads the name with `S<T>(a: Array<T>)`, which implements nothing.
// 100 x 4040 = 404000. PARENT: 1100.
BEZEL_TEST(codegen_linq_inherited_where_exact_relation_8184,
           w2_overload_of_an_implemented_interface_name_answers_the_extension) {
    JitResult r = runAotChecked(looped(
        "interface IA\n"
        "  method S<T>(s: string): int where T: INumber<T>;\n"
        "end IA;\n"
        "class D: IA\n"
        "  public method S<T>(s: string): int => 1;\n"
        "  public method S<T>(a: Array<T>): int => a.Sum();\n"
        "end D;\n",
        "      var d := new D();\n"
        "      t := t + d.S(a);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "404000\n");
}

// The issue's w3: the class implements IA (constrained `S<T>(s: string)`) and
// IB (UNconstrained `S<T>(a: Array<T>)`). The implementation of IB inherits
// IB's empty set — directly and through IB. 100 x 2 x 4040 = 808000.
// PARENT: 2200.
BEZEL_TEST(codegen_linq_inherited_where_exact_relation_8184,
           w3_implementation_of_the_unconstrained_interface_answers_the_extension) {
    JitResult r = runAotChecked(looped(
        "interface IA\n"
        "  method S<T>(s: string): int where T: INumber<T>;\n"
        "end IA;\n"
        "interface IB\n"
        "  method S<T>(a: Array<T>): int;\n"
        "end IB;\n"
        "class D: IA, IB\n"
        "  public method S<T>(s: string): int => 1;\n"
        "  public method S<T>(a: Array<T>): int => a.Sum();\n"
        "end D;\n",
        "      var d := new D();\n"
        "      var ib: IB := d;\n"
        "      t := t + d.S(a) + ib.S(a);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "808000\n");
}

// CONTROL (the silent-acceptance mirror of w2/w3): the `Array<T>` overload IS
// the implementation of the CONSTRAINED interface method, beside a sibling
// overload that implements the UNconstrained one. It inherits
// `where T: INumber<T>`, so the §5 `Sum()` applies — directly and through
// each interface. 100 x 3 x 11 = 3300. PARENT: 3300.
BEZEL_TEST(codegen_linq_inherited_where_exact_relation_8184,
           control_implementation_of_the_constrained_interface_keeps_the_builtin) {
    JitResult r = runAotChecked(looped(
        "interface IA\n"
        "  method S<T>(a: Array<T>): int where T: INumber<T>;\n"
        "end IA;\n"
        "interface IB\n"
        "  method S<T>(s: string): int;\n"
        "end IB;\n"
        "class D: IA, IB\n"
        "  public method S<T>(s: string): int => 1;\n"
        "  public method S<T>(a: Array<T>): int => a.Sum();\n"
        "end D;\n"
        "class E: IA\n"
        "  public method S<T>(a: Array<T>): int => a.Sum();\n"
        "end E;\n",
        "      var d := new D();\n"
        "      var ia: IA := d;\n"
        "      var e := new E();\n"
        "      t := t + d.S(a) + ia.S(a) + e.S(a);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3300\n");
}

// CONTROL: a genuine `override` (omitted `where`) of a constrained virtual
// inherits `where T: INumber<T>` — one level, and through an intermediate
// class that omits it too — and an override inheriting only position 1 keeps
// the §5 `Sum()` at that position alone (§14.2: "correspond by position").
// Per iteration: 11 + 11 + 11 = 33; the last assignment adds
// 4040 * 100000 + 11 once. 100 x 33 + 404000011 = 404003311. PARENT: the same.
BEZEL_TEST(codegen_linq_inherited_where_exact_relation_8184,
           control_genuine_override_keeps_the_inherited_builtin) {
    JitResult r = runAotChecked(
        "module M;\n"
        "extension Object\n"
        "  public method Sum(): int => 4040;\n"
        "end;\n"
        "class B\n"
        "  public virtual method S<T>(a: Array<T>): int where T: INumber<T> =>"
        " a.Sum();\n"
        "  public virtual method P<A, Z>(a: Array<A>, b: Array<Z>): int"
        " where Z: INumber<Z> => b.Sum();\n"
        "end B;\n"
        "class Mid: B\n"
        "  public override method S<T>(a: Array<T>): int => a.Sum();\n"
        "end Mid;\n"
        "class Leaf: Mid\n"
        "  public override method S<T>(a: Array<T>): int => a.Sum();\n"
        "  public override method P<X, Y>(a: Array<X>, b: Array<Y>): int =>"
        " a.Sum() * 100000 + b.Sum();\n"
        "end Leaf;\n"
        "class Prog\n"
        "  public static method Main(): int\n"
        "    var i := 0;\n"
        "    var t := 0;\n"
        "    var l := new Leaf();\n"
        "    var lb: B := l;\n"
        "    var m := new Mid();\n"
        "    while i < 100 do\n"
        "      var a := new Array<int>();\n"
        "      a.Add(5);\n"
        "      a.Add(6);\n"
        "      t := t + l.S(a) + lb.S(a) + m.S(a);\n"
        "      i := i + 1;\n"
        "    end;\n"
        "    var c := new Array<int>();\n"
        "    c.Add(5);\n"
        "    c.Add(6);\n"
        "    t := t + lb.P(c, c);\n"
        "    Console.WriteLine(t);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Prog;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "404003311\n");
}
