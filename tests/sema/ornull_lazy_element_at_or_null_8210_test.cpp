// fix #8210 — spec §15.1 (v0.355), the conditionally-admitted-member register
// for a standard-library member whose SIGNATURE forms `T?` at a type
// parameter of its receiver type — the `*OrNull` tier — at the LAZY
// receiver.
//
// THE RULE, quoted from docs/Bezel_Language_Spec.md §15.1 (v0.355):
//
//   "a standard-library member whose **signature** forms `T?` at a type
//    parameter of its receiver type — the element type, or a dictionary's
//    value type — as a result, `out` slot or property, the `*OrNull` tier
//    (`FirstOrNull`) and the `Try*(out item: T?)` family (`TryPop`,
//    `TryReceive`) among them, is a conditionally-admitted member where that
//    type argument is an inline array (§14.2): a compile error at the use —
//    the call, the property read, or the formation of a method or property
//    reference over it (§27.6) — naming the member and the remedies, a count
//    or index test, the non-nullable member, or an element type that wraps
//    the array in a record."
//
// and from docs/Bezel_Standard_Library.md §1 (v0.355), the *Conditional
// members* row:
//
//   "… the `*OrNull` tier over `IEnumerable<T>` (`FirstOrNull`, `LastOrNull`,
//    `SingleOrNull`, `ElementAtOrNull`) …"
//
// §5 (v0.355) declares the fourth of those four on `IEnumerable<T>`:
// `public method ElementAtOrNull(index: int): T?;` — so it forms its `T?` at
// the ELEMENT type, exactly like its three siblings.
//
// WHAT WAS WRONG (#8210): #5911 gated the `*OrNull` tier at every receiver
// family EXCEPT the lazy one. A lazy sequence — the `IEnumerable<T>`-shaped
// sentinel `arr.Where(...)` produces, and equally `Range<T>`, `IGrouping<TKey,
// T>` and `IOrderedEnumerable<T>` — is an `IEnumerable<T>` receiver, so §5's
// `ElementAtOrNull` is a member of its surface and forms `T?` at the element
// type. #5911's gate covered `FirstOrNull`/`LastOrNull`/`SingleOrNull`/
// `MinOrNull`/`MaxOrNull` on that receiver but NOT `ElementAtOrNull`, so
// `arr.Where(...).ElementAtOrNull(0)` over an `Array<[3]int>` compiled CLEAN
// (rc=0) with only codegen's degraded-lowering warnings, and the malformed
// nullable inline array was never formed and never rejected.
//
// MEASURED at the base commit (r30-c1, 9fef8146), `build/bezelc`:
//
//   Array<[3]int>.FirstOrNull()                  rc=1  §15.1 rejection
//   Array<[3]int>.Where(...).FirstOrNull()       rc=1  §15.1 rejection
//   IEnumerable<[3]int>.ElementAtOrNull(0)        rc=1  §15.1 rejection
//   Array<[3]int>.Where(...).ElementAtOrNull(0)  rc=0  ADMITTED (the residual)
//
// The rows below pin the gate at the lazy receiver, and pin the shapes the
// §15.1 register leaves ALONE — the admitting twin (an ordinary element type),
// the record-wrap escape hatch, and the three sibling spellings the #5911
// commit already gated (which must keep rejecting).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::diagnosticMessageCount;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

// The §15.1 `*OrNull` diagnostic's stable fragments. Matched on fragments,
// not on a whole-message literal, so a cosmetic reword does not silently
// unpin a row — but the member name and the receiver clause are pinned
// separately, so a message that lost EITHER still fails the row.
constexpr const char* kNotAdmitted = "is not admitted on";
constexpr const char* kNullableInlineArray = "nullable inline array";
constexpr const char* kSpecCite = "§15.1";
constexpr const char* kDiagnosticTag = "diagnostic: `inline array`";

// The receiver clause the lazy block's §15.1 gate reports — the same one
// #5911's lazy `FirstOrNull` row pins.
constexpr const char* kLazyClause = "a lazy sequence of an inline array element type";

// A MUST-REJECT row: the program must be rejected by the FRONT END, at a
// real source location, with the §15.1 `*OrNull` diagnostic naming the
// member and the receiver clause — and with exactly ONE such diagnostic (a
// second copy would mean two gates fired on one call).
void expectOrNullRejected(const std::string& program, const char* member,
                          const char* clause) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, member));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, clause));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNotAdmitted));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNullableInlineArray));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSpecCite));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kDiagnosticTag));
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, kNullableInlineArray), 1);
    // The rejection is a COMPILE-TIME rejection at a real location — not a
    // compiler crash, a runtime signal, a timeout, or an ASan report.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "terminated by signal"));
}

// An ADMITTED control: the program compiles and runs. The claim is that the
// §15.1 gate does NOT fire — a rejected program does not run, so `r.ran` IS
// the assertion. (The §15.1 register says nothing about a member over an
// ORDINARY element type; whether the compiler implements that member is the
// separate §5 question, which this file does not pin.)
void expectAdmitted(const std::string& program) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNotAdmitted));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNullableInlineArray));
}

// An ADMITTED control that RUNS and prints exactly `out` (the strict AOT
// oracle).
void expectStdout(const std::string& program, const std::string& out) {
    JitResult r = runAotChecked(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

std::string prog(const std::string& body) {
    return "module M;\n"
           "class C public static method Main(): int\n" + body +
           "  return 0;\n"
           "end Main;\n"
           "end C;\n";
}

// A program whose prelude declares a record (the "element type that wraps the
// array in a record" remedy — the §15.1 register's own escape hatch).
std::string progWithRecord(const std::string& body) {
    return "module M;\n"
           "record R(arr: [3]int);\n"
           "class C public static method Main(): int\n" + body +
           "  return 0;\n"
           "end Main;\n"
           "end C;\n";
}

// THE ISSUE'S OWN REPROGRAM, verbatim (issue #8210's `u08.bzl`). The generic
// methods `G`/`H` are part of it: their bodies call `xs.FirstOrNull()` and
// `xs.MinOrNull()` over a `T`-typed receiver, and §15.1 decides those at the
// INSTANTIATION (§3.6 rule 2 / §14.2), so the declaration itself is ADMITTED
// — the issue's program must fail on the LAZY call alone, not on the generic
// declarations beside it.
constexpr const char* kIssueRepro =
    "module M;\n"
    "record R(arr: [3]int);\n"
    "enum E A, B; end E;\n"
    "class K public x: int; end K;\n"
    "public class C\n"
    "  static method G<T>(xs: Array<T>): T? return xs.FirstOrNull(); end G;\n"
    "  static method H<T>(xs: Array<T>): T? where T: IComparable<T> return "
    "xs.MinOrNull(); end H;\n"
    " public static method Main(): int\n"
    "using var xs := new Array<[3]int>(); var f := "
    "xs.Where(lambda(x)=>true).ElementAtOrNull(0);\n"
    "  return 0;\n"
    "end Main; end C;\n";

}  // namespace

// ===========================================================================
// MUST-REJECT ROWS — the `*OrNull` tier at the LAZY receiver.
// ===========================================================================

// R1 — THE BUG: the issue's own repro, verbatim. `arr.Where(...).ElementAtOrNull(0)`
// over an `Array<[3]int>` forms `[3]int?` at the ELEMENT type, so §15.1 makes
// the member conditionally-admitted there and the use is a compile error.
// At the base commit this compiled clean (rc=0) with only codegen's
// degraded-lowering warnings.
BEZEL_TEST(sema_ornull_lazy_element_at_or_null_8210,
           issue_repro_lazy_element_at_or_null_inline_rejected) {
    expectOrNullRejected(kIssueRepro, "ElementAtOrNull", kLazyClause);
}

// R2 — the same shape without the issue's surrounding declarations, so the
// row cannot depend on them.
BEZEL_TEST(sema_ornull_lazy_element_at_or_null_8210,
           lazy_where_element_at_or_null_inline_rejected) {
    expectOrNullRejected(
        prog("  using var xs := new Array<[3]int>();\n"
             "  var a: [3]int := [1, 2, 3];\n"
             "  xs.Add(a);\n"
             "  var q := xs.Where(lambda(x) => x[0] > 0);\n"
             "  var v := q.ElementAtOrNull(0);\n"),
        "ElementAtOrNull", kLazyClause);
}

// R3 — the LAZY chain's OTHER shape: the §5 operator that produces the lazy
// sequence need not be `Where` — `Select` produces the same sentinel.
BEZEL_TEST(sema_ornull_lazy_element_at_or_null_8210,
           lazy_select_element_at_or_null_inline_rejected) {
    expectOrNullRejected(
        prog("  using var xs := new Array<[3]int>();\n"
             "  var a: [3]int := [1, 2, 3];\n"
             "  xs.Add(a);\n"
             "  var q := xs.Select(lambda(x) => x);\n"
             "  var v := q.ElementAtOrNull(0);\n"),
        "ElementAtOrNull", kLazyClause);
}

// R4 — the explicitly-spelled `IEnumerable<T>` receiver (stdlib §4.12) is
// #5911's own R13 row: the SAME member, the same element position. It must
// keep rejecting — the two spellings of one member may not disagree.
BEZEL_TEST(sema_ornull_lazy_element_at_or_null_8210,
           ienumerable_element_at_or_null_inline_rejected) {
    expectOrNullRejected(
        prog("  using var xs := new Array<[3]int>();\n"
             "  var a: [3]int := [1, 2, 3];\n"
             "  xs.Add(a);\n"
             "  var e: IEnumerable<[3]int> := xs;\n"
             "  var v := e.ElementAtOrNull(0);\n"),
        "ElementAtOrNull", "an `IEnumerable` of an inline array");
}

// R5 — the LAZY `FirstOrNull` (#5911's lazy row): the sibling that was
// already gated must keep rejecting.
BEZEL_TEST(sema_ornull_lazy_element_at_or_null_8210,
           lazy_where_first_or_null_inline_rejected) {
    expectOrNullRejected(
        prog("  using var xs := new Array<[3]int>();\n"
             "  var a: [3]int := [1, 2, 3];\n"
             "  xs.Add(a);\n"
             "  var q := xs.Where(lambda(x) => x[0] > 0);\n"
             "  var v := q.FirstOrNull();\n"),
        "FirstOrNull", kLazyClause);
}

// R6 — the MATERIALIZED spelling: `Where(...).ToArray()` yields an
// `Array<[3]int]`, and `FirstOrNull()` on THAT is #5911's `Array` row. The
// lazy chain's materializing twin must keep rejecting.
BEZEL_TEST(sema_ornull_lazy_element_at_or_null_8210,
           lazy_to_array_first_or_null_inline_rejected) {
    expectOrNullRejected(
        prog("  using var xs := new Array<[3]int>();\n"
             "  var a: [3]int := [1, 2, 3];\n"
             "  xs.Add(a);\n"
             "  var q := xs.Where(lambda(x) => x[0] > 0);\n"
             "  var v := q.ToArray().FirstOrNull();\n"),
        "FirstOrNull", "an `Array` of an inline array");
}

// ===========================================================================
// ADMITTED CONTROLS — the shapes §15.1's register leaves alone.
// ===========================================================================

// C1 — THE ADMITTING TWIN: the SAME shape as R2 with an ORDINARY element
// type. The §15.1 register is POSITIONAL — the `T?` is formed at the
// ELEMENT type, and an `int` element is not an inline array — so the call is
// ADMITTED. This row is green on the BASE arm and on the fixed arm: it shows
// the gate distinguishes the element TYPE, not the shape of the chain.
BEZEL_TEST(sema_ornull_lazy_element_at_or_null_8210,
           lazy_where_element_at_or_null_ordinary_element_admitted) {
    expectAdmitted(
        prog("  using var xs := new Array<int>();\n"
             "  xs.Add(1);\n"
             "  var q := xs.Where(lambda(x) => x > 0);\n"
             "  var v := q.ElementAtOrNull(0);\n"));
}

// C2 — THE RECORD CONTROL: an element type that WRAPS the inline array in a
// record is the §15.1 register's own escape hatch ("an element type that
// wraps the array in a record"). The `T?` is formed at the element type `R`,
// which is not an inline array — so the call is ADMITTED.
BEZEL_TEST(sema_ornull_lazy_element_at_or_null_8210,
           lazy_where_element_at_or_null_record_element_admitted) {
    expectAdmitted(
        progWithRecord("  using var xs := new Array<R>();\n"
                       "  var a: [3]int := [1, 2, 3];\n"
                       "  xs.Add(new R(a));\n"
                       "  var q := xs.Where(lambda(x) => true);\n"
                       "  var v := q.ElementAtOrNull(0);\n"));
}

// C3 — the lazy `FirstOrNull` over an ORDINARY element type (#5911's own
// C7 control): it must run and print the element. This is the row that
// reads DATA, so a gate that misreads its argument cannot pass the file.
BEZEL_TEST(sema_ornull_lazy_element_at_or_null_8210,
           lazy_where_first_or_null_ordinary_element_runs) {
    expectStdout(
        prog("  using var xs := new Array<int>();\n"
             "  xs.Add(1);\n"
             "  xs.Add(2);\n"
             "  var q := xs.Where(lambda(x) => x > 0);\n"
             "  var v := q.FirstOrNull();\n"
             "  if v <> null then\n"
             "    Console.WriteLine(v!);\n"
             "  end if;\n"),
        "1\n");
}
