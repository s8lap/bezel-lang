// Spec-derived regression test for fix #8816 — `SortedSet<string>.First` /
// `.Last` are ADMITTED (stdlib §4.18 / §9.6, spec §14.2, v0.359).
//
// THE DEFECT (issue #8816, MEASURED on staging b6088eb54 and on this lane's
// base r30-c1 @ e8b35d104). The issue's own repro —
//
//     using var s := new SortedSet<string>();
//     s.Add("b"); s.Add("a");
//     Console.WriteLine(s.First);
//     Console.WriteLine(s.Last);
//
// — was REJECTED at compile time with
//
//     `.First` on `SortedSet<T>` (stdlib §4.18) requires the element type to
//     implement `IComparable<T>`; the element type does not (it declares no
//     dispatchable single-parameter `CompareTo`, and being auto-hashable is
//     not sufficient — hashable and orderable are different properties).
//
// WHY THAT REJECTION IS FALSE. stdlib §4.18 (v0.359) declares
//
//     public class SortedSet<T>: IReadOnlyCollection<T> where T: IComparable<T>
//       public pure readonly property First: T;  // least; InvalidOperationException if empty
//       public pure readonly property Last: T;   // greatest; InvalidOperationException if empty
//     end SortedSet;
//
// and stdlib §9.6 declares
//
//     public class String: IEnumerable<char>, IComparable<string>, IEquatable<string>
//       public method CompareTo(other: string): int;  // satisfies IComparable<string>
//
// so `string` DOES implement `IComparable<string>` and `SortedSet<string>` is
// a legal instantiation of §4.18's sole constraint.
//
// THE ROOT CAUSE (codegen, not sema). The sorted containers' ONE ordering
// authority — `Emitter::buildSortedKeyComparator`
// (src/codegen/LlvmBackend_Calls3.cpp) — had no `string` arm: a `string`
// lowers to a POINTER, so it is neither an integer nor a float; it is not a
// class/interface (`hasObjectHeaderEquality` reads `PrimitiveType::Object`
// only); and it is not a record. So it fell through to the function's
// `else return nullptr`, and EVERY consumer of that comparator (`SortedSet`'s
// `.First`/`.Last` in BOTH spellings — the property form in
// LlvmBackend_Exprs.cpp and the call form in LlvmBackend_Calls.cpp) reported
// the "does not implement `IComparable<T>`" diagnostic about a type the
// stdlib declares `IComparable<string>`. Fix #8816 adds the `string` arm:
// the §9.6 ordinal CODE-POINT relation (`bz_str_compare_cp`), the same
// runtime call `string.CompareTo` dispatches to and the same one the §4.18
// ascending snapshot ranks by — so the order of a `SortedSet<string>` and the
// order of `Array<string>.Sort()` cannot drift apart.
//
// The rows below assert the PRINTED ORDER on stdout, never a derived
// checksum, so a row that happened to produce the right total while
// permuted would still fail. Row 2's triple is the #4487/#4495 one: three
// strings whose first two storage BYTES are equal, so a byte-wise (or
// truncated) comparison would answer "equal" and yield the INSERTION order
// instead of the code-point order §9.6 defines.
//
// The BOUNDARY (a `SortedSet<T>` element type that does NOT implement
// `IComparable<T>` must STAY rejected) is NOT a row here: it is green on the
// base as well as on the fix, so it belongs to the conformance corpus, not to
// this suite. It is banked as a corpus candidate (the producer's
// `corpus-candidates/` directory) with the sentence that decides it; the
// suite's own existing rows (tests/codegen/sorted_set_class_element_3428_
// test.cpp: `class_without_compareto_still_diagnosed`,
// `object_element_still_diagnosed`) already pin the same boundary on the
// construction surface.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

// The issue's repro shape, verbatim: a `Program` whose `Main` is the body.
std::string prog(const std::string& body) {
    return "module P; "
           "public class Program "
           "  public static method Main() "
           + body +
           "  end Main; "
           "end Program;";
}

}  // namespace

// stdlib §4.18 (v0.359): `First` is the LEAST element and `Last` the
// GREATEST. The issue's own repro, in the PROPERTY spelling the stdlib
// declares (`public pure readonly property First: T;`).
//
// Pre-fix (MEASURED on the base r30-c1 @ e8b35d104): the program is
// REJECTED — `runAotOut` returns the "<did-not-run>" sentinel — with
// "`.First` on `SortedSet<T>` (stdlib §4.18) requires the element type to
// implement `IComparable<T>`; the element type does not (…)".
BEZEL_TEST(sortedset_string_first_last_8816,
           string_element_first_last_admitted_spec_4_18) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var s := new SortedSet<string>(); "
        "    s.Add(\"b\"); "
        "    s.Add(\"a\"); "
        "    Console.WriteLine(s.First); "
        "    Console.WriteLine(s.Last); ")),
        "a\nb");
}

// stdlib §9.6 (v0.359): `string.CompareTo` is the ORDINAL CODE-POINT
// relation, so `First` is the code-point-least element and `Last` the
// code-point-greatest.
//
// The triple is the #4487/#4495 one — "日本", "日x", "日a": all three share
// the storage bytes `E5 65`, so a comparator that walked BYTES over a
// CODE-POINT length (the #535 defect class) would call all three EQUAL and
// answer the INSERTION order "日本", "日a" instead of the §9.6 order
// "日a", "日x", "日本". The expected values are the code-point order, so a
// fix that merely admitted the element without ranking it by §9.6 would fail
// this row.
//
// Pre-fix (MEASURED on the base r30-c1 @ e8b35d104): REJECTED, the same
// "does not implement `IComparable<T>`" diagnostic.
BEZEL_TEST(sortedset_string_first_last_8816,
           string_element_first_last_code_point_order_spec_9_6) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var s := new SortedSet<string>(); "
        "    s.Add(\"日本\"); "
        "    s.Add(\"日x\"); "
        "    s.Add(\"日a\"); "
        "    Console.WriteLine(s.First); "
        "    Console.WriteLine(s.Last); ")),
        "日a\n日本");
}
