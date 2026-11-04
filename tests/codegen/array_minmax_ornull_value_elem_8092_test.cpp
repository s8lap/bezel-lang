// Regression tests for issue #8092, FACET B — the `*OrNull` siblings
// (`MinOrNull()` / `MaxOrNull()`) must order a VALUE-RECORD element by the
// value's OWN `CompareTo`, not by its raw word, and must not report "empty"
// over a NON-EMPTY array. The other facet (the RESULT TYPE of the no-selector
// `Min()`/`Max()`) is in array_minmax_value_elem_result_type_8092_test.cpp;
// the #8049 REFUSAL itself was fixed by 2fd1ed56, on `r30-c1`.
//
// WHAT THE DOCUMENTS REQUIRE (each §-number opened at v0.355 in this tree).
//
//   * stdlib §5.1 "No-selector aggregation overloads" declares
//         public extension IEnumerable<T> where T: IComparable<T>
//           public method Min(): T;
//           public method Max(): T;
//           public method MinOrNull(): T?;       // null if empty
//           public method MaxOrNull(): T?;       // null if empty
//     ⇒ the `*OrNull` siblings return `T?` — the SAME `T` — and over an
//       EMPTY source they return null while `Min()`/`Max()` throw
//       `InvalidOperationException`.
//   * stdlib §2.5 "`DateTime`, `TimeSpan`, `DateOnly`, `TimeOnly`" declares
//         public record DateTime: IComparable<DateTime>, IEquatable<DateTime>
//     with the ordering spelled out per type — DateTime: "CompareTo = tick
//     order (Kind not normalized, matching .NET)".
//   * stdlib §2.8 "`IComparable<T>`" declares the constraint's authority:
//         public interface IComparable<T>
//           method CompareTo(other: T): int;
//         end IComparable;
//   * spec §14.2 "Constraints" admits a record: "`where T: IFoo` | Non-nullable
//     types conforming to `IFoo`: classes implementing it, **records and enums
//     implementing it**, interfaces extending it, and primitives conforming
//     through their standard-library declarations".
//
// THE DEFECT (measured on the base, r30-c1 @ 604a0778). The `*OrNull` arm of
// the stdlib §5.1 aggregation lowering compares a BUILT-IN VALUE RECORD
// element by its RAW WORD: a `DateTime`'s `Kind` rides in the word's top two
// bits, and stdlib §2.5's order is by TICKS with the `Kind` NOT normalized.
// The plain `Min()`/`Max()` arm (fixed by #8049) masks those bits with
// `dtTicksOf`, so the two arms of ONE extension disagreed about a
// value-record element — and the `*OrNull` one was silently wrong. A USER
// RECORD element was worse: the `*OrNull` arm REFUSED it ("not yet
// implemented for this element type") while the plain `Min()` computed the
// right answer.
//
// ⇒ EVERY ROW HERE IS POSITIVE: the program must COMPILE, RUN and PRINT the
// expected value. A program that is REJECTED yields the harness's
// "<did-not-run>" sentinel, which no expected string matches.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// The program's stdout, verbatim. "<did-not-run>" when the program was
// rejected, crashed or timed out.
std::string out(const std::string& program) {
    auto r = runAot(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

std::string prog(const std::string& body) {
    return "module M; class C "
           "public static method Main(): int "
           + body +
           " return 0; end Main; end C;";
}

// A USER RECORD implementing `IComparable<T>` (spec §14.2's conformance rule
// admits "records and enums implementing it"). It is a VALUE type: stored
// INLINE in the array, one struct per slot.
std::string progWithRecord(const std::string& body) {
    return "module M; "
           "record Meters(cents: int): IComparable<Meters> "
           "  public method CompareTo(other: Meters): int => "
           "    (if cents > other.cents then 1 else "
           "     (if cents < other.cents then -1 else 0)); "
           "end Meters; "
           "class C "
           "public static method Main(): int "
           + body +
           " return 0; end Main; end C;";
}

}  // namespace

// ── FACET B ───────────────────────────────────────────────────────────────
// The `*OrNull` siblings must order a value-record element by the value's
// OWN `CompareTo` — stdlib §2.5's "CompareTo = tick order (Kind not
// normalized, matching .NET)" for a `DateTime`.
//
// The elements mix `Kind`s on purpose: the Utc element's RAW word is
// 100 + 2^62, so a comparison of raw words would rank it ABOVE every
// Unspecified one and answer it as the MAXIMUM, and would rank the
// Unspecified 100-tick element BELOW the Utc one and answer it as the
// MINIMUM.
//
// MEASURED on the base, over {Unspecified 300, Utc 100, Unspecified 200}:
//
//   * `MaxOrNull()` returned the UTC element ("utc", 100 ticks) — the RAW
//     word's maximum — while the plain `Max()` (whose key masks the `Kind`
//     bits, #8049) returned the Unspecified 300-tick element;
//   * `MinOrNull()` returned the Unspecified 100-tick element ("notutc"),
//     while the plain `Min()` returned the Utc 100-tick element.
//
// ⇒ the two arms of ONE §5.1 extension disagreed about a value-record
// element, and the `*OrNull` one was silently wrong.
//
// The tick counts are DISTINCT (300 / 100 / 200) on purpose: with a tie the
// two orders would agree, and the row would not discriminate.
BEZEL_TEST(array_minmax_ornull_value_elem_8092,
           minornull_orders_a_datetime_element_by_ticks) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var darr: Array<DateTime> := [DateTime.FromTicks(300), "
            "DateTime.FromTicks(100, DateTimeKind.Utc), "
            "DateTime.FromTicks(200)]; "
            "var lo: DateTime? := darr.MinOrNull(); "
            "Console.WriteLine(lo!.Ticks); "
            "if lo!.Kind = DateTimeKind.Utc then Console.WriteLine(\"utc\"); "
            "else Console.WriteLine(\"notutc\"); end if; ")),
        "100\nutc\n");
}

// The MAXIMUM: the winner must be the Unspecified 300-tick element, NOT the
// Utc one (whose raw word is larger). The `Kind` is the discriminator — the
// plain `Max()` keeps the winner's OWN `Kind` too.
BEZEL_TEST(array_minmax_ornull_value_elem_8092,
           maxornull_orders_a_datetime_element_by_ticks) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var darr: Array<DateTime> := [DateTime.FromTicks(300), "
            "DateTime.FromTicks(100, DateTimeKind.Utc), "
            "DateTime.FromTicks(200)]; "
            "var hi: DateTime? := darr.MaxOrNull(); "
            "if hi!.Kind = DateTimeKind.Utc then "
            "  Console.WriteLine(\"utc\"); "
            "else Console.WriteLine(\"notutc\"); end if; "
            "Console.WriteLine(hi!.Ticks); ")),
        "notutc\n300\n");
}

// The `??` FALLBACK must not fire over a NON-EMPTY array: stdlib §5.1 says
// the `*OrNull` form returns "null if empty", so over a non-empty source the
// fallback is dead and the VALUE must come through. The fallback is
// `DateTime.MinValue` (0001-01-01T00:00:00), so the row's expected output
// distinguishes the two.
//
// MEASURED on the base, this row already passed: the `*OrNull` arm's EMPTY
// test is a length test, not an element test, so the fallback did not fire.
// The row is kept because it is the issue's facet-B ASSERTION — the printed
// VALUE, not the absence of an error.
BEZEL_TEST(array_minmax_ornull_value_elem_8092,
           minornull_over_a_non_empty_array_is_the_value_not_the_fallback) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var darr: Array<DateTime> := [new DateTime(2021, 1, 1), "
            "new DateTime(2019, 1, 1)]; "
            "var v := darr.MinOrNull() ?? DateTime.MinValue; "
            "Console.WriteLine(v); ")),
        "2019-01-01T00:00:00\n");
}

// stdlib §5.1: the `*OrNull` form returns null over an EMPTY source — the
// one behaviour that separates it from the throwing `Min()`/`Max()`.
BEZEL_TEST(array_minmax_ornull_value_elem_8092,
           minornull_over_an_empty_array_is_null) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var darr: Array<DateTime> := []; "
            "var v: DateTime? := darr.MinOrNull(); "
            "if v = null then Console.WriteLine(\"null\"); "
            "else Console.WriteLine(\"notnull\"); end if; ")),
        "null\n");
}

// The `*OrNull` sibling over a USER RECORD element: MEASURED on the base it
// was REFUSED with "`MinOrNull()` on `Array<Meters>` is not yet implemented
// for this element type" — while the plain `Min()` computed the right
// answer. The two arms of the same §5.1 extension disagreed.
BEZEL_TEST(array_minmax_ornull_value_elem_8092,
           minornull_over_a_user_record_element_is_the_value) {
    BEZEL_EXPECT_EQ(
        out(progWithRecord(
            "var arr: Array<Meters> := [new Meters(3), new Meters(1), "
            "new Meters(2)]; "
            "var v: Meters? := arr.MinOrNull(); "
            "Console.WriteLine(v!.cents); ")),
        "1\n");
}
