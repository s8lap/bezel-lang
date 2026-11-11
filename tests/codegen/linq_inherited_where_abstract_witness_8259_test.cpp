// fix #8259 (spec §14.2 *Constraints on overrides and interface
// implementations*, §11.2, §12.5, §26.5.1, v0.357): the inherited-`where`
// lookup (`ExprTC::currentMethodTypeParamMayInherit`, #8181 / #8184) follows
// the §11.2 WITNESS `checkAbstractImplementations` records for each
// (type, interface obligation). That check skipped an `abstract` / `closed`
// class entirely (such a class need not implement every obligation, §12.5),
// so such a class recorded NO witness and the reader fell back to its
// shape-based answer, which keeps a sibling interface's `where T: INumber<T>`.
//
// §14.2: "An `override` of a generic method, and a method implementing a
// generic interface method ... inherits the overridden or implemented
// declaration's constraints." Only the IMPLEMENTED declaration contributes.
// An abstract class's `S<T>(a: Array<T>)` implements IB's UNconstrained
// method; IA's constrained `S<T>(s: string)` is a different member. `T` in
// the body is therefore unconstrained and the stdlib §5 `Sum()` (bound by
// `T: INumber<T>`) is not applicable, so the user's `extension Object { Sum() }`
// answers (§26.5.1, §14.2).
//
// MEASURED ON THE PARENT (r30-c1 @ e49c32aca): the abstract-class RED rows
// below were REFUSED with "the returned value has a type that is not
// implicitly convertible ... `T` is a type parameter" -- the §5 `Sum(): T`
// candidate was kept. The non-abstract twin printed the extension's answer.
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

// The issue's p19 (a): an ABSTRACT class implementing IA (constrained
// `S<T>(s: string)`) and IB (unconstrained `S<T>(a: Array<T>)`). Called on the
// concrete subclass and through IB: 100 x 2 x 4040 = 808000. PARENT: compile
// refusal (the §5 `Sum(): T` candidate kept).
BEZEL_TEST(codegen_linq_inherited_where_abstract_witness_8259,
           a_abstract_class_implementing_the_unconstrained_interface_answers_the_extension) {
    JitResult r = runAotChecked(looped(
        "interface IA\n"
        "  method S<T>(s: string): int where T: INumber<T>;\n"
        "end IA;\n"
        "interface IB\n"
        "  method S<T>(a: Array<T>): int;\n"
        "end IB;\n"
        "abstract class A: IA, IB\n"
        "  public method S<T>(s: string): int => 1;\n"
        "  public method S<T>(a: Array<T>): int => a.Sum();\n"
        "end A;\n"
        "class D: A\n"
        "end D;\n",
        "      var d := new D();\n"
        "      var ib: IB := d;\n"
        "      t := t + d.S(a) + ib.S(a);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "808000\n");
}

// The `closed` twin of (a): `closed` subsumes `abstract` for the §12.5 skip
// (`typeIsAbstractOrClosed`). 100 x 4040 = 404000. PARENT: compile refusal.
BEZEL_TEST(codegen_linq_inherited_where_abstract_witness_8259,
           a_closed_class_implementing_the_unconstrained_interface_answers_the_extension) {
    JitResult r = runAotChecked(looped(
        "interface IA\n"
        "  method S<T>(s: string): int where T: INumber<T>;\n"
        "end IA;\n"
        "interface IB\n"
        "  method S<T>(a: Array<T>): int;\n"
        "end IB;\n"
        "closed class A: IA, IB\n"
        "  public method S<T>(s: string): int => 1;\n"
        "  public method S<T>(a: Array<T>): int => a.Sum();\n"
        "end A;\n"
        "class D: A\n"
        "end D;\n",
        "      var d := new D();\n"
        "      t := t + d.S(a);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "404000\n");
}

// The abstract twin of #8184's w2: the abstract class implements IA's
// constrained `S<T>(s: string)` and ALSO overloads the name with an
// `S<T>(a: Array<T>)` that implements nothing. 100 x 4040 = 404000.
// PARENT: compile refusal.
BEZEL_TEST(codegen_linq_inherited_where_abstract_witness_8259,
           a_abstract_overload_of_an_implemented_interface_name_answers_the_extension) {
    JitResult r = runAotChecked(looped(
        "interface IA\n"
        "  method S<T>(s: string): int where T: INumber<T>;\n"
        "end IA;\n"
        "abstract class A: IA\n"
        "  public method S<T>(s: string): int => 1;\n"
        "  public method S<T>(a: Array<T>): int => a.Sum();\n"
        "end A;\n"
        "class D: A\n"
        "end D;\n",
        "      var d := new D();\n"
        "      t := t + d.S(a);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "404000\n");
}

// CONTROL (the silent-acceptance mirror): the abstract class's `Array<T>`
// overload IS the implementation of the CONSTRAINED interface method, so it
// inherits `where T: INumber<T>` and the §5 `Sum()` applies -- with the
// witness now recorded for the abstract class, that must stay true. The class
// also leaves a second interface member UNimplemented (legal for an abstract
// class, §12.5). 100 x 11 = 1100. PARENT: 1100.
BEZEL_TEST(codegen_linq_inherited_where_abstract_witness_8259,
           control_abstract_implementation_of_the_constrained_interface_keeps_the_builtin) {
    JitResult r = runAotChecked(looped(
        "interface IA\n"
        "  method S<T>(a: Array<T>): T where T: INumber<T>;\n"
        "end IA;\n"
        "interface IB\n"
        "  method Z(): int;\n"
        "end IB;\n"
        "abstract class A: IA, IB\n"
        "  public method S<T>(a: Array<T>): T => a.Sum();\n"
        "end A;\n"
        "class D: A\n"
        "  public method Z(): int => 0;\n"
        "end D;\n",
        "      var d := new D();\n"
        "      t := t + d.S(a);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1100\n");
}
