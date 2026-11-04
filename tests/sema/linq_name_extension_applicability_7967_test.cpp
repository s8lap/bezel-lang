// fix #7967 (§26.5 / §26.5.1, v0.355): a stdlib §5 LINQ overload that CANNOT
// APPLY must not beat an applicable USER extension.
//
// THE DEFECT. §26.5.1 (v0.355) ranks "the APPLICABLE candidates":
//
//   "For receiver type R, specificity ranks the applicable candidates:
//    extension members of the called name that match the call's arity and
//    argument types, from blocks whose head matches R, with block-level
//    `where` clauses satisfied by the arguments inferred from the receiver.
//    A `where` clause is an *applicability* filter, never a ranking signal."
//
// The stdlib §5 block (`extension IEnumerable<T>`, auto-imported per §2.3)
// entered that ranking by (NAME, ARITY) alone — `lookupExtensionMember`'s
// §5-candidate admission (fix #3343) consulted the §5 overload table's
// `(name, arity)` rows and nothing else. So a §5 overload that cannot apply
// still competed, and — its `IEnumerable<T>` head being a SUBTYPE of `Object`
// (§26.5.1's specificity rule) — it BEAT an applicable user
// `extension Object` member (§26.5 rule 3's "most specific type wins").
//
// MEASURED on this fix's parent (r30-c1 @ ff2855bf), the issue's own probes:
//
//   shape                                            base behaviour
//   Array<string>.Sum() + ext Object { Sum() }      COMPILE ERROR:
//       "Sum() requires the element type to satisfy `T: INumber<T>` …"
//       — the §5.1 gate fired although the §5 overload does NOT apply
//       (`string` is not an §2.19 INumber conformer) and the user's
//       `extension Object { Sum() }` IS applicable.
//   a.Where("x") + ext Object { Where(s: string) }  COMPILES and PRINTS
//       GARBAGE (the §5 operator ran with a `string` where its parameter is
//       `(T) -> bool`) and LEAKS `bz_lazy_where` — although the §5 overload
//       does NOT apply (the argument's type does not match) and the user's
//       `extension Object { Where(s: string) }` IS the only applicable
//       candidate.
//
// THE FIX. The §5 candidate's ADMISSION now carries §26.5.1's applicability
// filter (`ExprTC::stdlibLinqCandidateApplicable`, src/sema/ExprTypecheck.cpp):
// the block-level `where` clause is evaluated against the receiver's INFERRED
// element type T (the §26.5 step-2 substitution), and the call's ARGUMENT
// TYPES are checked against the overload's parameter shape. A §5 overload that
// does not apply is NOT admitted, so the §26.5.1 ranking runs over the
// APPLICABLE candidates only and the user's extension wins.
//
// THE CONTROLS below pin that the fix does not OVER-reach: the built-in §5
// operators still win where they DO apply (a numeric `Array<int>`'s `Sum()`,
// and a lambda argument's `Where` / `Select`), and a user extension with a
// DIFFERENT name still answers.
//
// THE ORACLE. `runAot` — the strict AOT path (fork-isolated
// `Driver::compileFile` + clang link + native exec). Every row asserts the
// program's STDOUT and its exit code: the program's BEHAVIOUR, not "it
// compiled". The rows here are NOT leak rows (the leak half of the same
// defect is #7969, whose rows live in
// tests/codegen/linq_name_extension_no_lazy_leak_7969_test.cpp under forced
// ASan); ASan is therefore OFF in this file, per CLAUDE.md.
//
// SPEC BASIS. §26.5 rules 2-4 (v0.355), §26.5.1 *Specificity ordering*
// (v0.355), §2.3 (the auto-imported stdlib §5 extension module), stdlib §5.1
// (`extension IEnumerable<T> where T: INumber<T>` — the `Sum()` / `Average()`
// row) and stdlib §2.19 (the `INumber<T>` conformers).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The issue's probe A, verbatim in shape: a `string` element type (NOT an
// §2.19 `INumber<T>` conformer, so the §5 `Sum()` overload does not apply)
// and a user `extension Object { Sum() }` that DOES apply.
std::string sumOverStringElements() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Sum(): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<string>();\n"
        "    a.Add(\"x\");\n"
        "    Console.WriteLine(a.Sum());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// The issue's probe B, verbatim in shape: a `string` argument where the §5
// `Where` overload's parameter is `(T) -> bool`, and a user
// `extension Object { Where(s: string) }` that DOES apply.
std::string whereWithStringArgument() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Where(s: string): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(1);\n"
        "    Console.WriteLine(a.Where(\"x\"));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// CONTROL 1 — the built-in `Sum()` still wins on a NUMERIC Array, with the
// same `extension Object { Sum() }` in scope. §26.5.1: the §5 block's
// `IEnumerable<int>` head IS a subtype of `Object`, so the §5 candidate is
// more specific AND applicable (`int` is an §2.19 `INumber<T>` conformer) —
// it keeps the call. The value printed is the §5 `Sum()`'s (1 + 2 = 3), NOT
// the extension's 4242.
std::string builtinSumStillWins() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Sum(): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(1);\n"
        "    a.Add(2);\n"
        "    Console.WriteLine(a.Sum());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// CONTROL 2 — the built-in `Where` / `Select` still win on a LAMBDA argument,
// with the same-named `extension Object` members in scope. The §5 overloads
// ARE applicable there (the argument is the `(T) -> bool` / `(T) -> int` the
// §5 signature declares), so they keep the call and the lazy pipeline's
// terminal answers: `Where(x > 1)` keeps 1 of the 2 elements, `Select(x + 1)`
// keeps 2.
std::string builtinWhereSelectStillWin() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Where(s: string): int => 4242;\n"
        "  public method Select(s: string): int => 4343;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(1);\n"
        "    a.Add(2);\n"
        "    var q := a.Where(lambda(x) => x > 1);\n"
        "    Console.WriteLine(q.Count());\n"
        "    var s := a.Select(lambda(x) => x + 1);\n"
        "    Console.WriteLine(s.Count());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// CONTROL 3 — a user extension with a DIFFERENT name still answers (the fix
// is about the §5 NAME set, not about extensions in general).
std::string differentlyNamedExtensionStillWorks() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Total(): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<string>();\n"
        "    a.Add(\"x\");\n"
        "    Console.WriteLine(a.Total());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// R1 — the issue's probe A: `Array<string>.Sum()` with an applicable
// `extension Object { Sum() }`. The §5 `Sum()` overload does NOT apply (its
// block-level `where T: INumber<T>` is unsatisfied at T = string), so §26.5.1
// ranks the user's member as the only APPLICABLE candidate and it answers.
//
// BASE (r30-c1 @ ff2855bf): a COMPILE ERROR — "Sum() requires the element
// type to satisfy `T: INumber<T>` (stdlib §5.1 / spec §2.19 / #4730);
// `string` does not conform to `INumber<string>`".
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_linq_name_extension_applicability_7967,
           sum_over_string_elements_answers_the_applicable_extension) {
    JitResult r = runAot(sumOverStringElements());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n");
}

// ---------------------------------------------------------------------------
// R2 — the issue's probe B: `a.Where("x")` with an applicable
// `extension Object { Where(s: string) }`. The §5 `Where` overload's
// parameter is `(T) -> bool`; a `string` argument does not match it, so the
// overload does NOT apply and the user's member — the only APPLICABLE
// candidate — answers.
//
// BASE: the program COMPILES and PRINTS GARBAGE (the §5 operator ran with the
// string where its predicate belongs) and leaks `bz_lazy_where`.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_linq_name_extension_applicability_7967,
           where_with_string_argument_answers_the_applicable_extension) {
    JitResult r = runAot(whereWithStringArgument());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n");
}

// ---------------------------------------------------------------------------
// C1 — CONTROL: the built-in `Sum()` still wins on a NUMERIC Array. The §5
// candidate is applicable there (`int` conforms to `INumber<int>`) and its
// `IEnumerable<int>` head is more specific than the user's `Object` (§26.5.1),
// so the §5 answer (3) is the one that must be printed.
//
// BASE: also 3 — this row must NOT change.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_linq_name_extension_applicability_7967,
           builtin_sum_still_wins_on_a_numeric_array) {
    JitResult r = runAot(builtinSumStillWins());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}

// ---------------------------------------------------------------------------
// C2 — CONTROL: the built-in `Where` / `Select` still win on a LAMBDA
// argument (the §5 overloads ARE applicable there).
//
// BASE: also 1 / 2 — this row must NOT change.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_linq_name_extension_applicability_7967,
           builtin_where_and_select_still_win_on_a_lambda_argument) {
    JitResult r = runAot(builtinWhereSelectStillWin());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n2\n");
}

// ---------------------------------------------------------------------------
// C3 — CONTROL: a user extension with a DIFFERENT name still answers.
//
// BASE: also 4242 — this row must NOT change.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_linq_name_extension_applicability_7967,
           differently_named_extension_still_works) {
    JitResult r = runAot(differentlyNamedExtensionStillWorks());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n");
}
