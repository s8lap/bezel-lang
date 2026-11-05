// fix #8199 (spec §26.5.1 *Applicability*, §14.2 *Interface conformance* rule 1,
// stdlib §5.1, v0.355): the §5 `Min()`/`Max()` block's APPLICABILITY FILTER
// must decide a DECLARATION-LESS BUILTIN CLASS element (`Array<int>`), so that
// a user's applicable `extension Object { Min() }` answers the call.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * stdlib §5.1 (v0.355): "public extension IEnumerable<T> where T:
//     IComparable<T> … public method Min(): T; public method Max(): T;" — the
//     no-selector form is available only where the element type satisfies
//     `T: IComparable<T>`.
//   * spec §26.5.1 (v0.355): "extension members of the called name that match
//     the call's arity and argument types … with block-level `where` clauses
//     satisfied … A `where` clause is an *applicability* filter, never a
//     ranking signal." A candidate whose `where` clause is NOT satisfied is
//     not applicable, so it does not enter the ranking at all.
//   * spec §14.2 *Interface conformance* rule 1 (v0.355): "T is a class that
//     lists I (or any interface extending I) in its inheritance clause, that
//     inherits from a class which conforms to I" — `Array<int>` is a class
//     with NO inheritance clause naming `IComparable`, so it does not
//     conform.
//
// THE DEFECT. The §5 block's applicability filter
// (`stdlibLinqCandidateApplicable` → `linqIComparableProvablyViolated`)
// decided FUNCTION elements, NULLABLE PRIMITIVE elements, `bool` elements,
// user CLASS / INTERFACE elements and user RECORD elements — but NOT a
// declaration-less BUILTIN CLASS element: a Named ref with no `declaration`
// (the shape every builtin generic class carries) fell through to the "keep
// the candidate" answer.
//
// MEASURED on r30-c1: the §5 candidate therefore stayed APPLICABLE, it BEAT
// the user's `extension Object { Min() }` (§26.5 rule 3's "most specific type
// wins" is a rule over the APPLICABLE candidates — the §5 block's
// `IEnumerable<T>` head beats `Object`), and CODEGEN's #2770 guard then
// refused the call:
//
//   Min() without a selector requires the element type to satisfy `T:
//   IComparable<T>` (stdlib §5.1 / spec §14.2 / #2770); `Array` does not —
//   its `CompareTo` could not be resolved for dispatch.
//
// The shape is document-admissible: §5's `Min()` is inapplicable at
// `Array<int>` (the element conforms to nothing), so the user's extension is
// the only applicable candidate and it answers.
//
// ROWS. The issue's own reproducer (the extension answers, 4242), plus four
// CONTROLS: the same program WITHOUT any extension stays refused (the §5.1
// constraint diagnostic — the issue records "Without any extension the
// diagnostic is the correct outcome"); a comparable element keeps the §5
// `Min()` (the extension must NEVER answer it); a built-in VALUE RECORD
// element keeps the §5 `Min()` (stdlib §2.5 declares `IComparable<Self>` on
// `DateTime`/`TimeSpan`/`DateOnly`/`TimeOnly`); and the #8209 INVARIANCE
// rejection stays.
//
// ORACLE. `runAot` / `runAotChecked` (the AOT path). Every rejecting row
// asserts the SPECIFIC diagnostic text, so a program rejected for an unrelated
// reason (or that compiled) cannot satisfy the row. Every accepting row
// asserts the EXACT STDOUT — a VALUE oracle, never "no diagnostic was
// produced".

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The user extension of the issue. §26.5.1: it has no `where` clause, so it
// is applicable wherever the receiver conforms to `Object`.
const char* kObjectMinMax =
    "extension Object\n"
    "  public method Min(): int => 4242;\n"
    "  public method Max(): int => 4343;\n"
    "end;\n";

std::string prog(const std::string& prelude, const std::string& body) {
    return "module M;\n" + prelude +
           "class C\n"
           "  public static method Main(): int\n" +
           body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// THE ISSUE'S REPRODUCER. `Array<Array<int>>.Min()` with the user's
// `extension Object { Min(): int }` in scope: the §5 candidate is NOT
// applicable (the element `Array<int>` conforms to no `IComparable`), so the
// extension answers and prints 4242 / 4343.
//
// MEASURED on r30-c1: REFUSED with the #2770 guard's diagnostic ("`Array`
// does not — its `CompareTo` could not be resolved for dispatch").
BEZEL_TEST(codegen_linq_icomparable_applicability_builtin_class_8199,
           builtin_class_element_answers_the_user_object_extension) {
    JitResult r = runAotChecked(prog(kObjectMinMax,
        "    using var a := new Array<Array<int>>();\n"
        "    Console.WriteLine(a.Min());\n"
        "    Console.WriteLine(a.Max());\n"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n4343\n");
}

// CONTROL — the same program WITHOUT any extension stays refused, with the
// §5.1 / #2770 constraint diagnostic. The issue records this as the correct
// outcome ("Without any extension the diagnostic is the correct outcome"), so
// the fix must not turn the refusal into something else — and it must not
// turn it into a SILENT acceptance either.
BEZEL_TEST(codegen_linq_icomparable_applicability_builtin_class_8199,
           control_no_extension_keeps_the_constraint_refusal) {
    JitResult r = runAot(prog("",
        "    using var a := new Array<Array<int>>();\n"
        "    Console.WriteLine(a.Min());\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "its `CompareTo` could not be resolved for dispatch"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// CONTROL — a COMPARABLE element keeps the §5 `Min()`: the extension must
// never answer it (§26.5.1 — the §5 candidate IS applicable there, so it
// stays in the ranking and its `IEnumerable<T>` head beats `Object`).
// 3,1,9 -> Min 1, Max 9.
BEZEL_TEST(codegen_linq_icomparable_applicability_builtin_class_8199,
           control_comparable_element_keeps_the_builtin_min_max) {
    JitResult r = runAotChecked(prog(kObjectMinMax,
        "    using var a := new Array<int>();\n"
        "    a.Add(3);\n"
        "    a.Add(1);\n"
        "    a.Add(9);\n"
        "    Console.WriteLine(a.Min());\n"
        "    Console.WriteLine(a.Max());\n"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n9\n");
}

// CONTROL — a BUILT-IN VALUE RECORD element keeps the §5 `Min()` too:
// stdlib §2.5 declares `IComparable<Self>` on `DateTime`/`TimeSpan`/
// `DateOnly`/`TimeOnly`, so the §5 candidate IS applicable there. The
// extension must not answer it.
BEZEL_TEST(codegen_linq_icomparable_applicability_builtin_class_8199,
           control_builtin_value_record_element_keeps_the_builtin_min_max) {
    JitResult r = runAotChecked(prog(kObjectMinMax,
        "    using var a := new Array<DateTime>();\n"
        "    a.Add(DateTime.FromTicks(300));\n"
        "    a.Add(DateTime.FromTicks(100));\n"
        "    Console.WriteLine(a.Min().Ticks);\n"
        "    Console.WriteLine(a.Max().Ticks);\n"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n300\n");
}

// CONTROL — the #8209 INVARIANCE rejection stays. `class K: IComparable<int>`
// names `IComparable<int>`, NOT `IComparable<K>`, and spec §12.9 makes
// interface type arguments INVARIANT — so `K` does not satisfy `where T:
// IComparable<T>`. The row is the #8209 file's own ROW 1 shape.
BEZEL_TEST(codegen_linq_icomparable_applicability_builtin_class_8199,
           control_invariant_generic_argument_stays_rejected_8209) {
    JitResult r = runAot(prog(
        "public class K: IComparable<int>\n"
        "  public X: int;\n"
        "  public constructor(x: int)\n"
        "    X := x;\n"
        "  end;\n"
        "  public method CompareTo(other: int): int => X - other;\n"
        "end K;\n",
        "    using var a := new Array<K>();\n"
        "    var m := a.Min();\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "does not implement IComparable<T>"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
