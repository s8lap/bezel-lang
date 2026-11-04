// Regression tests for issue #8049: the no-selector `Array<DateTime>.Min()` /
// `.Max()` (and its `TimeSpan` / `DateOnly` / `TimeOnly` siblings) was
// REFUSED with
//
//   "Min() without a selector requires the element type to satisfy
//    `T: IComparable<T>` (stdlib §5.1 / spec §14.2 / #2770); `DateTime`
//    does not — its `CompareTo` could not be resolved for dispatch."
//
// WHAT THE DOCUMENTS REQUIRE (verified at v0.353 in this tree).
//
//   * stdlib §2.5 "`DateTime`, `TimeSpan`, `DateOnly`, `TimeOnly`" declares
//         public record DateTime: IComparable<DateTime>, IEquatable<DateTime>
//         public record TimeSpan: IComparable<TimeSpan>, IEquatable<TimeSpan>
//         public record DateOnly: IComparable<DateOnly>, IEquatable<DateOnly>
//         public record TimeOnly: IComparable<TimeOnly>, IEquatable<TimeOnly>
//     with the ordering spelled out per type — DateTime: "CompareTo = tick
//     order (Kind not normalized, matching .NET)"; TimeSpan: "CompareTo =
//     tick order"; DateOnly: "CompareTo … chronological"; TimeOnly:
//     "CompareTo".
//   * stdlib §2.8 "`IComparable<T>`" declares the constraint's authority
//         public interface IComparable<T>
//           method CompareTo(other: T): int;
//         end IComparable;
//   * stdlib §5.1 "No-selector aggregation overloads" declares
//         public extension IEnumerable<T> where T: IComparable<T>
//           public method Min(): T;              // InvalidOperationException if empty
//           public method Max(): T;              // InvalidOperationException if empty
//     so `T = DateTime` SATISFIES the constraint and `darr.Min()` MUST BE
//     ADMITTED.
//   * spec §14.2 "Constraints" admits it: "`where T: IFoo` | Non-nullable
//     types conforming to `IFoo`: classes implementing it, **records and enums
//     implementing it**, interfaces extending it, and primitives conforming
//     through their standard-library declarations".
//
// THE DEFECT (measured on the base, r30-c1 @ e8655436). The four are
// BUILT-IN value records: they carry no user TypeDecl, so their TypeRef has
// NO `declaration`. In `tryLowerCollectionInstanceCall` the comparator
// resolution therefore resolved nothing for them — `findMethodByName(
// elemTypeRef->declaration, "CompareTo")` needs a declaration, and
// `selfComparableIfaceCompareTo` (the §14.2 rule-3 INTERFACE substitution)
// walks `declaration->baseTypeRefs` — so control reached the #2770 guard and
// the call was refused.
//
// THE FIX (codegen only) adds a SEPARATE arm for a built-in VALUE-RECORD
// element. It could NOT be folded into the pointer arm: that arm reads each
// element as a POINTER (a string object, a class instance, an interface
// instance), whereas a value record is stored INLINE — one scalar word in the
// array buffer — so admitting it there would compare the ADDRESSES of two
// inline words. The new arm loads the element's WORD and orders it by the
// value's own `CompareTo`: a `DateTime` by TICKS (its `Kind` bits masked
// off, per §2.5's "Kind not normalized"), a `TimeSpan` by its SIGNED i64
// tick word, a `DateOnly` by its i32 day number, a `TimeOnly` by its i32
// millisecond-of-day count.
//
// ⚠ COUPLING WITH A SEPARATELY-FILED RESIDUAL — READ THIS BEFORE RE-SPELLING
// THESE ROWS. The RESULT TYPE of the no-selector `Min()`/`Max()` is ALSO
// wrong, and it is NOT fixed by the commit that fixes the refusal: sema types
// the result as a primitive `int` (`ExprTC::typeCall`,
// src/sema/ExprTypecheck_Calls.cpp — the `ptrComparableElem` arm keys on a
// Class/Interface/string element, so a built-in value-record element falls
// through to the `int` default). That is the SECOND facet of #8049 and the
// coordinator has filed it separately, against a live lane's sema file.
//
//     ⇒ `var x: DateTime := darr.Min();` is STILL REFUSED today, with a
//       §5.1 conversion diagnostic ("no implicit conversion from a concrete
//       primitive to the value type `DateTime`"). That binding shape is
//       therefore NOT asserted here.
//     ⇒ These rows observe the computed value through a `long`-typed binding,
//       which is the shape that is legal while sema types the result `int`
//       (spec §5.1's numeric widening int → long). It prints the winner's
//       word EXACTLY — the full 64-bit tick count, not a truncation.
//     ⇒ WHEN THE RESIDUAL RESULT-TYPE FIX LANDS, these rows MUST BE
//       RE-SPELLED against the `DateTime`-typed result (the binding becomes
//       `var x: DateTime := darr.Min();` and the observable becomes the
//       element's own rendering). Until then they pin the value the codegen
//       arm computes, which is the subject of THIS commit.
//
// EVERY ROW IS POSITIVE: each asserts that the program COMPILES, RUNS, and
// PRINTS the expected value. A program that is rejected (as it was before the
// fix) yields the harness's "<did-not-run>" sentinel, which no expected string
// matches — so the refusal is observed as a FAILURE of these rows, not
// asserted as an outcome.
//
// `Guid` (stdlib §2.6, also `IComparable<Guid>`) is deliberately NOT covered:
// it is a `{i64,i64}` STRUCT whose §2.6 order is "lexicographic over the 16
// canonical big-endian bytes", not a scalar-word order, so it needs a
// structural comparison of its own. `Random` (stdlib §2.7) is a CLASS, not a
// value record, and declares no `IComparable` — the §5.1 refusal is correct
// for it.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// The program's stdout, verbatim. "<did-not-run>" when the program was
// rejected, crashed or timed out — the value a REFUSED program produces here,
// and the reason every row below fails before the fix.
std::string out(const std::string& program) {
    auto r = runAot(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

// The same, with the GENERATED code run under AddressSanitizer/LSan: the new
// arm READS the element word out of the array buffer, so an out-of-bounds or
// wrongly-sized load is a memory error rather than a wrong number.
std::string outChecked(const std::string& program) {
    auto r = bezel::test::runAotChecked(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

std::string prog(const std::string& body) {
    return "module M; class C "
           "public static method Main(): int "
           + body +
           " return 0; end Main; end C;";
}

}  // namespace

// #8049 / stdlib §2.5 + §5.1 + §2.8 / spec §14.2: `Array<DateTime>.Min()` /
// `.Max()` are ADMITTED and order by TICK.
//
// The elements are 2019-01-01, 2021-01-01 and 2020-01-02, so the minimum is
// the FIRST and the maximum the LAST of the array — an address-ordering
// (the bug a naive widening of the pointer arm would introduce) would answer
// the first and the last element of the BUFFER instead and print
// 637181568000000000 / 637450560000000000.
//
// The printed numbers are the winners' TICK COUNTS: 2019-01-01 is 737,059
// days after 0001-01-01 and 2021-01-01 is 737,790 (2020 is a leap year, so
// the span is 731 days). A tick is 100 ns, so 737,059 days is
// 737059 * 864000000000 = 636818976000000000 ticks.
//
// The values are read through a `long` binding because sema types the
// no-selector result as `int` today (see the RESIDUAL note in the header);
// the binding is what keeps the full 64-bit count intact.
BEZEL_TEST(array_datetime_min_max_8049, datetime_element_min_max_admitted_and_ordered) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var arr: Array<DateTime> := [new DateTime(2019, 1, 1), "
            "new DateTime(2021, 1, 1), new DateTime(2020, 1, 2)]; "
            "var lo: long := arr.Min(); Console.WriteLine(lo); "
            "var hi: long := arr.Max(); Console.WriteLine(hi); ")),
        "636818976000000000\n637450560000000000\n");
}

// The same shape under AddressSanitizer: the new arm LOADS the element word
// out of the array buffer, so a wrongly-sized or out-of-bounds load is a
// memory error rather than a wrong number. (The elements are value types, so
// the program owns no heap object and LSan has nothing to report.)
BEZEL_TEST(array_datetime_min_max_8049, datetime_element_min_max_under_asan) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var arr: Array<DateTime> := [DateTime.FromTicks(300), "
            "DateTime.FromTicks(100), DateTime.FromTicks(200)]; "
            "var lo: long := arr.Min(); Console.WriteLine(lo); "
            "var hi: long := arr.Max(); Console.WriteLine(hi); ")),
        "100\n300\n");
}

// #8049 / stdlib §2.5: `Array<TimeSpan>.Min()` / `.Max()` are ADMITTED and
// order by TICK — and the tick word is SIGNED, because a span may be
// negative. The elements are 300, 100 and -50 ticks, so the minimum is -50
// and the maximum is 300. An UNSIGNED comparison would answer 100 and 300.
BEZEL_TEST(array_datetime_min_max_8049, timespan_element_min_max_admitted_and_ordered) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var arr: Array<TimeSpan> := [TimeSpan.FromTicks(300), "
            "TimeSpan.FromTicks(100), TimeSpan.FromTicks(-50)]; "
            "var lo: long := arr.Min(); Console.WriteLine(lo); "
            "var hi: long := arr.Max(); Console.WriteLine(hi); ")),
        "-50\n300\n");
}

// #8049 / stdlib §2.5: `Array<DateOnly>.Min()` / `.Max()` are ADMITTED and
// order CHRONOLOGICALLY. A `DateOnly` is an i32 day number since 0001-01-01,
// so the winners are 737,059 (2019-01-01) and 737,790 (2021-01-01). The
// element word is 32 bits wide, so this row's value is exact with no width
// question at all.
BEZEL_TEST(array_datetime_min_max_8049, dateonly_element_min_max_admitted_and_ordered) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var arr: Array<DateOnly> := [new DateOnly(2019, 1, 1), "
            "new DateOnly(2021, 1, 1), new DateOnly(2020, 1, 2)]; "
            "var lo: long := arr.Min(); Console.WriteLine(lo); "
            "var hi: long := arr.Max(); Console.WriteLine(hi); ")),
        "737059\n737790\n");
}

// #8049 / stdlib §2.5: `Array<TimeOnly>.Min()` / `.Max()` are ADMITTED and
// order by the millisecond-of-day count. A `TimeOnly` is an i32 millisecond
// count, so 01:00:00 is 3,600,000 and 03:00:00 is 10,800,000.
BEZEL_TEST(array_datetime_min_max_8049, timeonly_element_min_max_admitted_and_ordered) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var arr: Array<TimeOnly> := [new TimeOnly(3, 0, 0), "
            "new TimeOnly(1, 0, 0), new TimeOnly(2, 0, 0)]; "
            "var lo: long := arr.Min(); Console.WriteLine(lo); "
            "var hi: long := arr.Max(); Console.WriteLine(hi); ")),
        "3600000\n10800000\n");
}

// #8049 / stdlib §2.5 ("CompareTo = tick order (Kind not normalized,
// matching .NET)"): a `DateTime`'s `Kind` rides in the TOP TWO BITS of its
// word, and the ordering is by TICKS — the Kind is NOT normalised, i.e. it
// takes no part in the comparison.
//
// The elements are an Unspecified 300, a Utc 100 and an Unspecified 200. The
// Utc element's WORD is 100 + 2^62 = 4611686018427388004, so:
//
//   * ordering by TICKS gives the minimum as the Utc 100-tick element
//     (printed as its own word, 4611686018427388004) and the maximum as the
//     Unspecified 300-tick element (300);
//   * ordering by the RAW WORD would give the minimum as the Unspecified
//     200-tick element (200) and the maximum as the Utc one
//     (4611686018427388004).
//
// So this row distinguishes tick-order from word-order, AND pins that the
// winner keeps its OWN `Kind` (the minimum's printed word still carries the
// Utc bits) — the same discipline the `Math.Min`/`Max` arm applies.
BEZEL_TEST(array_datetime_min_max_8049, datetime_element_kind_not_normalized_in_ordering) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var arr: Array<DateTime> := [DateTime.FromTicks(300), "
            "DateTime.FromTicks(100, DateTimeKind.Utc), "
            "DateTime.FromTicks(200)]; "
            "var lo: long := arr.Min(); Console.WriteLine(lo); "
            "var hi: long := arr.Max(); Console.WriteLine(hi); ")),
        "4611686018427388004\n300\n");
}

// #8049 / stdlib §5.1 ("InvalidOperationException if empty"): the new arm
// keeps the empty-source guard the pointer arm has, so an EMPTY array still
// throws rather than reading element 0.
BEZEL_TEST(array_datetime_min_max_8049, datetime_empty_source_throws_invalid_operation_exception) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var arr: Array<DateTime> := []; "
            "var caught := false; "
            "try "
            "  Console.WriteLine(arr.Min()); "
            "catch InvalidOperationException => caught := true; end; "
            "Console.WriteLine(caught); ")),
        "True\n");
}

// CONTROL — the SELECTOR form (`Min<U>(selector: (T) -> U): U`,
// stdlib §5) is the documented alternative and already worked before this
// fix; it must keep working, and it must keep ordering by the same ticks.
// The projection is `d.Ticks`, so the result is a `long` and the value is
// exact.
BEZEL_TEST(array_datetime_min_max_8049, control_selector_form_still_works) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var arr: Array<DateTime> := [DateTime.FromTicks(300), "
            "DateTime.FromTicks(100), DateTime.FromTicks(200)]; "
            "var lo: long := arr.Min(lambda(d: DateTime) => d.Ticks); "
            "Console.WriteLine(lo); "
            "var hi: long := arr.Max(lambda(d: DateTime) => d.Ticks); "
            "Console.WriteLine(hi); ")),
        "100\n300\n");
}

// CONTROL — a `string` element keeps the POINTER arm's ordinal comparison
// (stdlib §9.6). The new value-record arm is selected from the STATIC
// element type and is disjoint from the pointer arm, so this row is
// bit-identical before and after the fix.
BEZEL_TEST(array_datetime_min_max_8049, control_string_element_still_ordinal) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var arr: Array<string> := [\"c\", \"a\", \"b\"]; "
            "Console.WriteLine(arr.Min()); "
            "Console.WriteLine(arr.Max()); ")),
        "a\nc\n");
}

// CONTROL — an `int` element keeps the numeric path the new arm does not
// touch.
BEZEL_TEST(array_datetime_min_max_8049, control_int_element_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var arr: Array<int> := [3, 1, 2]; "
            "Console.WriteLine(arr.Min()); "
            "Console.WriteLine(arr.Max()); ")),
        "1\n3\n");
}
