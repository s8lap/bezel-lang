// fix #8181 (stdlib §5.1, spec §14.2 *Interface conformance* rule 6, §26.5.1,
// v0.355): the leak-checked (runAotChecked: generated-code ASan + LeakSanitizer)
// rows for a §5 LINQ call inside a GENERIC body. An unconstrained `T` satisfies
// no `where T: …` clause (§14.2 rule 6: "T is a type parameter whose constraints
// guarantee conformance to I"), so on an `Array<T>` receiver the §5 `Sum()` is
// not an applicable candidate and a same-named `extension Object` member answers,
// at every instantiation; a constrained `T` keeps the §5 overload.
//
// MEASURED ON THE PARENT (r30-c1 @ 737e6d0a, plus the #8180 commit): the
// T = string rows aborted in `verifyModule` ("IR verification failed (malformed
// IR emitted)"); the T = int row printed 5 per call (BEHAVIOUR FLIP, see the
// sema file's header); the constrained controls printed the same output.
//
// The rows are LOOPED (100 iterations) so a per-call leak cannot hide. The
// diagnostic-free sema rows live in
// tests/sema/linq_generic_body_where_applicability_8181_test.cpp.

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
           "  public method Sum(): int => 4242;\n"
           "end;\n" + decls +
           "class D\n"
           "  public static method Main(): int\n"
           "    var i := 0;\n"
           "    var t := 0;\n"
           "    while i < 100 do\n" +
           loopBody +
           "      i := i + 1;\n"
           "    end;\n"
           "    Console.WriteLine(t);\n"
           "    return 0;\n"
           "  end Main;\n"
           "end D;\n";
}

}  // namespace

// The issue's repro (T = string), its generic-class and `extension Array<T>`
// twins. 100 x 3 x 4242 = 1272600. PARENT: malformed IR.
BEZEL_TEST(codegen_linq_generic_body_where_applicability_8181,
           generic_sum_at_string_answers_the_extension_without_leaking) {
    JitResult r = runAotChecked(looped(
        "class C\n"
        "  public static method G<T>(a: Array<T>): int => a.Sum();\n"
        "end C;\n"
        "class E<T>\n"
        "  public method G(a: Array<T>): int => a.Sum();\n"
        "end E;\n"
        "extension Array<T>\n"
        "  public method Total(): int => this.Sum();\n"
        "end;\n",
        "      var a := new Array<string>();\n"
        "      a.Add(\"x\");\n"
        "      var e := new E<string>();\n"
        "      t := t + C.G(a) + e.G(a) + a.Total();\n"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1272600\n");
}

// BEHAVIOUR FLIP: the same unconstrained body at T = int. 100 x 4242.
// PARENT: 500 (the §5 `Sum()`, 5 per call).
BEZEL_TEST(codegen_linq_generic_body_where_applicability_8181,
           generic_sum_at_int_answers_the_extension_without_leaking) {
    JitResult r = runAotChecked(looped(
        "class C\n"
        "  public static method G<T>(a: Array<T>): int => a.Sum();\n"
        "end C;\n",
        "      var a := new Array<int>();\n"
        "      a.Add(5);\n"
        "      t := t + C.G(a);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "424200\n");
}

// CONTROL (silent-acceptance mirror): an `INumber<T>`-constrained body and a
// concrete `Array<int>` keep the §5 `Sum()`. 100 x (11 + 11) = 2200.
// PARENT: the same output.
BEZEL_TEST(codegen_linq_generic_body_where_applicability_8181,
           constrained_and_concrete_sum_keep_the_builtin_without_leaking) {
    JitResult r = runAotChecked(looped(
        "class C\n"
        "  public static method G<T>(a: Array<T>): T where T: INumber<T> =>"
        " a.Sum();\n"
        "end C;\n",
        "      var a := new Array<int>();\n"
        "      a.Add(5);\n"
        "      a.Add(6);\n"
        "      t := t + C.G(a) + a.Sum();\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2200\n");
}

// fix-forward #8181 (spec §14.2 *Constraints on overrides and interface
// implementations*, v0.355): QUOTED "An `override` of a generic method, and a
// method implementing a generic interface method ... inherits the overridden
// or implemented declaration's constraints. Omitting the `where` clauses is
// the recommended spelling and means exactly the inherited set". An override
// written in that recommended spelling has an `INumber<T>`-constrained `T`, so
// the §5 `Sum()` applies and is the more specific candidate: 11 per call —
// directly, through the base, and through a TWO-level omitted chain.
// 100 x (11 + 11 + 11 + 11) = 4400.
// PARENT (737e6d0a / bbdd4bb8): 4400. dece4010: 100 x 4 x 4242 = 1696800
// (the omitted clause was read as "unconstrained" — review r39-rev-8180 o1).
BEZEL_TEST(codegen_linq_generic_body_where_applicability_8181,
           override_with_omitted_where_keeps_the_inherited_inumber_sum_without_leaking) {
    JitResult r = runAotChecked(looped(
        "class B\n"
        "  public virtual method S<T>(a: Array<T>): int where T: INumber<T> =>"
        " a.Sum();\n"
        "end B;\n"
        "class O: B\n"
        "  public override method S<T>(a: Array<T>): int => a.Sum();\n"
        "end O;\n"
        "class Mid: B\n"
        "  public override method S<T>(a: Array<T>): int => a.Sum();\n"
        "end Mid;\n"
        "class Leaf: Mid\n"
        "  public override method S<T>(a: Array<T>): int => a.Sum();\n"
        "end Leaf;\n",
        "      var a := new Array<int>();\n"
        "      a.Add(5);\n"
        "      a.Add(6);\n"
        "      var o := new O();\n"
        "      var b: B := o;\n"
        "      var l := new Leaf();\n"
        "      var lb: B := l;\n"
        "      t := t + o.S(a) + b.S(a) + l.S(a) + lb.S(a);\n"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4400\n");
}

// fix-forward #8181: the INTERFACE-IMPLEMENTATION half of the same §14.2
// sentence (which #5039's materialization does not cover: it runs for
// `override` only). A direct implementation, one reached through an extending
// interface, and one of a GENERIC interface's method ("the inherited set is
// read under the implemented interface's ... substitution"), each called
// directly and through the interface. 100 x 6 x 11 = 6600.
// PARENT (737e6d0a / bbdd4bb8): 6600. dece4010: 100 x 6 x 4242 = 2545200
// (review r39-rev-8180 o2).
BEZEL_TEST(codegen_linq_generic_body_where_applicability_8181,
           interface_implementation_with_omitted_where_keeps_the_inherited_inumber_sum_without_leaking) {
    JitResult r = runAotChecked(looped(
        "interface IA\n"
        "  method S<T>(a: Array<T>): int where T: INumber<T>;\n"
        "end IA;\n"
        "interface IB: IA\n"
        "  method Q(): int;\n"
        "end IB;\n"
        "interface IG<U>\n"
        "  method S<T>(a: Array<T>, u: U): int where T: INumber<T>;\n"
        "end IG;\n"
        "class X: IA\n"
        "  public method S<T>(a: Array<T>): int => a.Sum();\n"
        "end X;\n"
        "class Y: IB\n"
        "  public method S<T>(a: Array<T>): int => a.Sum();\n"
        "  public method Q(): int => 0;\n"
        "end Y;\n"
        "class Z: IG<string>\n"
        "  public method S<T>(a: Array<T>, u: string): int => a.Sum();\n"
        "end Z;\n",
        "      var a := new Array<int>();\n"
        "      a.Add(5);\n"
        "      a.Add(6);\n"
        "      var x := new X();\n"
        "      var xi: IA := x;\n"
        "      var y := new Y();\n"
        "      var yi: IA := y;\n"
        "      var z := new Z();\n"
        "      var zi: IG<string> := z;\n"
        "      t := t + x.S(a) + xi.S(a) + y.S(a) + yi.S(a) + z.S(a, \"u\")"
        " + zi.S(a, \"v\");\n"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "6600\n");
}

// CONTROL for the two rows above (the reviewer's ruling, point 3, kept): an
// override of an UNCONSTRAINED virtual inherits the EMPTY set, so its `T` is
// as unconstrained as `G<T>`'s above and the extension answers, directly and
// through the base. 100 x 2 x 4242 = 848400.
// PARENT (737e6d0a): 2200 (the §5 `Sum()` re-decided per instantiation);
// dece4010: 848400.
BEZEL_TEST(codegen_linq_generic_body_where_applicability_8181,
           override_of_an_unconstrained_virtual_answers_the_extension_without_leaking) {
    JitResult r = runAotChecked(looped(
        "class B\n"
        "  public virtual method S<T>(a: Array<T>): int => a.Sum();\n"
        "end B;\n"
        "class O: B\n"
        "  public override method S<T>(a: Array<T>): int => a.Sum();\n"
        "end O;\n",
        "      var a := new Array<int>();\n"
        "      a.Add(5);\n"
        "      a.Add(6);\n"
        "      var o := new O();\n"
        "      var b: B := o;\n"
        "      t := t + o.S(a) + b.S(a);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "848400\n");
}
