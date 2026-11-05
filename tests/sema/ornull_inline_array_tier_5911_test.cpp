// fix #5911 — spec §15.1 (v0.355), the conditionally-admitted-member register
// for a standard-library member whose SIGNATURE forms `T?` at a type
// parameter of its receiver type — the `*OrNull` tier.
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
// and from docs/Bezel_Standard_Library.md §1 (v0.355):
//
//   "Neither a shape-(a) member nor an `*OrNull` member is admissible where
//    the type argument its `T?` is formed at — the element type, or a
//    dictionary's value type — is an inline array: an inline array has no
//    nullable form (language spec §15.1), so each is a conditionally-admitted
//    member there (language spec §14.2; the *Conditional members* row
//    above), a compile error at the call, or at the formation of a method
//    reference over the member (language spec §27.6) — `Channel<[3]int>
//    .TryReceive` and `Array<[3]int>.FirstOrNull()` among them — and the
//    throwing form, the non-nullable member or a count or index test is the
//    access."
//
// The register is scoped BY POSITION. `Dictionary<K,V>.GetOrNull(key: K): V?`
// forms its `T?` at V. The KEY type participates in no `T?` the signature
// forms (`key: K` is K, not K?) — so `Dictionary<[3]int, string>` is NOT over
// the register and the call is ADMITTED. The same holds for the element
// accessors: the `T?` is formed at the ELEMENT type, so an element type that
// WRAPS the inline array in a record is not over the register either.
//
// WHAT WAS WRONG (#5911): the `Try*(out item: T?)` family was gated (#5641);
// the `*OrNull` tier was NOT. `Dictionary<[3]int,[3]int>.GetOrNull(k)` got
// no diagnostic, sema formed the result type `[3]int?`, and codegen then
// failed ("Trunc only produces integer" / "ZExt only operates on integer",
// "codegen error: IR verification failed") — or, with the result unwrapped,
// SEGFAULTED the compiler (rc=139).
//
// MEASURED at the base commit (0c6cc082), `build/bezelc`:
//
//   Dictionary<string,[3]int>.GetOrNull("a")     rc=1  "Trunc only produces integer"
//                                                    (and rc=139 with `v![0]`)
//   SortedDictionary<string,[3]int>.GetOrNull("a") rc=139 (compiler SIGSEGV)
//   Array<[3]int>.MinOrNull()                    rc=1  #2783 "not yet implemented
//                                                          for this element type"
//   Array<[3]int>.Where(...).FirstOrNull()       rc=0  ADMITTED (no diagnostic)
//   IList<[3]int>.FirstOrNull()                 rc=0  ADMITTED (no diagnostic)
//
// The rows below pin the gate at EVERY position the `*OrNull` tier forms its
// `T?` at, and pin the three ADMITTED shapes the register leaves alone.
//
// `ElementAtOrNull` (stdlib §5) has ONE row (R13, the `IEnumerable<T>`
// receiver): on a CONCRETE collection the compiler does not declare it at
// all (`Array<T>.ElementAtOrNull` is "no member `ElementAtOrNull` on
// `Array<T>`"), so there is no signature to be over the register there.

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

// An ADMITTED control: the program compiles, runs clean under the strict AOT
// oracle, and prints exactly `out`.
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

}  // namespace

// ===========================================================================
// MUST-REJECT ROWS — the `*OrNull` tier over an inline-array type argument.
// ===========================================================================

// R1 — `Dictionary<K,V>.GetOrNull` forms its `T?` at V (the VALUE type).
// This is the issue's own repro family: at the base commit the program
// compiled to a malformed-IR failure ("Trunc only produces integer"), and
// SEGFAULTED the compiler when the result was unwrapped.
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           dict_value_inline_array_get_or_null_rejected) {
    auto r = runAot(prog(
        "  using var d := new Dictionary<string, [3]int>();\n"
        "  var a: [3]int := [1, 2, 3];\n"
        "  d.Add(\"a\", a);\n"
        "  var v := d.GetOrNull(\"a\");\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "GetOrNull"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `Dictionary` whose value type is an inline array"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNotAdmitted));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNullableInlineArray));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSpecCite));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kDiagnosticTag));
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, kNullableInlineArray), 1);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
}

// R2 — `SortedDictionary<K,V>.GetOrNull` (stdlib §4.17) forms its `T?` at V
// as well. At the base commit this SEGFAULTED the compiler (rc=139).
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           sorted_dict_value_inline_array_get_or_null_rejected) {
    auto r = runAot(prog(
        "  using var d := new SortedDictionary<string, [3]int>();\n"
        "  var a: [3]int := [1, 2, 3];\n"
        "  d.Add(\"a\", a);\n"
        "  var v := d.GetOrNull(\"a\");\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "GetOrNull"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `SortedDictionary` whose value type is an inline array"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNotAdmitted));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNullableInlineArray));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
}

// R3 — `Array<T>.FirstOrNull()` forms its `T?` at the ELEMENT type.
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           array_first_or_null_inline_rejected) {
    expectOrNullRejected(
        prog("  using var xs := new Array<[3]int>();\n"
             "  var a: [3]int := [1, 2, 3];\n"
             "  xs.Add(a);\n"
             "  var v := xs.FirstOrNull();\n"),
        "FirstOrNull", "an `Array` of an inline array");
}

// R4 — `Array<T>.LastOrNull()`.
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           array_last_or_null_inline_rejected) {
    expectOrNullRejected(
        prog("  using var xs := new Array<[3]int>();\n"
             "  var a: [3]int := [1, 2, 3];\n"
             "  xs.Add(a);\n"
             "  var v := xs.LastOrNull();\n"),
        "LastOrNull", "an `Array` of an inline array");
}

// R5 — `Array<T>.SingleOrNull()`.
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           array_single_or_null_inline_rejected) {
    expectOrNullRejected(
        prog("  using var xs := new Array<[3]int>();\n"
             "  var a: [3]int := [1, 2, 3];\n"
             "  xs.Add(a);\n"
             "  var v := xs.SingleOrNull();\n"),
        "SingleOrNull", "an `Array` of an inline array");
}

// R6 — the PREDICATE form `FirstOrNull(predicate)` (stdlib §5) forms the
// same `T?` at the element type.
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           array_first_or_null_predicate_inline_rejected) {
    expectOrNullRejected(
        prog("  using var xs := new Array<[3]int>();\n"
             "  var a: [3]int := [1, 2, 3];\n"
             "  xs.Add(a);\n"
             "  var v := xs.FirstOrNull(lambda(x) => x[0] > 0);\n"),
        "FirstOrNull", "an `Array` of an inline array");
}

// R7 — `Array<T>.MinOrNull()` (stdlib §5.1) forms its `T?` at the element
// type. At the base commit this was refused by CODEGEN with #2783's "not
// yet implemented for this element type" — a §5.1-shaped message for a §15.1
// violation, and one that let the malformed nullable inline array be formed
// first.
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           array_min_or_null_inline_rejected) {
    expectOrNullRejected(
        prog("  using var xs := new Array<[3]int>();\n"
             "  var a: [3]int := [1, 2, 3];\n"
             "  xs.Add(a);\n"
             "  var v := xs.MinOrNull();\n"),
        "MinOrNull", "an `Array` of an inline array");
}

// R8 — `Array<T>.MaxOrNull()`.
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           array_max_or_null_inline_rejected) {
    expectOrNullRejected(
        prog("  using var xs := new Array<[3]int>();\n"
             "  var a: [3]int := [1, 2, 3];\n"
             "  xs.Add(a);\n"
             "  var v := xs.MaxOrNull();\n"),
        "MaxOrNull", "an `Array` of an inline array");
}

// R9 — the LAZY terminal: `arr.Where(...).FirstOrNull()` forms its `T?` at
// the lazy sequence's element type. At the base commit this compiled clean
// (rc=0) with no diagnostic at all.
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           lazy_where_first_or_null_inline_rejected) {
    expectOrNullRejected(
        prog("  using var xs := new Array<[3]int>();\n"
             "  var a: [3]int := [1, 2, 3];\n"
             "  xs.Add(a);\n"
             "  var q := xs.Where(lambda(x) => x[0] > 0);\n"
             "  var v := q.FirstOrNull();\n"),
        "FirstOrNull",
        "a lazy sequence of an inline array element type");
}

// R10 — the LAZY terminal `MinOrNull()` over an inline-array element type.
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           lazy_where_min_or_null_inline_rejected) {
    expectOrNullRejected(
        prog("  using var xs := new Array<[3]int>();\n"
             "  var a: [3]int := [1, 2, 3];\n"
             "  xs.Add(a);\n"
             "  var q := xs.Where(lambda(x) => x[0] > 0);\n"
             "  var v := q.MinOrNull();\n"),
        "MinOrNull",
        "a lazy sequence of an inline array element type");
}

// R11 — the explicitly-spelled `IEnumerable<T>` receiver (stdlib §4.12).
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           ienumerable_first_or_null_inline_rejected) {
    expectOrNullRejected(
        prog("  using var xs := new Array<[3]int>();\n"
             "  var a: [3]int := [1, 2, 3];\n"
             "  xs.Add(a);\n"
             "  var e: IEnumerable<[3]int> := xs;\n"
             "  var v := e.FirstOrNull();\n"),
        "FirstOrNull", "an `IEnumerable` of an inline array");
}

// R12 — `IEnumerable<T>.MinOrNull()` (stdlib §5.1) forms its `T?` at the
// element type too. At the base commit sema never formed that `T?` at all,
// and CODEGEN refused the call with #2783's "not yet implemented for this
// element type" — a §5.1-shaped message for a §15.1 violation.
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           ienumerable_min_or_null_inline_rejected) {
    expectOrNullRejected(
        prog("  using var xs := new Array<[3]int>();\n"
             "  var a: [3]int := [1, 2, 3];\n"
             "  xs.Add(a);\n"
             "  var e: IEnumerable<[3]int> := xs;\n"
             "  var v := e.MinOrNull();\n"),
        "MinOrNull", "an `IEnumerable` of an inline array");
}

// R13 — `IEnumerable<T>.ElementAtOrNull(index)` (stdlib §5) forms its `T?`
// at the element type as well. At the base commit sema never formed that
// `T?`, and CODEGEN refused the call with #2457's "produced no value, and no
// dispatch arm emitted any code for this call".
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           ienumerable_element_at_or_null_inline_rejected) {
    expectOrNullRejected(
        prog("  using var xs := new Array<[3]int>();\n"
             "  var a: [3]int := [1, 2, 3];\n"
             "  xs.Add(a);\n"
             "  var e: IEnumerable<[3]int> := xs;\n"
             "  var v := e.ElementAtOrNull(0);\n"),
        "ElementAtOrNull", "an `IEnumerable` of an inline array");
}

// R14 — the LIST INTERFACE: `IList<T>` inherits `IEnumerable<T>` (stdlib
// §4.1), so §5's `FirstOrNull` is a member of its surface and forms `T?` at
// the element type. At the base commit this compiled clean (rc=0) with no
// diagnostic, and SEGFAULTED the compiler (rc=139) when the result was used.
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           ilist_first_or_null_inline_rejected) {
    expectOrNullRejected(
        prog("  using var xs := new Array<[3]int>();\n"
             "  var a: [3]int := [1, 2, 3];\n"
             "  xs.Add(a);\n"
             "  var e: IList<[3]int> := xs;\n"
             "  var v := e.FirstOrNull();\n"),
        "FirstOrNull", "an `IList` of an inline array");
}

// R15 — the READ-ONLY list interface (`IReadOnlyList<T>`, stdlib §4.1).
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           ireadonlylist_first_or_null_inline_rejected) {
    expectOrNullRejected(
        prog("  using var xs := new Array<[3]int>();\n"
             "  var a: [3]int := [1, 2, 3];\n"
             "  xs.Add(a);\n"
             "  var e: IReadOnlyList<[3]int> := xs;\n"
             "  var v := e.FirstOrNull();\n"),
        "FirstOrNull", "an `IReadOnlyList` of an inline array");
}

// ===========================================================================
// ADMITTED CONTROLS — the shapes §15.1's register leaves alone.
// ===========================================================================

// C1 — THE CONTROL THE ISSUE NAMES: an inline-array KEY with a non-array
// VALUE. §15.1's register is positional — `GetOrNull(key: K): V?` forms its
// `T?` at V, and the KEY type participates in no `T?` the signature forms
// (`key: K` is K, not K?). So this call is ADMITTED, and it RUNS: the value
// comes back as `string?`.
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           dict_inline_array_key_string_value_get_or_null_admitted) {
    expectStdout(
        prog("  using var d := new Dictionary<[3]int, string>();\n"
             "  var k: [3]int := [1, 2, 3];\n"
             "  d.Add(k, \"three\");\n"
             "  var v := d.GetOrNull(k);\n"
             "  if v <> null then\n"
             "    Console.WriteLine(v!);\n"
             "  end if;\n"),
        "three\n");
}

// C2 — an ordinary dictionary (no inline array anywhere).
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           dict_string_key_int_value_get_or_null_admitted) {
    expectStdout(
        prog("  using var d := new Dictionary<string, int>();\n"
             "  d.Add(\"a\", 7);\n"
             "  var v := d.GetOrNull(\"a\");\n"
             "  if v <> null then\n"
             "    Console.WriteLine(v!);\n"
             "  end if;\n"),
        "7\n");
}

// C3 — the same for the sorted dictionary.
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           sorted_dict_string_key_int_value_get_or_null_admitted) {
    expectStdout(
        prog("  using var d := new SortedDictionary<string, int>();\n"
             "  d.Add(\"a\", 7);\n"
             "  var v := d.GetOrNull(\"a\");\n"
             "  if v <> null then\n"
             "    Console.WriteLine(v!);\n"
             "  end if;\n"),
        "7\n");
}

// C4 — the element accessors over an ORDINARY element type.
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           array_int_first_or_null_admitted) {
    expectStdout(
        prog("  using var xs := new Array<int>();\n"
             "  xs.Add(1);\n"
             "  xs.Add(2);\n"
             "  var v := xs.FirstOrNull();\n"
             "  if v <> null then\n"
             "    Console.WriteLine(v!);\n"
             "  end if;\n"),
        "1\n");
}

// C5 — THE RECORD CONTROL: an element type that WRAPS the inline array in a
// record is the §15.1 register's own escape hatch ("an element type that
// wraps the array in a record"). The `T?` is formed at the element type
// `R`, which is not an inline array — so the call is ADMITTED.
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           array_record_element_first_or_null_admitted) {
    expectStdout(
        progWithRecord("  using var xs := new Array<R>();\n"
                       "  var a: [3]int := [1, 2, 3];\n"
                       "  xs.Add(new R(a));\n"
                       "  var v := xs.FirstOrNull();\n"
                       "  if v <> null then\n"
                       "    Console.WriteLine(v!.arr[0]);\n"
                       "  end if;\n"),
        "1\n");
}

// C6 — the same record-wrap at a dictionary's VALUE position.
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           dict_record_value_get_or_null_admitted) {
    expectStdout(
        progWithRecord("  using var d := new Dictionary<string, R>();\n"
                       "  var a: [3]int := [1, 2, 3];\n"
                       "  d.Add(\"k\", new R(a));\n"
                       "  var v := d.GetOrNull(\"k\");\n"
                       "  if v <> null then\n"
                       "    Console.WriteLine(v!.arr[0]);\n"
                       "  end if;\n"),
        "1\n");
}

// C7 — the lazy terminal over an ordinary element type.
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           lazy_where_int_first_or_null_admitted) {
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

// C8 — the explicitly-spelled `IEnumerable<T>` receiver over an ordinary
// element type.
BEZEL_TEST(sema_ornull_inline_array_tier_5911,
           ienumerable_int_first_or_null_admitted) {
    expectStdout(
        prog("  using var xs := new Array<int>();\n"
             "  xs.Add(1);\n"
             "  xs.Add(2);\n"
             "  var e: IEnumerable<int> := xs;\n"
             "  var v := e.FirstOrNull();\n"
             "  if v <> null then\n"
             "    Console.WriteLine(v!);\n"
             "  end if;\n"),
        "1\n");
}
