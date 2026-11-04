// Regression tests for issue #8092, FACET A — the RESULT TYPE of the
// no-selector `Min()`/`Max()` over a comparable element must be `T` (the
// ELEMENT type), not a PRIMITIVE. The other facet (the `*OrNull` siblings'
// ORDERING) is in array_minmax_ornull_value_elem_8092_test.cpp; the #8049
// REFUSAL itself was fixed by 2fd1ed56, on `r30-c1`.
//
// WHAT THE DOCUMENTS REQUIRE (each §-number opened at v0.355 in this tree).
//
//   * stdlib §5.1 "No-selector aggregation overloads" declares
//         public extension IEnumerable<T> where T: IComparable<T>
//           public method Min(): T;              // InvalidOperationException if empty
//           public method Max(): T;              // InvalidOperationException if empty
//           public method MinOrNull(): T?;       // null if empty
//           public method MaxOrNull(): T?;       // null if empty
//     ⇒ the no-selector form's RESULT TYPE is the ELEMENT type `T`, and the
//       `*OrNull` siblings return `T?` — the SAME `T`, not a primitive.
//   * stdlib §2.5 "`DateTime`, `TimeSpan`, `DateOnly`, `TimeOnly`" declares
//         public record DateTime: IComparable<DateTime>, IEquatable<DateTime>
//         public record TimeSpan: IComparable<TimeSpan>, IEquatable<TimeSpan>
//         public record DateOnly: IComparable<DateOnly>, IEquatable<DateOnly>
//         public record TimeOnly: IComparable<TimeOnly>, IEquatable<TimeOnly>
//     with the ordering spelled out per type — DateTime: "CompareTo = tick
//     order (Kind not normalized, matching .NET)"; TimeSpan: "CompareTo = tick
//     order"; DateOnly: "chronological"; TimeOnly: "CompareTo".
//   * stdlib §2.8 "`IComparable<T>`" declares the constraint's authority:
//         public interface IComparable<T>
//           method CompareTo(other: T): int;
//         end IComparable;
//   * stdlib §5.1's prose: "On an **empty source**: … `Min`/`Max` — selector
//     and no-selector forms alike — and `Average` throw
//     `InvalidOperationException`" and the `*OrNull` siblings "return `null`
//     instead".
//   * spec §14.2 "Constraints" admits a record: "`where T: IFoo` | Non-nullable
//     types conforming to `IFoo`: classes implementing it, **records and enums
//     implementing it**, interfaces extending it, and primitives conforming
//     through their standard-library declarations".
//
// FACET A — the RESULT TYPE was a PRIMITIVE.
//
// MEASURED on the base (r30-c1 @ 604a0778), `var x: DateTime := darr.Min();`
// was REFUSED with
//
//   "the initializer for `x` has a type that is not implicitly convertible to
//    the target type (§5.1): §5.1 has no implicit conversion from a concrete
//    primitive to the value type `DateTime` …"
//
// ⇒ SEMA TYPED THE NO-SELECTOR `Min()` AS `int`, NOT `T`. The INFERRED form
// was worse than a refusal: it compiled and printed the TRUNCATED tick word
// (313868288 for the 636818976000000000-tick minimum), and an argument
// position and an interpolation were refused the same way.
//
// The defect site was `src/sema/ExprTypecheck_Calls.cpp`'s `ExprTC::typeCall`
// — the `ptrComparableElem` arm, which keyed on a CLASS/INTERFACE/`string`
// element only. A built-in VALUE RECORD carries no user `TypeDecl`, so its
// TypeRef has no `declaration` and the arm never saw it; a USER RECORD
// implementing `IComparable<T>` was not in the arm either. Both fell through
// to the `int` default.
//
// FACET B — the `*OrNull` SIBLINGS produced a WRONG VALUE.
//
// MEASURED on the base, over {Utc 300 ticks, Unspecified 100, Unspecified 200}
// (the Utc `Kind` rides in the word's top two bits, so its RAW word is
// 300 + 2^62), `MaxOrNull()` returned the UTC element while the plain
// `Max()` — the same §5.1 extension — returned the Unspecified 300 one. The
// `*OrNull` arm compared the RAW WORD; stdlib §2.5 orders a `DateTime` by
// TICKS with the `Kind` NOT normalized. The two arms of ONE extension
// disagreed about a value-record element, and the `*OrNull` one was silently
// wrong.
//
// ⇒ EVERY ROW HERE IS POSITIVE: the program must COMPILE, RUN and PRINT the
// expected value. A program that is REJECTED yields the harness's
// "<did-not-run>" sentinel, which no expected string matches — so a refusal
// and a wrong value are both observed as a FAILURE of the row.
//
// ⇒ THE CONTROLS (an `int`, a `double` and a `string` element) pin the
// unchanged paths.

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

// The same, with the GENERATED code run under AddressSanitizer/LSan.
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

// ── FACET A ───────────────────────────────────────────────────────────────
// The no-selector `Min()`'s RESULT TYPE is `T` (stdlib §5.1), so an
// ANNOTATED binding of the element type must bind and hold the right value.
//
// The elements are 2021-01-01 and 2019-01-01, so the minimum is the SECOND
// element — an address-ordering (the bug a naive widening of the pointer arm
// would introduce) would answer the FIRST element of the buffer instead.
BEZEL_TEST(array_minmax_value_elem_result_type_8092,
           datetime_min_result_is_the_element_type) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var darr: Array<DateTime> := [new DateTime(2021, 1, 1), "
            "new DateTime(2019, 1, 1)]; "
            "var x: DateTime := darr.Min(); "
            "Console.WriteLine(x); ")),
        "2019-01-01T00:00:00\n");
}

// The INFERRED binding: `var x := darr.Min();` must now hold a `DateTime`,
// so the program prints the element's own rendering. Before the fix the
// inferred local was an `int` and printed the TRUNCATED tick word
// (313868288 for this source).
BEZEL_TEST(array_minmax_value_elem_result_type_8092,
           datetime_min_inferred_binding_is_the_element_type) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var darr: Array<DateTime> := [new DateTime(2021, 1, 1), "
            "new DateTime(2019, 1, 1)]; "
            "var x := darr.Min(); "
            "Console.WriteLine(x); ")),
        "2019-01-01T00:00:00\n");
}

// The ARGUMENT position: a `DateTime` parameter must accept the result.
BEZEL_TEST(array_minmax_value_elem_result_type_8092,
           datetime_min_binds_at_an_argument_position) {
    BEZEL_EXPECT_EQ(
        out("module M; class C "
           "public static method take(d: DateTime): DateTime "
           "return d; end take; "
           "public static method Main(): int "
           "var darr: Array<DateTime> := [new DateTime(2021, 1, 1), "
           "new DateTime(2019, 1, 1)]; "
           "Console.WriteLine(take(darr.Min())); "
           "return 0; end Main; end C;"),
        "2019-01-01T00:00:00\n");
}

// The INTERPOLATION position: `$"{darr.Min()}"` must render the element.
BEZEL_TEST(array_minmax_value_elem_result_type_8092,
           datetime_min_interpolates_as_the_element_type) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var darr: Array<DateTime> := [new DateTime(2021, 1, 1), "
            "new DateTime(2019, 1, 1)]; "
            "Console.WriteLine($\"{darr.Min()}\"); ")),
        "2019-01-01T00:00:00\n");
}

// The `Max()` sibling: the result type is `T` too, and the winner is the
// LARGEST element (the FIRST one here).
BEZEL_TEST(array_minmax_value_elem_result_type_8092,
           datetime_max_result_is_the_element_type) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var darr: Array<DateTime> := [new DateTime(2021, 1, 1), "
            "new DateTime(2019, 1, 1)]; "
            "var x: DateTime := darr.Max(); "
            "Console.WriteLine(x); ")),
        "2021-01-01T00:00:00\n");
}

// stdlib §2.5's OTHER THREE temporal value records: `TimeSpan`, `DateOnly`
// and `TimeOnly` are `IComparable<Self>` too, so their no-selector result is
// the element type as well. `TimeSpan`'s tick word is SIGNED, so the minimum
// over {300, 100, -50} is -50 — an UNSIGNED comparison would answer 100.
BEZEL_TEST(array_minmax_value_elem_result_type_8092,
           timespan_min_result_is_the_element_type) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var arr: Array<TimeSpan> := [TimeSpan.FromTicks(300), "
            "TimeSpan.FromTicks(100), TimeSpan.FromTicks(-50)]; "
            "var x: TimeSpan := arr.Min(); "
            "Console.WriteLine(x.Ticks); ")),
        "-50\n");
}

// stdlib §2.5's `DateOnly`: "CompareTo … chronological" — the minimum is the
// earlier day, and the result is a `DateOnly` (rendered "yyyy-MM-dd").
BEZEL_TEST(array_minmax_value_elem_result_type_8092,
           dateonly_min_result_is_the_element_type) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var arr: Array<DateOnly> := [new DateOnly(2019, 1, 1), "
            "new DateOnly(2021, 1, 1)]; "
            "var x: DateOnly := arr.Min(); "
            "Console.WriteLine(x.ToString()); ")),
        "2019-01-01\n");
}

// stdlib §2.5's `TimeOnly`: the minimum is the earliest millisecond-of-day.
BEZEL_TEST(array_minmax_value_elem_result_type_8092,
           timeonly_min_result_is_the_element_type) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var arr: Array<TimeOnly> := [new TimeOnly(3, 0, 0), "
            "new TimeOnly(1, 0, 0)]; "
            "var x: TimeOnly := arr.Min(); "
            "Console.WriteLine(x.ToString()); ")),
        "01:00:00\n");
}

// spec §14.2's "records and enums implementing it": a USER RECORD that
// implements `IComparable<T>` is a comparable element, so the no-selector
// result is the RECORD type — and the ORDER is the record's own `CompareTo`.
//
// MEASURED on the base, this program did not compile at all: sema typed the
// result `int` (so `x.cents` was "no member `cents` on primitive type
// `int`"), and the shape that reached codegen produced malformed IR ("Both
// operands to ICmp instruction are not of the same type! … %record.Meters vs
// i32"), which the LLVM verifier REJECTED — the compiler ABORTED.
BEZEL_TEST(array_minmax_value_elem_result_type_8092,
           user_record_min_result_is_the_record_type) {
    BEZEL_EXPECT_EQ(
        out(progWithRecord(
            "var arr: Array<Meters> := [new Meters(3), new Meters(1), "
            "new Meters(2)]; "
            "var x: Meters := arr.Min(); "
            "Console.WriteLine(x.cents); ")),
        "1\n");
}

// The `Max()` sibling over a user record: the winner is the LARGEST element
// by the record's own `CompareTo`.
BEZEL_TEST(array_minmax_value_elem_result_type_8092,
           user_record_max_result_is_the_record_type) {
    BEZEL_EXPECT_EQ(
        out(progWithRecord(
            "var arr: Array<Meters> := [new Meters(3), new Meters(1), "
            "new Meters(2)]; "
            "var x: Meters := arr.Max(); "
            "Console.WriteLine(x.cents); ")),
        "3\n");
}

// The same shape under AddressSanitizer: the new arm READS the element out of
// the array buffer and SPILLS it to a slot for the `CompareTo` call, so a
// wrongly-sized or out-of-bounds load is a memory error rather than a wrong
// number. (The elements are value types, so the program owns no heap object
// and LSan has nothing to report.)
BEZEL_TEST(array_minmax_value_elem_result_type_8092,
           user_record_min_max_under_asan) {
    BEZEL_EXPECT_EQ(
        outChecked(progWithRecord(
            "var arr: Array<Meters> := [new Meters(3), new Meters(1), "
            "new Meters(2)]; "
            "var lo: Meters := arr.Min(); "
            "Console.WriteLine(lo.cents); "
            "var hi: Meters := arr.Max(); "
            "Console.WriteLine(hi.cents); ")),
        "1\n3\n");
}

// CONTROLS — the unchanged paths. An `int` element keeps the numeric result,
// a `double` element keeps its own, and a `string` element keeps the §9.6
// ordinal relation (and its `string` result type).
BEZEL_TEST(array_minmax_value_elem_result_type_8092,
           control_int_double_and_string_elements_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var ai: Array<int> := [3, 1, 2]; "
            "Console.WriteLine(ai.Min()); "
            "Console.WriteLine(ai.Max()); "
            "var ad: Array<double> := [3.5, 1.5, 2.5]; "
            "Console.WriteLine(ad.Min()); "
            "Console.WriteLine(ad.Max()); "
            "var astr: Array<string> := [\"c\", \"a\", \"b\"]; "
            "Console.WriteLine(astr.Min()); "
            "Console.WriteLine(astr.Max()); ")),
        "1\n3\n1.5\n3.5\na\nc\n");
}

