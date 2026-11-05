// Regression tests for issue #8237 — `Array<Guid>.Sort()` / `Sorted()` was
// REFUSED with
//
//   "Array<Guid>.Sort() requires T: IComparable<T> (spec §14.2, stdlib §4.4,
//    v0.276); Guid does not implement IComparable<Guid>. Use Sort(comparer:
//    (Guid, Guid) -> int) instead."
//
// WHAT THE DOCUMENTS REQUIRE (v0.355).
//   * stdlib §2.6 declares
//         public record Guid: IComparable<Guid>, IEquatable<Guid>
//     with "CompareTo = lexicographic over the 16 canonical big-endian bytes
//     (stated explicitly — .NET's field-group order is a known trap); ordering
//     is CompareTo only — no <,>,<=,>= overloads are declared".
//   * stdlib §4.4 declares `Array<T>.Sort()` / `Sorted()` `where T:
//     IComparable<T>`, checked at the call's instantiation.
//   * spec §14.2 admits "records and enums implementing it" for a
//     `where T: IFoo` bound.
// So `T = Guid` satisfies the constraint and both calls MUST be admitted, and
// the order they impose MUST be §2.6's.
//
// THE DEFECT. `Guid` is a BUILT-IN value record: it carries no user
// `TypeDecl`, so its TypeRef has no `baseTypeRefs` and sema's constraint gate
// — whose class/record/interface arms all read `baseTypeRefs` — never saw the
// `IComparable<Guid>` conformance. The same shape was fixed for stdlib §2.5's
// temporal records by #7766 (`Array<DateTime>.Sort()`).
//
// THE SECOND HALF OF THE FIX IS THE ORDER, and it is the half a
// "just admit it" fix gets wrong. A `Guid` is stored INLINE as a 16-byte
// `{i64,i64}` struct (§2.6), so the compiler's default comparator for an
// unrecognised element (`bz$intcmp`, an i32 LOAD out of the element slot)
// would order the array by the LOW HALF of the Guid's first word — a wrong
// order, silently, on a program that compiles and runs. §2.6's order is over
// the 16 CANONICAL bytes, which is exactly what the runtime's
// `bz_guid_compare` computes (`memcmp` over the 16 raw bytes; the canonical
// bytes ARE the struct's memory order — the parse path memcpy's them in).
//
// So the rows below pin the ORDER, not merely the compilation:
//   * `order_is_over_the_canonical_bytes_not_a_word` uses a pair whose
//     canonical order and whose LOW-WORD order are OPPOSITE — a word-wise (or
//     i32-wise) comparator answers the reverse of §2.6.
//   * `bytes_compare_unsigned` pins `memcmp`'s unsigned byte comparison
//     (0x80 above 0x7f — the same rule the conformance corpus pins for
//     `Guid.Min`/`Max` in tests/conformance/rules/cases/l5_0062.bzl).
//   * `sort_agrees_with_the_compare_to_member` pins that the order the sort
//     imposes IS the order `Guid.CompareTo` reports.
//
// A REFUSED program yields the harness's "<did-not-run>" sentinel, which no
// expected string matches — so the refusal is observed as a FAILURE of the
// positive rows.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <initializer_list>
#include <string>

using bezel::test::runAotChecked;

namespace {

// The program's stdout, verbatim. "<did-not-run>" when the program was
// rejected, crashed or timed out — the value a REFUSED program produces here,
// and the reason every positive row below fails before the fix.
std::string outChecked(const std::string& program) {
    auto r = runAotChecked(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

// The canonical §2.6 renderings, in the order §2.6's rule puts them — one
// per line, each terminated by a newline (the program's stdout).
std::string lines(std::initializer_list<std::string> parts) {
    std::string s;
    for (const auto& p : parts) {
        s += p;
        s += "\n";
    }
    return s;
}

}  // namespace

// #8237 / stdlib §2.6 + §4.4 / spec §14.2: the issue's own repro, with FIXED
// Guid values (the issue's `NewGuid()` is random, so it cannot pin an order).
// `Sort()` sorts IN PLACE and yields no value.
BEZEL_TEST(array_guid_sort_8237, issue_repro_sorts_fixed_guids) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<Guid> := "
            "[Guid.Parse(\"22222222-2222-2222-2222-222222222222\"), "
            "Guid.Parse(\"11111111-1111-1111-1111-111111111111\"), "
            "Guid.Parse(\"33333333-3333-3333-3333-333333333333\")]; "
            "a.Sort(); "
            "for var x in a do Console.WriteLine(x.ToString()); end;")),
        lines({"11111111-1111-1111-1111-111111111111",
               "22222222-2222-2222-2222-222222222222",
               "33333333-3333-3333-3333-333333333333"}));
}

// `Sorted()` is the COPY form: it returns a new array and leaves the source
// alone. Both forms carry the same §4.4 constraint, so both are admitted and
// both impose §2.6's order.
BEZEL_TEST(array_guid_sort_8237, sorted_copy_orders_by_canonical_bytes_and_keeps_source) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<Guid> := "
            "[Guid.Parse(\"22222222-2222-2222-2222-222222222222\"), "
            "Guid.Parse(\"11111111-1111-1111-1111-111111111111\"), "
            "Guid.Parse(\"33333333-3333-3333-3333-333333333333\")]; "
            "var s := a.Sorted(); "
            "for var x in s do Console.WriteLine(x.ToString()); end; "
            "Console.WriteLine(a[0].ToString());")),
        lines({"11111111-1111-1111-1111-111111111111",
               "22222222-2222-2222-2222-222222222222",
               "33333333-3333-3333-3333-333333333333",
               "22222222-2222-2222-2222-222222222222"}));
}

// §2.6's order is over the 16 CANONICAL BYTES, and this pair separates it
// from every WORD-wise order:
//
//   A = 01000000-0000-0000-0000-000000000000  (byte 0 = 0x01, rest 0)
//   B = 00000000-0000-0000-0000-000000000001  (byte 15 = 0x01, rest 0)
//
// Canonically, A > B (byte 0 decides: 0x01 > 0x00), so the sorted order is
// B, A. But A's LOW WORD (bytes 0..7) is 1 and B's is 0 — so a comparator
// that read the low word (or the low i32) would answer A, B: the REVERSE.
// This is the row that separates §2.6's order from the compiler's default
// `bz$intcmp` (an i32 LOAD out of the element slot).
BEZEL_TEST(array_guid_sort_8237, order_is_over_the_canonical_bytes_not_a_word) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<Guid> := "
            "[Guid.Parse(\"01000000-0000-0000-0000-000000000000\"), "
            "Guid.Parse(\"00000000-0000-0000-0000-000000000001\")]; "
            "a.Sort(); "
            "for var x in a do Console.WriteLine(x.ToString()); end;")),
        lines({"00000000-0000-0000-0000-000000000001",
               "01000000-0000-0000-0000-000000000000"}));
}

// The canonical bytes are compared as UNSIGNED (the rule the conformance
// corpus pins for `Guid.Min`/`Max`, l5_0062): 0x80 sorts ABOVE 0x7f. A
// signed comparison of the first byte would put 0x80 first.
BEZEL_TEST(array_guid_sort_8237, bytes_compare_unsigned) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<Guid> := "
            "[Guid.Parse(\"80000000-0000-0000-0000-000000000000\"), "
            "Guid.Parse(\"7fffffff-ffff-ffff-ffff-ffffffffffff\")]; "
            "a.Sort(); "
            "for var x in a do Console.WriteLine(x.ToString()); end;")),
        lines({"7fffffff-ffff-ffff-ffff-ffffffffffff",
               "80000000-0000-0000-0000-000000000000"}));
}

// The order the sort imposes IS the order `Guid.CompareTo` reports (§2.6:
// "ordering is CompareTo only"). `bz_guid_compare` is the very comparison
// `Guid.CompareTo` dispatches to, so the two cannot drift; this row pins it.
BEZEL_TEST(array_guid_sort_8237, sort_agrees_with_the_compare_to_member) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<Guid> := "
            "[Guid.Parse(\"33333333-3333-3333-3333-333333333333\"), "
            "Guid.Parse(\"11111111-1111-1111-1111-111111111111\"), "
            "Guid.Parse(\"22222222-2222-2222-2222-222222222222\")]; "
            "a.Sort(); "
            "Console.WriteLine(a[0].CompareTo(a[1])); "
            "Console.WriteLine(a[1].CompareTo(a[2]));")),
        std::string("-1\n-1\n"));
}

// CONTROL — an element type that implements NO `IComparable` is STILL
// REFUSED. The gate must not have been widened past the built-in `Guid`.
BEZEL_TEST(array_guid_sort_8237, control_non_comparable_record_is_still_rejected) {
    auto r = runAotChecked(
        "module M; public record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "var a: Array<Pt> := [new Pt(1, 2)]; "
        "a.Sort(); "
        "return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool saw = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("Pt does not implement IComparable<Pt>") !=
            std::string::npos)
            saw = true;
    BEZEL_EXPECT_TRUE(saw);
}
