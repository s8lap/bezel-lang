// fix #8181 (stdlib §5.1, spec §14.2 *Interface conformance*, §26.5.1, v0.355):
// inside a GENERIC body, the §5 block's `where` clause is decided by the type
// parameter's OWN constraints. An unconstrained `T` satisfies no `where T: …`
// clause, so on a receiver of element type `T` the §5 `Sum()` / `Min()` /
// `Distinct()` overloads are NOT applicable candidates and a same-named user
// extension — the only applicable candidate — answers, at every instantiation.
//
// SPEC BASIS (QUOTED, v0.355).
//   stdlib §5.1: "`Sum` and `Average` are only available on numeric
//     sequences: `public extension IEnumerable<T> where T: INumber<T>`"; "`Min`
//     and `Max` without a selector are generic over comparable types: `public
//     extension IEnumerable<T> where T: IComparable<T>`".
//   spec §14.2 *Interface conformance*: "For an interface I and a candidate
//     type T, T **conforms to** I exactly when one of the following holds:
//     ... 6. **T is a type parameter** whose constraints guarantee conformance
//     to I". The same section: "an unconstrained `U` contributes only
//     `Object`'s surface."
//   spec §26.5.1: "... with block-level `where` clauses satisfied by the
//     arguments inferred from the receiver. A `where` clause is an
//     *applicability* filter, never a ranking signal."
//
// INTERPRETATION (recorded in the commit). The generic body is checked once,
// against `T`, not per instantiation: `G<T>(a: Array<T>) => a.Sum()` with an
// unconstrained `T` therefore resolves to the user's `extension Object { Sum() }`
// whether `G` is later instantiated at `string` or at `int`. The `G<int>` row
// below is a BEHAVIOUR FLIP from the parent (which printed 5, the §5 answer
// picked per instantiation).
//
// MEASURED ON THE PARENT (r30-c1 @ 737e6d0a, plus the #8180 commit):
//   * `G<T>(a: Array<T>) => a.Sum()` at T = string with `extension Object
//     { Sum(): int => 4242 }`: "IR verification failed (malformed IR emitted)".
//     The same for a generic CLASS `C<T>` method and for `this.Sum()` inside
//     `extension Array<T>`.
//   * the same at T = int: printed 5.
//   * `G<T>(a: Array<T>) => a.Min()` / `a.Distinct()` with the extension: REFUSED
//     by the per-instantiation gate although the extension applies.
//
// Leak-checked rows of the same shapes live in
// tests/codegen/linq_generic_body_where_applicability_8181_test.cpp.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

const char* kObjectSum =
    "module M;\n"
    "extension Object\n"
    "  public method Sum(): int => 4242;\n"
    "end;\n";

}  // namespace

// ---------------------------------------------------------------------------
// R1 — the issue's repro: a generic `a.Sum()` at T = string.
// PARENT: malformed IR.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_linq_generic_body_where_applicability_8181,
           generic_sum_at_string_answers_the_applicable_extension) {
    JitResult r = runAot(std::string(kObjectSum) +
        "class C\n"
        "  static method G<T>(a: Array<T>): int => a.Sum();\n"
        "  public static method Main(): int\n"
        "    var a := new Array<string>();\n"
        "    a.Add(\"x\");\n"
        "    Console.WriteLine(G(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n");
}

// The generic-CLASS and the `extension Array<T>` body twins. PARENT: malformed IR.
BEZEL_TEST(sema_linq_generic_body_where_applicability_8181,
           generic_class_method_sum_answers_the_applicable_extension) {
    JitResult r = runAot(std::string(kObjectSum) +
        "class C<T>\n"
        "  public method G(a: Array<T>): int => a.Sum();\n"
        "end C;\n"
        "class D\n"
        "  public static method Main(): int\n"
        "    var a := new Array<string>();\n"
        "    a.Add(\"x\");\n"
        "    var c := new C<string>();\n"
        "    Console.WriteLine(c.G(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end D;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n");
}

BEZEL_TEST(sema_linq_generic_body_where_applicability_8181,
           extension_block_this_sum_answers_the_applicable_extension) {
    JitResult r = runAot(std::string(kObjectSum) +
        "extension Array<T>\n"
        "  public method Total(): int => this.Sum();\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<string>();\n"
        "    a.Add(\"x\");\n"
        "    Console.WriteLine(a.Total());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n");
}

// ---------------------------------------------------------------------------
// R2 — BEHAVIOUR FLIP: the same generic body at T = int. `T` is unconstrained,
// so (§14.2 rule 6) it does not conform to `INumber<T>`; the §5 `Sum()` is not
// applicable in the body and the extension answers at every instantiation.
// PARENT: printed 5 (the §5 answer, chosen per instantiation).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_linq_generic_body_where_applicability_8181,
           generic_sum_at_int_also_answers_the_extension) {
    JitResult r = runAot(std::string(kObjectSum) +
        "class C\n"
        "  static method G<T>(a: Array<T>): int => a.Sum();\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(5);\n"
        "    Console.WriteLine(G(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n");
}

// The `IComparable<T>` and `hashable` blocks, the same rule.
// PARENT: both REFUSED by the per-instantiation gate.
BEZEL_TEST(sema_linq_generic_body_where_applicability_8181,
           generic_min_and_distinct_answer_the_applicable_extension) {
    JitResult r = runAot(
        "module M;\n"
        "extension Object\n"
        "  public method Min(): int => 4242;\n"
        "  public method Distinct(): int => 4343;\n"
        "end;\n"
        "class C\n"
        "  static method G<T>(a: Array<T>): int => a.Min();\n"
        "  static method H<T>(a: Array<T>): int => a.Distinct();\n"
        "  public static method Main(): int\n"
        "    var a := new Array<string>();\n"
        "    a.Add(\"x\");\n"
        "    Console.WriteLine(G(a));\n"
        "    var b := new Array<int>();\n"
        "    b.Add(5);\n"
        "    Console.WriteLine(H(b));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n4343\n");
}

// ---------------------------------------------------------------------------
// CONTROLS (silent-acceptance mirror). A CONSTRAINED `T` satisfies its clause
// (§14.2 rule 6), so the §5 overload is applicable and, being more specific
// than `Object` (§26.5.1), keeps the call. A concrete `Array<int>.Sum()` with
// the extension in scope keeps the §5 answer. PARENT: the same output.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_linq_generic_body_where_applicability_8181,
           constrained_type_parameter_keeps_the_builtin_sum_and_distinct) {
    JitResult r = runAot(
        "module M;\n"
        "extension Object\n"
        "  public method Sum(): int => 4242;\n"
        "  public method Distinct(): int => 4343;\n"
        "end;\n"
        "class C\n"
        "  static method G<T>(a: Array<T>): T where T: INumber<T> => a.Sum();\n"
        "  static method H<T>(a: Array<T>): int where T: hashable =>"
        " a.Distinct().Count();\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(5);\n"
        "    a.Add(6);\n"
        "    Console.WriteLine(G(a));\n"
        "    var b := new Array<int>();\n"
        "    b.Add(5);\n"
        "    b.Add(5);\n"
        "    b.Add(6);\n"
        "    Console.WriteLine(H(b));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\n2\n");
}

BEZEL_TEST(sema_linq_generic_body_where_applicability_8181,
           concrete_int_sum_keeps_the_builtin_with_the_extension) {
    JitResult r = runAot(std::string(kObjectSum) +
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(5);\n"
        "    a.Add(6);\n"
        "    Console.WriteLine(a.Sum());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\n");
}
