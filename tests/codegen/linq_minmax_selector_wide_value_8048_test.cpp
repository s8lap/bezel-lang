// Regression tests for issue #8048: a LINQ `Min`/`Max` over a SELECTOR whose
// RESULT type is a MULTI-WORD built-in value type (`Guid` — a 128-bit value,
// stdlib §2.6) returned a GARBAGE 128-bit value: the accumulator was one
// machine word wide, so the value could not survive it.
//
// WHAT THE DOCUMENTS REQUIRE (each §-number opened at v0.355 in this tree).
//
//   * stdlib §5 "System.Collections.Linq" declares, on the sequence surface,
//         public method Min<U>(selector: (T) -> U): U where U: IComparable<U>;
//         public method Max<U>(selector: (T) -> U): U where U: IComparable<U>;
//     so the selector's RESULT type U is the RESULT type of the call, and U is
//     constrained only by `IComparable<U>`.
//   * stdlib §2.6 "`Guid`" declares
//         public record Guid: IComparable<Guid>, IEquatable<Guid>
//           // (#1357): CompareTo = lexicographic over the 16 canonical
//           // big-endian bytes (stated explicitly — .NET's field-group order
//           // is a known trap); ordering is CompareTo only — no <,>,<=,>=
//           // overloads are declared.
//           public method CompareTo(other: Guid): int;
//     ⇒ `Guid` IS `IComparable<Guid>`, so `Array<Guid>.Min(sel)` is an
//     ADMITTED shape — the correct outcome is a LOWERING that keeps the whole
//     128-bit value and orders it by §2.6's rule, NOT a rejection.
//   * stdlib §5 (the empty-source rule): "Min/Max/Average, selector and
//     no-selector forms alike: InvalidOperationException on an empty source".
//
// THE DEFECT (measured on the base, r30-c1 @ d13e2fbd). The `Min`/`Max`
// accumulator was keyed off the selector's RESULT TYPE only when that type is
// a numeric PRIMITIVE (`uRef->primitive`) or one of the four ONE-WORD built-in
// value records (`DateTime`, `TimeSpan`, `DateOnly`, `TimeOnly` — the #7874
// fix). A `Guid` is neither: it is a `{i64,i64}` STRUCT (16 bytes), so the
// accumulator stayed the `int` DEFAULT — the `i32` placeholder — while the
// selector's closure returns the `{i64,i64}` value. The value therefore
// arrived half-written, and the printed result mixed the right low half with
// whatever was on the stack.
//
// ⇒ the fix keys the accumulator off the VALUE TYPE for `Guid` too: the
// accumulator holds the whole `{i64,i64}` and the comparison is §2.6's own
// 16-byte lexicographic order (`bz_guid_compare` — the very function the
// `Guid.CompareTo` member call dispatches to).
//
// ORACLE. `runAot` (the AOT path). Every row asserts the EXACT STDOUT — a
// VALUE oracle, never "no diagnostic was produced" — so a legal `Min`/`Max`
// over a `Guid` selector must compile and print the value stdlib §2.6's
// ordering selects.
//
// ⚠ THE ORDERING IS NOT "compare the two halves as integers". The 16
// canonical bytes live in the two halves LITTLE-ENDIAN (the runtime's
// `bz_guid_t` is `{ uint64_t lo; uint64_t hi; }` and `bz_guid_compare` is a
// `memcmp` over the raw bytes), so the canonical order compares the LEAST
// significant byte of `lo` FIRST. ROW 2 pins that distinction: its two
// elements are ordered the OTHER WAY ROUND by a half-word comparison, so a
// fix that compared `lo`/`hi` as unsigned integers would answer the opposite
// pair.

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

// The same, with the GENERATED code run under AddressSanitizer/LSan: the new
// arm LOADS the element's whole `{i64,i64}` out of the array buffer, so an
// out-of-bounds or wrongly-sized load is a memory error rather than a wrong
// value. (The elements are value types, so the program owns no heap object
// and LSan has nothing to report.)
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

// =====================================================================
// ROW 1 — THE ISSUE'S OWN REPRO (its deterministic program, verbatim).
// The two elements are the all-zero `Guid` and the `01 02 … 10` `Guid`, so
// stdlib §2.6's order selects the all-zero one as the MINIMUM and the
// `01 02 … 10` one as the MAXIMUM.
//
// Pre-fix: "min  =00000000-0000-0000-0100-000000000000" (the low 4 bytes,
// then zeros) and "max  =01020304-fe7f-0000-e8a9-c280fe7f0000" (the low
// 4 bytes, then STACK GARBAGE — `fe7f` is the high half of a stack
// pointer).
// Post-fix: the exact minimum and maximum.
// =====================================================================
BEZEL_TEST(linq_minmax_selector_wide_value_8048,
           the_issue_repro_min_and_max_over_a_guid_selector) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var b0 := new Array<byte>(); "
            "var i := 0; "
            "while i < 16 do b0.Add(byte(0)); i := i + 1; end while; "
            "var b1 := new Array<byte>(); "
            "var j := 0; "
            "while j < 16 do b1.Add(byte(j + 1)); j := j + 1; end while; "
            "var g0 := Guid.FromByteArray(b0); "
            "var g1 := Guid.FromByteArray(b1); "
            "var arr := new Array<Guid>(); "
            "arr.Add(g1); arr.Add(g0); "
            "Console.WriteLine(\"elem0=\" + arr[0].ToString()); "
            "Console.WriteLine(\"elem1=\" + arr[1].ToString()); "
            "Console.WriteLine(\"min  =\" + "
            "arr.Min(lambda(x: Guid) => x).ToString()); "
            "Console.WriteLine(\"max  =\" + "
            "arr.Max(lambda(x: Guid) => x).ToString()); ")),
        "elem0=01020304-0506-0708-090a-0b0c0d0e0f10\n"
        "elem1=00000000-0000-0000-0000-000000000000\n"
        "min  =00000000-0000-0000-0000-000000000000\n"
        "max  =01020304-0506-0708-090a-0b0c0d0e0f10\n");
}

// =====================================================================
// ROW 2 — THE ORDERING ITSELF (stdlib §2.6: "CompareTo = lexicographic over
// the 16 canonical big-endian bytes").
//
// The two elements differ ONLY in WHERE their single non-zero byte sits:
//   a = 01000000-0000-0000-0000-000000000000  → canonical byte 0 is 0x01
//   b = 00000000-0000-0000-0001-000000000000  → canonical byte 7 is 0x01
//
// The canonical order compares byte 0 first, so `b` < `a` and the MINIMUM is
// `b`. A fix that compared the two 64-bit HALVES as unsigned integers (the
// plausible-but-wrong reading, since the halves hold the canonical bytes
// LITTLE-ENDIAN) would order `a` < `b` and answer the opposite pair.
//
// Pre-fix: the minimum and maximum are garbage 128-bit values.
// Post-fix: "min=00000000-0000-0000-0001-000000000000" and
//           "max=01000000-0000-0000-0000-000000000000".
// =====================================================================
BEZEL_TEST(linq_minmax_selector_wide_value_8048,
           a_guid_selector_orders_by_the_canonical_bytes) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var a := Guid.Parse(\"01000000-0000-0000-0000-000000000000\"); "
            "var b := Guid.Parse(\"00000000-0000-0000-0001-000000000000\"); "
            "var arr := new Array<Guid>(); "
            "arr.Add(a); arr.Add(b); "
            "Console.WriteLine(\"a=\" + a.ToString()); "
            "Console.WriteLine(\"b=\" + b.ToString()); "
            "Console.WriteLine(\"min=\" + "
            "arr.Min(lambda(x: Guid) => x).ToString()); "
            "Console.WriteLine(\"max=\" + "
            "arr.Max(lambda(x: Guid) => x).ToString()); ")),
        "a=01000000-0000-0000-0000-000000000000\n"
        "b=00000000-0000-0000-0001-000000000000\n"
        "min=00000000-0000-0000-0001-000000000000\n"
        "max=01000000-0000-0000-0000-000000000000\n");
}

// =====================================================================
// ROW 3 — THE HIGH HALF TAKES PART. The two elements share their low half
// and differ in the high one, so only the high half can decide the order —
// a fix that compared the LOW half only (or that kept the low half and lost
// the high one, which is the pre-fix symptom) would answer wrongly.
//
// Pre-fix: garbage.
// Post-fix: the all-zero element is the minimum, the `…-010000000000` one is
// the maximum.
// =====================================================================
BEZEL_TEST(linq_minmax_selector_wide_value_8048,
           a_guid_selector_keeps_both_halves_of_the_value) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var c := Guid.Parse(\"00000000-0000-0000-0000-010000000000\"); "
            "var d := Guid.Parse(\"00000000-0000-0000-0000-000000000000\"); "
            "var arr := new Array<Guid>(); "
            "arr.Add(c); arr.Add(d); "
            "Console.WriteLine(\"c=\" + c.ToString()); "
            "Console.WriteLine(\"d=\" + d.ToString()); "
            "Console.WriteLine(\"min=\" + "
            "arr.Min(lambda(x: Guid) => x).ToString()); "
            "Console.WriteLine(\"max=\" + "
            "arr.Max(lambda(x: Guid) => x).ToString()); ")),
        "c=00000000-0000-0000-0000-010000000000\n"
        "d=00000000-0000-0000-0000-000000000000\n"
        "min=00000000-0000-0000-0000-000000000000\n"
        "max=00000000-0000-0000-0000-010000000000\n");
}

// =====================================================================
// ROW 4 — THE SAME UNDER AddressSanitizer. The new arm LOADS the element's
// whole `{i64,i64}` out of the array buffer, so a wrongly-sized or
// out-of-bounds load is a MEMORY ERROR rather than a wrong number.
//
// Pre-fix: garbage values (the load itself is not out of bounds — the
// accumulator is the wrong WIDTH, not the wrong offset).
// Post-fix: the exact minimum and maximum.
// =====================================================================
BEZEL_TEST(linq_minmax_selector_wide_value_8048,
           the_guid_selector_under_asan) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a := Guid.Parse(\"01020304-0506-0708-090a-0b0c0d0e0f10\"); "
            "var b := Guid.Parse(\"00000000-0000-0000-0000-000000000000\"); "
            "var arr := new Array<Guid>(); "
            "arr.Add(a); arr.Add(b); "
            "Console.WriteLine(arr.Min(lambda(x: Guid) => x).ToString()); "
            "Console.WriteLine(arr.Max(lambda(x: Guid) => x).ToString()); ")),
        "00000000-0000-0000-0000-000000000000\n"
        "01020304-0506-0708-090a-0b0c0d0e0f10\n");
}

// =====================================================================
// ROW 5 — THE LAZY SEQUENCE SURFACE. stdlib §5's operators are also
// declared on `IEnumerable<T>`, so the same `Min<U>(selector)` is reachable
// through a lazy chain (`arr.Select(...)` produces an `IEnumerable<Guid>`).
// The chain is materialised and ordered by the SAME accumulator, so the
// multi-word result is broken there exactly as it is on the direct array —
// and fixed by the same change.
//
// Pre-fix: "min=00000000-fe7f-0000-a8f3-1d9afe7f0000" (garbage).
// Post-fix: the exact minimum.
// =====================================================================
BEZEL_TEST(linq_minmax_selector_wide_value_8048,
           a_lazy_chain_min_over_a_guid_selector) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var arr := new Array<Guid>(); "
            "arr.Add(Guid.Parse(\"01020304-0506-0708-090a-0b0c0d0e0f10\")); "
            "arr.Add(Guid.Parse(\"00000000-0000-0000-0000-000000000000\")); "
            "var q := arr.Select(lambda(x: Guid) => x); "
            "Console.WriteLine(\"min=\" + "
            "q.Min(lambda(x: Guid) => x).ToString()); ")),
        "min=00000000-0000-0000-0000-000000000000\n");
}

// =====================================================================
// ROW 6 — THE ACCUMULATOR IS KEYED OFF `U`, NOT OFF THE ELEMENT TYPE. §5
// declares the RESULT type as the SELECTOR's result type `U`, so a selector
// that projects an `int` element to a `Guid` must return a `Guid` — and the
// accumulator must hold that `Guid`'s whole 128 bits.
//
// Pre-fix: "min=01020304-ff7f-0000-f882-fc8cff7f0000" (garbage).
// Post-fix: the exact `Guid`.
// =====================================================================
BEZEL_TEST(linq_minmax_selector_wide_value_8048,
           a_guid_projection_over_an_int_element_keeps_the_guid) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var arr := new Array<int>(); "
            "arr.Add(3); arr.Add(1); "
            "Console.WriteLine(\"min=\" + "
            "arr.Min(lambda(x: int) => "
            "Guid.Parse(\"01020304-0506-0708-090a-0b0c0d0e0f10\")).ToString()); ")),
        "min=01020304-0506-0708-090a-0b0c0d0e0f10\n");
}

// =====================================================================
// ROW 7 — THE EMPTY SOURCE (stdlib §5: "InvalidOperationException on an
// empty source" for the selector and no-selector forms alike). The selector
// arm keeps that guard, so an EMPTY array still throws rather than reading
// element 0.
//
// Pre-fix: the guard was already there (it is the shared §5.1 guard), so this
// row is GREEN on both arms — it is a CONTROL for the guard, not a defect
// row.
// =====================================================================
BEZEL_TEST(linq_minmax_selector_wide_value_8048,
           an_empty_source_throws_invalid_operation_exception) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var arr := new Array<Guid>(); "
            "var caught := false; "
            "try "
            "  Console.WriteLine(arr.Min(lambda(x: Guid) => x).ToString()); "
            "catch InvalidOperationException => caught := true; end; "
            "Console.WriteLine(caught); ")),
        "True\n");
}

// =====================================================================
// CONTROL — the ONE-WORD built-in value record is unchanged (stdlib §2.5's
// `DateTime`, whose order is by TICKS). The new arm is selected from the
// STATIC result type, so a `DateTime` selector keeps the #7874 path.
//
// The elements are 500, 300 and 100 ticks, so the tick order selects 100 and
// 500.
//
// Pre-fix and post-fix: "100" and "500".
// =====================================================================
BEZEL_TEST(linq_minmax_selector_wide_value_8048,
           control_a_datetime_selector_is_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var arr := new Array<DateTime>(); "
            "arr.Add(DateTime.FromTicks(500L)); "
            "arr.Add(DateTime.FromTicks(300L)); "
            "arr.Add(DateTime.FromTicks(100L)); "
            "Console.WriteLine(arr.Min(lambda(x: DateTime) => x).Ticks); "
            "Console.WriteLine(arr.Max(lambda(x: DateTime) => x).Ticks); ")),
        "100\n500\n");
}

// =====================================================================
// CONTROL — the OTHER multi-word built-in value type is unchanged: a
// `decimal` (stdlib §9.3) is a 128-bit `{lo32,mid32,hi32,flags}` struct and
// already has its OWN selector arm (the `bz_decimal_cmp` path). This row
// pins that the `Guid` arm did not disturb it.
//
// Pre-fix and post-fix: "1.75" and "9.25".
// =====================================================================
BEZEL_TEST(linq_minmax_selector_wide_value_8048,
           control_a_decimal_selector_is_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var arr := new Array<decimal>(); "
            "arr.Add(9.25m); arr.Add(5.5m); arr.Add(1.75m); "
            "Console.WriteLine(arr.Min(lambda(x: decimal) => x).ToString()); "
            "Console.WriteLine(arr.Max(lambda(x: decimal) => x).ToString()); ")),
        "1.75\n9.25\n");
}

// =====================================================================
// CONTROL — the NUMERIC selector path is unchanged.
//
// Pre-fix and post-fix: "1" and "9".
// =====================================================================
BEZEL_TEST(linq_minmax_selector_wide_value_8048,
           control_a_numeric_selector_is_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var arr := new Array<int>(); "
            "arr.Add(9); arr.Add(1); arr.Add(5); "
            "Console.WriteLine(arr.Min(lambda(x: int) => x).ToString()); "
            "Console.WriteLine(arr.Max(lambda(x: int) => x).ToString()); ")),
        "1\n9\n");
}

// =====================================================================
// CONTROL — a `string` selector keeps the POINTER arm's ordinal comparison
// (stdlib §9.6). The `Guid` arm is selected from the STATIC result type and
// is disjoint from the pointer arm, so this row is bit-identical before and
// after the fix.
//
// Pre-fix and post-fix: "aaa" and "ccc".
// =====================================================================
BEZEL_TEST(linq_minmax_selector_wide_value_8048,
           control_a_string_selector_is_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var arr := new Array<string>(); "
            "arr.Add(\"ccc\"); arr.Add(\"aaa\"); arr.Add(\"bbb\"); "
            "Console.WriteLine(arr.Min(lambda(x: string) => x)); "
            "Console.WriteLine(arr.Max(lambda(x: string) => x)); ")),
        "aaa\nccc\n");
}
