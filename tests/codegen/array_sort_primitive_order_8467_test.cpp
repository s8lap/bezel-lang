// Regression tests for issue #8467 — `Array<T>.Sort()` / `Array<T>.Sorted()`.
//
// WHAT THE DOCUMENTS REQUIRE (stdlib §4.4 / §9.1 / §9.3, spec §14.2, v0.357).
//
//   * stdlib §4.4 declares, on `Array<T>`:
//         public method Sort() where T: IComparable<T>;                  // in place
//         public method Sorted(): owned Array<T> where T: IComparable<T>; // fresh copy
//     and states of the parameterless natural-order form: "for a concrete
//     element type the natural-order comparer is that type's own `CompareTo`
//     (for example `int.CompareTo`)".
//   * stdlib §9.1 declares the eight integer types in record syntax — "`SByte`,
//     `Byte`, `Int16`, `UInt16`, `Int32`, `UInt32`, `Int64`, `UInt64`. The
//     example below shows `Int32`; the others have analogous shapes" — the
//     example being `public record Int32: IComparable<int>, IEquatable<int>`.
//   * stdlib §9.2 declares `public record Double: IComparable<double>` and
//     `public record Float: IComparable<float>`; stdlib §9.3 declares
//     `public record Decimal: IComparable<decimal>, IEquatable<decimal>`; and
//     stdlib §9.5 declares `public record Char: IComparable<char>`.
//   * spec §14.2 *Interface conformance* rule 5 is the deciding sentence for a
//     primitive: "T is a primitive. A primitive conforms to the interfaces its
//     standard-library declaration lists: the keyword and PascalCase forms name
//     one type (§3.4), and stdlib §9 declares that type in record syntax, so
//     `int` conforms to `IComparable<int>`/`IEquatable<int>` through the `Int32`
//     declaration exactly as clause 2 reads for records."
//
// THE DEFECT had two halves, and both are pinned here.
//
//   1. A SILENT WRONG ORDER. The comparator read an i32 out of the element
//      SLOT and returned the DIFFERENCE of the two i32s, so (a) for every
//      element whose slot is not 4 bytes wide the order was the wrong one: a
//      `double` (an 8-byte slot) was ranked by the low 32 bits of the slot —
//      the tail of its mantissa, which is 0 for every "round" value, so
//      `Sort()` on `[1.0, 6.0, 0.25, -3.5, 2.0]` was a NO-OP; a `long` was
//      ranked by its low half; and a `byte` (a ONE-byte slot) was ranked by an
//      i32 load that read THREE BYTES PAST the element — an out-of-bounds
//      read of the array's backing buffer; and (b) even an `int` element was
//      mis-ordered where the difference OVERFLOWS (the extremes are pinned
//      as a wrong-order row of their own).
//   2. A FALSE REJECTION of six element types the stdlib declares
//      `IComparable<Self>`: `sbyte`, `short`, `ushort`, `uint`, `ulong` and
//      `decimal`. The diagnostic's claim — "`uint` does not implement
//      `IComparable<uint>`" — was false.
//
// WHY THE ORACLE IS stdout AND NOT AN EXIT CODE.  The subject is the ORDER of
// the elements; a silent no-op sort and a correct sort both exit 0.  Each row
// PRINTS the post-sort sequence.
//
// WHY `runAotChecked` AND NOT `runAot`.  The `byte` defect is an out-of-bounds
// READ of the array's backing buffer (an i32 load out of a one-byte slot), so
// the sanitizer is a real net for it and not a formality: with the fix the
// comparator reads the element at its own width, and the read is in bounds.
//
// THE CONTROLS pin what the fix must NOT disturb.  Only two are gtest rows
// (owner rule, 2026-11-12: a base-green row is a gtest row only where a corpus
// case cannot express the check; at most two per suite, each named with the
// wrong fix it would catch).  An `int` element, whose order the pre-fix
// comparator already got right for every pair whose i32 DIFFERENCE does not
// overflow (the extremes are pinned as a wrong-order row of their own).  `bool`
// (stdlib §9.4 declares `public record Boolean: IEquatable<bool>` — NO
// `IComparable`) stays REFUSED: it is the admitting twin of the six newly
// admitted primitives, and the family must agree on which primitives conform.
// The `string`, `char`, `float` and `Object` controls are decided by spec
// §14.2 clause 5 and the stdlib declarations, and are corpus candidates (R42
// harvest), not gtest rows here.
//
// ONE ROW IS A BEHAVIOUR FLIP, not a fix row: `Array<int?>.Sort()` was
// ADMITTED before this fix (the primitive arm of the gate ignored
// nullability) and compared each element's NULLABILITY FLAG instead of its
// value.  spec §14.2's constraint table reads `where T: IFoo` as "NON-NULLABLE
// types conforming to `IFoo`", the class arm of the same gate already refused a
// nullable element (the corpus case `s14_2_1901` pins `Array<Guid?>.Sort()` as
// rejected), and the §5.1 `Min()`/`Max()` gate refuses a nullable primitive too
// — so the nullable primitive is now refused as well, and the widening would
// otherwise have admitted `Array<uint?>.Sort()` and its five siblings into the
// same wrong order.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
std::string outChecked(const std::string& program) {
    auto r = runAotChecked(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}
std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}
// Print every element of `v` on ONE line, space-separated, and end the line —
// so a printed sequence is "<elements> \n" and a mis-ordered sequence shows as
// a different string.
std::string printAll(const std::string& v) {
    return "for var e in " + v + " do Console.Write($\"{e} \"); end; " +
           "Console.WriteLine();";
}
// Diagnostic TEXT: a rejection is only pinned when the diagnostic says WHY.
bool rejectsWith(const std::string& program, const std::string& needle) {
    auto r = runAot(program);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}
}  // namespace

// ── HALF 1: the SILENT WRONG ORDER ───────────────────────────────────────────
// The issue's own repro: `double`.  The pre-fix comparator read the low 32
// bits of the 8-byte slot, which for every "round" value is 0 — so the sort
// did nothing at all.
BEZEL_TEST(array_sort_primitive_order_8467, double_sort_orders_by_value) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<double> := [1.0, 6.0, 0.25, -3.5, 2.0]; "
            "a.Sort(); " + printAll("a"))),
        std::string("-3.5 0.25 1 2 6 \n"));
}

// The issue's own repro: `long`.  The pre-fix comparator ranked a 64-bit
// element by its low 32 bits.
BEZEL_TEST(array_sort_primitive_order_8467, long_sort_orders_by_value) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<long> := [3000000000L, 1L, 2L, -7000000000L, "
            "4294967296L]; "
            "a.Sort(); " + printAll("a"))),
        std::string("-7000000000 1 2 3000000000 4294967296 \n"));
}

// The issue's own repro: `byte`.  The pre-fix comparator read an i32 out of a
// ONE-byte slot — three bytes past the element.
BEZEL_TEST(array_sort_primitive_order_8467, byte_sort_orders_by_value) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<byte> := [3, 1, 100, 2]; "
            "a.Sort(); " + printAll("a"))),
        std::string("1 2 3 100 \n"));
}

// The `Sorted()` copy-form is the TWIN of `Sort()` and shares the same stdlib
// §4.4 constraint, so it takes the same gate and must impose the same order —
// and it must leave the RECEIVER unchanged (stdlib §4.4: "the past-participle
// form (`Reversed`, `Sorted`) leaves the receiver unchanged and returns a
// fresh `owned Array<T>`").
BEZEL_TEST(array_sort_primitive_order_8467, sorted_copy_form_orders_by_value) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<double> := [1.0, 6.0, 0.25, -3.5, 2.0]; "
            "var s := a.Sorted(); " + printAll("s") + printAll("a"))),
        std::string("-3.5 0.25 1 2 6 \n1 6 0.25 -3.5 2 \n"));
}

// The `Sorted()` twin for `long`, whose 8-byte slot the pre-fix comparator
// read by its low half.
BEZEL_TEST(array_sort_primitive_order_8467, sorted_copy_form_long_orders_by_value) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<long> := [3000000000L, 1L, -7000000000L]; "
            "var s := a.Sorted(); " + printAll("s"))),
        std::string("-7000000000 1 3000000000 \n"));
}

// AN `int` ELEMENT IS NOT A CONTROL: the pre-fix comparator returned the
// i32 DIFFERENCE of the two elements, so at the extremes that difference
// OVERFLOWS and the order inverts — `int.MaxValue` ranked BELOW
// `int.MinValue`, and both ranked above `0`.  MEASURED on the base
// (`r30-c1`): `[2147483647, -2147483648, 0]` sorted to
// `2147483647 -2147483648 0`.  The new comparator returns -1 / 0 / +1
// (stdlib §2.8), so the difference cannot overflow at all.
BEZEL_TEST(array_sort_primitive_order_8467, int_sort_extremes_are_ordered) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<int> := [2147483647, -2147483648, 0]; "
            "a.Sort(); " + printAll("a"))),
        std::string("-2147483648 0 2147483647 \n"));
}

// ── HALF 2: the FALSE REJECTION ──────────────────────────────────────────────
// stdlib §2.8 / stdlib §9.1: `UInt32` conforms to `IComparable<uint>`, so the
// gate must admit the element — and the order must be UNSIGNED (a `uint`
// compares as the unsigned integer it is: 4000000000 is the LARGEST of the
// five, not the smallest).
BEZEL_TEST(array_sort_primitive_order_8467, uint_sort_is_admitted_and_ordered) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<uint> := [3, 1, 4000000000U, 2, 7]; "
            "a.Sort(); " + printAll("a"))),
        std::string("1 2 3 7 4000000000 \n"));
}

// stdlib §2.8 / stdlib §9.1: `SByte` conforms to `IComparable<sbyte>` — and it
// is SIGNED, so -1 sorts first (an unsigned compare would rank it last).
BEZEL_TEST(array_sort_primitive_order_8467, sbyte_sort_is_admitted_and_ordered) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<sbyte> := [3, -1, 100, 2]; "
            "a.Sort(); " + printAll("a"))),
        std::string("-1 2 3 100 \n"));
}

// stdlib §2.8 / stdlib §9.1: `Int16` conforms to `IComparable<short>` — signed.
BEZEL_TEST(array_sort_primitive_order_8467, short_sort_is_admitted_and_ordered) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<short> := [30000, -5, 100, 2]; "
            "a.Sort(); " + printAll("a"))),
        std::string("-5 2 100 30000 \n"));
}

// stdlib §2.8 / stdlib §9.1: `UInt16` conforms to `IComparable<ushort>` —
// unsigned.
BEZEL_TEST(array_sort_primitive_order_8467, ushort_sort_is_admitted_and_ordered) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<ushort> := [65535, 3, 1, 2]; "
            "a.Sort(); " + printAll("a"))),
        std::string("1 2 3 65535 \n"));
}

// stdlib §2.8 / stdlib §9.1: `UInt64` conforms to `IComparable<ulong>` — and it
// is UNSIGNED, so 18000000000000000000 is the LAST element, not the first.
BEZEL_TEST(array_sort_primitive_order_8467, ulong_sort_is_admitted_and_ordered) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<ulong> := [3UL, 18000000000000000000UL, 1UL]; "
            "a.Sort(); " + printAll("a"))),
        std::string("1 3 18000000000000000000 \n"));
}

// stdlib §2.8 / stdlib §9.3: `Decimal` conforms to `IComparable<decimal>` — and
// `decimal`'s order is its own exact arithmetic order, not an integer read of
// its bytes.
BEZEL_TEST(array_sort_primitive_order_8467, decimal_sort_is_admitted_and_ordered) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<decimal> := [3.5m, 1.25m, 2.0m, -7.75m, 100.125m]; "
            "a.Sort(); " + printAll("a"))),
        std::string("-7.75 1.25 2 3.5 100.125 \n"));
}

// The `Sorted()` twin of the `uint` admission — the copy-form shares the
// stdlib §4.4 constraint and so shares the gate.
BEZEL_TEST(array_sort_primitive_order_8467, sorted_copy_form_uint_is_admitted_and_ordered) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<uint> := [3, 1, 4000000000U, 2, 7]; "
            "var s := a.Sorted(); " + printAll("s"))),
        std::string("1 2 3 7 4000000000 \n"));
}

// ── CONTROLS: the shapes the fix must NOT disturb (two gtest rows) ───────────
// An `int` element: the pre-fix comparator already ordered it correctly (for
// every pair whose difference does not overflow — the extremes are pinned as a
// wrong-order row of their own), and the new one must agree.  It catches a fix
// that reorders the 32-bit path: a comparator whose sign is inverted sorts
// [3, 1, 2] to `3 2 1` and goes RED here.
BEZEL_TEST(array_sort_primitive_order_8467, control_int_sort_still_ordered) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<int> := [3, 1, 2]; "
            "a.Sort(); " + printAll("a"))),
        std::string("1 2 3 \n"));
}

// `bool` (stdlib §9.4 declares `public record Boolean: IEquatable<bool>` — NO
// `IComparable`) stays REFUSED: it is the admitting twin of the six newly
// admitted primitives, and the family must agree on which primitives conform.
// It catches a fix that drops the spec §14.2 constraint gate: with the gate
// gone `bool` is admitted, compiles, and this rejection row goes RED.
BEZEL_TEST(array_sort_primitive_order_8467, control_bool_sort_is_still_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("var a: Array<bool> := [true, false]; a.Sort(); "),
        "Array<bool>.Sort() requires T: IComparable<T> (spec §14.2, stdlib §4.4, "
        "v0.355); bool does not implement IComparable<bool>"));
}

// THE FLIP: a NULLABLE primitive element is refused, as spec §14.2's
// constraint table requires ("NON-NULLABLE types conforming to `IFoo`") and as
// the class arm of the same gate already required (the corpus case
// `s14_2_1901` pins `Array<Guid?>.Sort()` as rejected).  Before this fix the
// primitive arm ignored nullability, so `Array<int?>.Sort()` was admitted and
// compared each element's NULLABILITY FLAG instead of its value.
BEZEL_TEST(array_sort_primitive_order_8467, nullable_int_sort_is_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("var a: Array<int?> := [3, 1, 2]; a.Sort(); "),
        "Array<int?>.Sort() requires T: IComparable<T> (spec §14.2, stdlib §4.4, "
        "v0.355); int? does not implement IComparable<int?>"));
}
