// fix #8198 (spec §14.2 *A type parameter as the type argument* /
// *Interface conformance* rule 6, §26.5.1, v0.355): the leak-checked
// (runAotChecked: generated-code ASan + LeakSanitizer) rows for a §5 LINQ
// call inside a GENERIC body whose type parameter carries a constraint that
// does NOT guarantee the §5 block's `where T: INumber<T>` clause.
//
// THE DEFECT. §26.5.1 (v0.355) ranks "the APPLICABLE candidates": extension
// members "that match the call's arity and argument types, from blocks whose
// head matches R ..., with block-level `where` clauses satisfied by the
// arguments inferred from the receiver. A `where` clause is an *applicability*
// filter, never a ranking signal." The §5 candidate's applicability filter
// (fix #7967) decided an UNCONSTRAINED type parameter (fix #8181) but KEPT a
// CONSTRAINED one whatever the constraint was, so `where T: class` /
// `where T: notnull` — which name NO type bound and therefore guarantee NO
// conformance — kept the §5 `Sum()`.
//
// §14.2 *A type parameter as the type argument* case 1: "The argument is a
// type parameter `P` ... the constraint is decided at that site, by `P`'s own
// constraints. `P` satisfies the constraint exactly when its constraints
// guarantee it at every substitution they admit, as a matter of language
// rule ... A type bound guarantees `where T: IFoo` by *Interface conformance*
// rule 6 below". §14.2 *Interface conformance* rule 6: "T is a type parameter
// whose constraints guarantee conformance to I: a type-bound constraint naming
// I or an interface extending I, or a class bound naming a class that conforms
// to I under rule 1, directly or through a `where T: U` chain."
//
// MEASURED ON THE PARENT (r30-c1 @ b52409a7), the issue's own repros:
//
//   where T: class,   Array<string>   COMPILE ERROR:
//       "Both operands to a binary operator are not of the same type!
//        %11 = add i32 %10, ptr %9" ...
//       "bezelc: codegen error: IR verification failed (malformed IR emitted;
//        see report above)" — the §5 `Sum()` was lowered per instantiation
//        over a `string` element.
//   where T: notnull, Array<int> {5,6}  COMPILES and PRINTS 11 (the §5
//        `Sum()`), not 4040 (the user's `extension Object { Sum() }`).
//
// The rows are LOOPED (100 iterations) so a per-call leak cannot hide.
//
// SPEC BASIS. §26.5.1 *Specificity ordering* (v0.355), §14.2 *A type
// parameter as the type argument* and *Interface conformance* rule 6
// (v0.355), stdlib §5.1 (`extension IEnumerable<T> where T: INumber<T>` — the
// no-selector `Sum()` row) and stdlib §2.19 (the `INumber<T>` conformers).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The issue's REPRO A, verbatim in shape: a `where T: class` generic method
// whose `a.Sum()` is answered by the user's `extension Object { Sum() }`
// when the element type is a `string`.
std::string classConstrainedSum() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Min(): int => 4242;\n"
        "  public method Sum(): int => 4040;\n"
        "end;\n"
        "class C\n"
        "  static method G<T>(a: Array<T>): int where T: class => a.Sum();\n"
        "  public static method Main(): int\n"
        "    var a := new Array<string>();\n"
        "    a.Add(\"x\");\n"
        "    Console.WriteLine(G(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// The issue's REPRO B, verbatim in shape: the same extension, the
// `where T: notnull` generic method, and an `Array<int>` holding 5 and 6.
std::string notnullConstrainedSum() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Min(): int => 4242;\n"
        "  public method Sum(): int => 4040;\n"
        "end;\n"
        "class C\n"
        "  static method G<T>(a: Array<T>): int where T: notnull => a.Sum();\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(5); a.Add(6);\n"
        "    Console.WriteLine(G(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// CONTROL 1 — an `INumber<T>`-constrained body KEEPS the §5 `Sum()`: the
// type parameter's own constraint DOES guarantee the clause (§14.2 *Interface
// conformance* rule 6), so the §5 candidate is applicable and, its
// `IEnumerable<int>` head being a subtype of `Object` (§26.5.1), it is the
// more specific candidate. The value printed is the §5 `Sum()`'s (5 + 6 = 11),
// NOT the extension's 4040.
std::string inumberConstrainedSum() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Min(): int => 4242;\n"
        "  public method Sum(): int => 4040;\n"
        "end;\n"
        "class C\n"
        "  static method G<T>(a: Array<T>): int where T: INumber<T> =>"
        " a.Sum();\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(5); a.Add(6);\n"
        "    Console.WriteLine(G(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// CONTROL 2 — an UNCONSTRAINED body keeps the extension's answer (the #8181
// fix): an unconstrained parameter guarantees no conformance either.
std::string unconstrainedSum() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Min(): int => 4242;\n"
        "  public method Sum(): int => 4040;\n"
        "end;\n"
        "class C\n"
        "  static method G<T>(a: Array<T>): int => a.Sum();\n"
        "  public static method Main(): int\n"
        "    var a := new Array<string>();\n"
        "    a.Add(\"x\");\n"
        "    Console.WriteLine(G(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// CONTROL 3 — a CONCRETE `Array<int>`'s `Sum()` is the §5 one (the receiver's
// element type is not a type parameter at all, so the §26.5.1 filter's
// type-parameter arm does not apply).
std::string concreteSum() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Min(): int => 4242;\n"
        "  public method Sum(): int => 4040;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(5); a.Add(6);\n"
        "    Console.WriteLine(a.Sum());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// The LOOPED shape: 100 iterations of the issue's two repro bodies plus the
// `INumber<T>`-constrained control, so a per-call leak cannot hide.
std::string looped() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Min(): int => 4242;\n"
        "  public method Sum(): int => 4040;\n"
        "end;\n"
        "class C\n"
        "  static method G<T>(a: Array<T>): int where T: class => a.Sum();\n"
        "  static method N<T>(a: Array<T>): int where T: notnull => a.Sum();\n"
        "  static method I<T>(a: Array<T>): int where T: INumber<T> =>"
        " a.Sum();\n"
        "end C;\n"
        "class D\n"
        "  public static method Main(): int\n"
        "    var i := 0;\n"
        "    var t := 0;\n"
        "    while i < 100 do\n"
        "      var s := new Array<string>();\n"
        "      s.Add(\"x\");\n"
        "      var n := new Array<int>();\n"
        "      n.Add(5); n.Add(6);\n"
        "      t := t + C.G(s) + C.N(n) + C.I(n);\n"
        "      i := i + 1;\n"
        "    end;\n"
        "    Console.WriteLine(t);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end D;\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// R1 — the issue's REPRO A: `where T: class` over an `Array<string>`. The
// §5 `Sum()`'s `where T: INumber<T>` clause is unsatisfied at the type
// parameter (`class` names no type bound), so the §5 candidate is NOT
// applicable and the user's `extension Object { Sum() }` — the only APPLICABLE
// candidate — answers 4040.
//
// PARENT (r30-c1 @ b52409a7): a COMPILE ERROR — "IR verification failed
// (malformed IR emitted)"; the §5 `Sum()` was lowered over a `string`.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_generic_body_where_applicability_8198,
           class_constrained_generic_sum_answers_the_applicable_extension) {
    JitResult r = runAotChecked(classConstrainedSum());
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4040\n");
}

// ---------------------------------------------------------------------------
// R2 — the issue's REPRO B: `where T: notnull` over an `Array<int>` holding
// 5 and 6. Same verdict: the clause is unsatisfied at the type parameter, so
// the user's extension answers 4040.
//
// PARENT: the program COMPILES and PRINTS 11 (the §5 `Sum()`) — a BEHAVIOUR
// FLIP.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_generic_body_where_applicability_8198,
           notnull_constrained_generic_sum_answers_the_applicable_extension) {
    JitResult r = runAotChecked(notnullConstrainedSum());
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4040\n");
}

// ---------------------------------------------------------------------------
// R3 — the LOOPED row: 100 x (4040 + 4040 + 11) = 809100.
//
// PARENT: the program ABORTS at compile time (malformed IR) — the row fails
// on the `ran` / stdout assertions.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_generic_body_where_applicability_8198,
           constrained_generic_sums_are_leak_free_over_a_hundred_calls) {
    JitResult r = runAotChecked(looped());
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "809100\n");
}

// ---------------------------------------------------------------------------
// C1 — CONTROL: an `INumber<T>`-constrained body KEEPS the §5 `Sum()` (the
// constraint DOES guarantee the clause) — 11.
//
// PARENT: also 11 — this row must NOT change.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_generic_body_where_applicability_8198,
           inumber_constrained_generic_sum_keeps_the_builtin) {
    JitResult r = runAotChecked(inumberConstrainedSum());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\n");
}

// ---------------------------------------------------------------------------
// C2 — CONTROL: an UNCONSTRAINED body keeps the extension's answer (the
// #8181 fix) — 4040.
//
// PARENT: also 4040 — this row must NOT change.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_generic_body_where_applicability_8198,
           unconstrained_generic_sum_keeps_the_extension_answer) {
    JitResult r = runAotChecked(unconstrainedSum());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4040\n");
}

// ---------------------------------------------------------------------------
// C3 — CONTROL: a CONCRETE `Array<int>`'s `Sum()` is the §5 one — 11.
//
// PARENT: also 11 — this row must NOT change.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_generic_body_where_applicability_8198,
           concrete_array_sum_keeps_the_builtin) {
    JitResult r = runAotChecked(concreteSum());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\n");
}
