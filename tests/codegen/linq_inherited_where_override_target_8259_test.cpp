// fix #8259 part (b) (spec §14.2 *Constraints on overrides and interface
// implementations*, §12.1, §12.2, §26.5.1, v0.357): the inherited-`where`
// lookup (`ExprTC::currentMethodTypeParamMayInherit`, #8181 / #8184) walked
// the CLASS bases by SHAPE (name, parameter count, generic arity) for an
// `override`, so an override of the UNconstrained `S<T>(a: Array<T>)` also
// inherited `where T: INumber<T>` from a same-shape SIBLING overload
// `S<T>(k: int)` it does not override.
//
// §14.2: "An `override` of a generic method ... inherits the overridden ...
// declaration's constraints." Only the OVERRIDDEN declaration contributes.
// `T` in the override body is then unconstrained, the stdlib §5 `Sum()`
// (bound by `T: INumber<T>`) is not applicable, and the user's
// `extension Object { Sum() }` answers (§26.5.1, §14.2).
//
// The Resolver (`checkHidingAndOverride`) now records each override's target
// (`SemaContext::overrideTarget`) and the reader follows that chain.
//
// MEASURED ON THE PARENT (d703dd1a2): the RED rows below were REFUSED with
// "the returned value has a type that is not implicitly convertible ... `T` is
// a type parameter" -- the §5 `Sum(): T` candidate was kept.
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

// The issue's p25: an override of the unconstrained overload, with a
// constrained same-name same-arity sibling. 100 x 4040 = 404000.
// PARENT: compile refusal.
BEZEL_TEST(codegen_linq_inherited_where_override_target_8259,
           p25_override_of_unconstrained_overload_answers_the_extension) {
    JitResult r = runAotChecked(looped(
        "class B\n"
        "  public virtual method S<T>(a: Array<T>): int => 0;\n"
        "  public virtual method S<T>(k: int): int where T: INumber<T> => 0;\n"
        "end B;\n"
        "class D: B\n"
        "  public override method S<T>(a: Array<T>): int => a.Sum();\n"
        "end D;\n",
        "      var d := new D();\n"
        "      t := t + d.S(a);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "404000\n");
}

// The generic-base twin: `D: B<int>`. PARENT: compile refusal.
BEZEL_TEST(codegen_linq_inherited_where_override_target_8259,
           generic_base_twin_override_of_unconstrained_overload_answers_the_extension) {
    JitResult r = runAotChecked(looped(
        "class B<U>\n"
        "  public virtual method S<T>(a: Array<T>): int => 0;\n"
        "  public virtual method S<T>(k: int): int where T: INumber<T> => 0;\n"
        "end B;\n"
        "class D: B<int>\n"
        "  public override method S<T>(a: Array<T>): int => a.Sum();\n"
        "end D;\n",
        "      var d := new D();\n"
        "      t := t + d.S(a);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "404000\n");
}

// A two-level chain: C overrides B's unconstrained overload, D overrides C's.
// The grandbase has the constrained sibling. 100 x 4040 = 404000.
// PARENT: compile refusal.
BEZEL_TEST(codegen_linq_inherited_where_override_target_8259,
           override_chain_of_unconstrained_overload_answers_the_extension) {
    JitResult r = runAotChecked(looped(
        "class B\n"
        "  public virtual method S<T>(a: Array<T>): int => 0;\n"
        "  public virtual method S<T>(k: int): int where T: INumber<T> => 0;\n"
        "end B;\n"
        "class C: B\n"
        "  public override method S<T>(a: Array<T>): int => 1;\n"
        "end C;\n"
        "class D: C\n"
        "  public override method S<T>(a: Array<T>): int => a.Sum();\n"
        "end D;\n",
        "      var d := new D();\n"
        "      t := t + d.S(a);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "404000\n");
}

// CONTROL (the silent-acceptance mirror): the override DOES override the
// constrained method, so it inherits `where T: INumber<T>` and the §5
// `Sum()` applies. 100 x 11 = 1100. PARENT: 1100.
BEZEL_TEST(codegen_linq_inherited_where_override_target_8259,
           control_override_of_the_constrained_method_keeps_the_builtin) {
    JitResult r = runAotChecked(looped(
        "class B\n"
        "  public virtual method S<T>(a: Array<T>): T where T: INumber<T> => a.Sum();\n"
        "  public virtual method S<T>(k: int): int => 0;\n"
        "end B;\n"
        "class D: B\n"
        "  public override method S<T>(a: Array<T>): T => a.Sum();\n"
        "end D;\n",
        "      var d := new D();\n"
        "      t := t + d.S(a);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1100\n");
}

// CONTROL: the constraint is declared TWO levels up and inherited down an
// override chain; the leaf still sees it. 100 x 11 = 1100. PARENT: 1100.
BEZEL_TEST(codegen_linq_inherited_where_override_target_8259,
           control_constraint_inherited_down_an_override_chain_keeps_the_builtin) {
    JitResult r = runAotChecked(looped(
        "class B\n"
        "  public virtual method S<T>(a: Array<T>): T where T: INumber<T> => a.Sum();\n"
        "  public virtual method S<T>(k: int): int => 0;\n"
        "end B;\n"
        "class C: B\n"
        "  public override method S<T>(a: Array<T>): T => a.Sum();\n"
        "end C;\n"
        "class D: C\n"
        "  public override method S<T>(a: Array<T>): T => a.Sum();\n"
        "end D;\n",
        "      var d := new D();\n"
        "      t := t + d.S(a);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1100\n");
}
