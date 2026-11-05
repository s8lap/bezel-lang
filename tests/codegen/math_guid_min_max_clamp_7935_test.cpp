// Regression tests for issue #7935 — `Math.Max(g1, g2)` / `Math.Min` /
// `Math.Clamp` over the built-in `Guid` was REFUSED with
//
//   "type argument `Guid` does not satisfy the constraint `where T:
//    IComparable<T>` — `Math.Max` requires an argument type that implements
//    `IComparable<T>` (§14.2, stdlib §2.2)"
//
// WHAT THE DOCUMENTS REQUIRE (v0.355).
//   * stdlib §2.6 declares
//         public record Guid: IComparable<Guid>, IEquatable<Guid>
//     with "CompareTo = lexicographic over the 16 canonical big-endian bytes
//     (stated explicitly — .NET's field-group order is a known trap); ordering
//     is CompareTo only — no <,>,<=,>= overloads are declared".
//   * stdlib §2.2 declares `Min<T>(a, b): T`, `Max<T>(a, b): T` and
//     `Clamp<T>(value, low, high): T`, each `where T: IComparable<T>`, and
//     fixes the ORDER they compute: "Ordering throughout is `T`'s `CompareTo`
//     (§2.8). `Min(a, b)` returns `a` when `a.CompareTo(b) <= 0` and `b`
//     otherwise; `Max(a, b)` returns `a` when `a.CompareTo(b) >= 0` and `b`
//     otherwise — so a tie returns `a` in both … `Clamp(value, low, high)`
//     returns `low` when `value` compares less than `low`, `high` when it
//     compares greater than `high`, and `value` otherwise."
//   * stdlib §2.8: `a.CompareTo(b)` returns a negative number if `a < b`, zero
//     if equal, a positive number if `a > b`.
//   * spec §14.2 admits the record that implements the bound; spec §14.5
//     requires that "all arguments for a type parameter must agree".
// So `T = Guid` satisfies the constraint: the three calls MUST be admitted,
// and the values they yield MUST be ordered by §2.6's canonical-byte rule.
//
// THE DEFECT. `Guid` is a BUILT-IN value record: it carries no user
// `TypeDecl`, so its TypeRef has no `baseTypeRefs` and sema's constraint gate
// — whose class/record/interface arms all read `baseTypeRefs` — never saw the
// `IComparable<Guid>` conformance. It is the same shape #7900 fixed for
// stdlib §2.5's temporal records and the shape #8237 fixed for stdlib §4.4's
// `Array<T>.Sort()`.
//
// THE SECOND HALF OF THE FIX IS THE ORDER, and it is the half a "just admit
// it" fix gets wrong. A `Guid` is stored INLINE as a 16-byte `{i64,i64}`
// struct (§2.6): it is neither an integer nor a pointer, so the compiler's
// numeric arms decline it and its default comparator (`bz$intcmp`, an i32
// LOAD out of the element slot) would order a pair by the LOW HALF of the
// first word — a wrong order, silently, on a program that compiles and runs.
// §2.6's order is over the 16 CANONICAL bytes, which is exactly what the
// runtime's `bz_guid_compare` computes (`memcmp` over the 16 raw bytes,
// compared UNSIGNED; the canonical bytes ARE the struct's memory order).
//
// So the rows below pin the ORDER, not merely the compilation:
//   * `order_is_over_the_canonical_bytes_not_the_little_endian_field_order`
//     uses a pair whose canonical order and whose LITTLE-ENDIAN field order
//     (the order .NET uses, and the one stdlib §2.6 names as "a known trap")
//     are OPPOSITE.
//   * `bytes_compare_unsigned` pins `memcmp`'s unsigned byte comparison
//     (0x80 sorts ABOVE 0x7f — the rule the conformance corpus pins for
//     `Guid.Min`/`Max` in tests/conformance/rules/cases/l5_0062.bzl).
//   * `the_math_order_agrees_with_the_compare_to_member` pins that the order
//     `Math.Max` imposes IS the order `Guid.CompareTo` reports.
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

// The program's stdout, one canonical §2.6 rendering per line, each
// terminated by a newline.
std::string lines(std::initializer_list<std::string> parts) {
    std::string s;
    for (const auto& p : parts) {
        s += p;
        s += "\n";
    }
    return s;
}

// True when the program was REJECTED with a diagnostic carrying `needle`.
// The needle is asserted, not just "something was reported": a rejection for
// an unrelated reason would pass a bare `!r.ran` assertion vacuously.
bool rejectedWith(const std::string& program, const std::string& needle) {
    auto r = runAotChecked(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// =====================================================================
// ROW 1 — THE ISSUE'S OWN REPRO, with FIXED Guid values.
//
// The issue's repro uses `Guid.NewGuid()` — RANDOM, so it cannot pin WHICH
// of the two operands `Max` returns. The fixed pair below answers the
// question the issue asks: `Math.Max` returns the greater of the two by
// stdlib §2.6's order.
// =====================================================================
BEZEL_TEST(math_guid_min_max_clamp_7935,
           issue_repro_max_returns_the_greater_guid) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var g1 := Guid.Parse(\"11111111-1111-1111-1111-111111111111\"); "
            "var g2 := Guid.Parse(\"22222222-2222-2222-2222-222222222222\"); "
            "Console.WriteLine(Math.Max(g1, g2).ToString());")),
        lines({"22222222-2222-2222-2222-222222222222"}));
}

// `Min` is the other direction of the SAME order (§2.2: "`Min(a, b)` returns
// `a` when `a.CompareTo(b) <= 0` and `b` otherwise").
BEZEL_TEST(math_guid_min_max_clamp_7935, min_returns_the_lesser_guid) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var g1 := Guid.Parse(\"11111111-1111-1111-1111-111111111111\"); "
            "var g2 := Guid.Parse(\"22222222-2222-2222-2222-222222222222\"); "
            "Console.WriteLine(Math.Min(g1, g2).ToString());")),
        lines({"11111111-1111-1111-1111-111111111111"}));
}

// stdlib §2.2's `Clamp` order, all three positions: "`Clamp(value, low, high)`
// returns `low` when `value` compares less than `low`, `high` when it
// compares greater than `high`, and `value` otherwise".
BEZEL_TEST(math_guid_min_max_clamp_7935,
           clamp_returns_low_below_high_above_and_value_inside) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var lo := Guid.Parse(\"11111111-1111-1111-1111-111111111111\"); "
            "var mid := Guid.Parse(\"22222222-2222-2222-2222-222222222222\"); "
            "var hi := Guid.Parse(\"33333333-3333-3333-3333-333333333333\"); "
            "var below := Guid.Parse(\"00000000-0000-0000-0000-000000000000\"); "
            "var above := Guid.Parse(\"44444444-4444-4444-4444-444444444444\"); "
            "Console.WriteLine(Math.Clamp(below, lo, hi).ToString()); "
            "Console.WriteLine(Math.Clamp(mid, lo, hi).ToString()); "
            "Console.WriteLine(Math.Clamp(above, lo, hi).ToString());")),
        lines({"11111111-1111-1111-1111-111111111111",
               "22222222-2222-2222-2222-222222222222",
               "33333333-3333-3333-3333-333333333333"}));
}

// stdlib §2.6's order is over the 16 CANONICAL bytes, and this pair separates
// it from the LITTLE-ENDIAN FIELD order — the order .NET uses, and the one
// stdlib §2.6 names as "a known trap":
//
//   A = 01000000-0000-0000-0000-000000000000  (canonical byte 0 = 0x01)
//   B = 00000001-0000-0000-0000-000000000000  (canonical byte 3 = 0x01)
//
// Canonically, A > B — the FIRST canonical byte decides (0x01 > 0x00), so
// `Max` is A and `Min` is B. But the first four canonical bytes read as a
// LITTLE-ENDIAN int32 are 1 for A and 0x01000000 for B, so a field-wise
// comparator (or the compiler's `bz$intcmp` fallback, an i32 LOAD out of the
// element slot) answers the REVERSE. This is the row that separates §2.6's
// order from every word-wise order.
BEZEL_TEST(math_guid_min_max_clamp_7935,
           order_is_over_the_canonical_bytes_not_the_little_endian_field_order) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a := Guid.Parse(\"01000000-0000-0000-0000-000000000000\"); "
            "var b := Guid.Parse(\"00000001-0000-0000-0000-000000000000\"); "
            "Console.WriteLine(Math.Max(a, b).ToString()); "
            "Console.WriteLine(Math.Min(a, b).ToString());")),
        lines({"01000000-0000-0000-0000-000000000000",
               "00000001-0000-0000-0000-000000000000"}));
}

// The canonical bytes are compared as UNSIGNED (the rule the conformance
// corpus pins for `Guid.Min`/`Max`, l5_0062): 0x80 sorts ABOVE 0x7f. A
// signed comparison of the first byte would put 0x80 first.
BEZEL_TEST(math_guid_min_max_clamp_7935, bytes_compare_unsigned) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a := Guid.Parse(\"80000000-0000-0000-0000-000000000000\"); "
            "var b := Guid.Parse(\"7fffffff-ffff-ffff-ffff-ffffffffffff\"); "
            "Console.WriteLine(Math.Max(a, b).ToString()); "
            "Console.WriteLine(Math.Min(a, b).ToString());")),
        lines({"80000000-0000-0000-0000-000000000000",
               "7fffffff-ffff-ffff-ffff-ffffffffffff"}));
}

// stdlib §2.2: "a tie returns `a` in both" — `Min` and `Max` of two EQUAL
// Guids answer that Guid (and, since a `Guid` is a VALUE, the tie is not
// observable as an identity; the row pins that the call is admitted and
// computes rather than refusing).
BEZEL_TEST(math_guid_min_max_clamp_7935, equal_guids_yield_that_guid) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var g := Guid.Parse(\"11111111-1111-1111-1111-111111111111\"); "
            "var h := Guid.Parse(\"11111111-1111-1111-1111-111111111111\"); "
            "Console.WriteLine(Math.Max(g, h).ToString()); "
            "Console.WriteLine(Math.Min(g, h).ToString()); "
            "Console.WriteLine(Math.Clamp(g, g, h).ToString());")),
        lines({"11111111-1111-1111-1111-111111111111",
               "11111111-1111-1111-1111-111111111111",
               "11111111-1111-1111-1111-111111111111"}));
}

// An EXPLICIT type list binds T (stdlib §2.2 declares ONE type parameter for
// the whole call), so `Math.Max<Guid>(a, b)` is the same call with the
// constraint checked at the instantiation (§14.1).
BEZEL_TEST(math_guid_min_max_clamp_7935,
           explicit_type_argument_list_binds_t) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var g1 := Guid.Parse(\"11111111-1111-1111-1111-111111111111\"); "
            "var g2 := Guid.Parse(\"22222222-2222-2222-2222-222222222222\"); "
            "Console.WriteLine(Math.Max<Guid>(g1, g2).ToString()); "
            "Console.WriteLine(Math.Min<Guid>(g1, g2).ToString()); "
            "Console.WriteLine(Math.Clamp<Guid>(g1, g1, g2).ToString());")),
        lines({"22222222-2222-2222-2222-222222222222",
               "11111111-1111-1111-1111-111111111111",
               "11111111-1111-1111-1111-111111111111"}));
}

// The order `Math.Max` imposes IS the order `Guid.CompareTo` reports (§2.6:
// "ordering is CompareTo only"). `bz_guid_compare` is the very comparison
// `Guid.CompareTo` dispatches to, so the two cannot drift; this row pins it.
BEZEL_TEST(math_guid_min_max_clamp_7935,
           the_math_order_agrees_with_the_compare_to_member) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var g1 := Guid.Parse(\"11111111-1111-1111-1111-111111111111\"); "
            "var g2 := Guid.Parse(\"22222222-2222-2222-2222-222222222222\"); "
            "Console.WriteLine(Math.Max(g1, g2).CompareTo(g1)); "
            "Console.WriteLine(Math.Max(g1, g2).CompareTo(g2)); "
            "Console.WriteLine(Math.Min(g1, g2).CompareTo(g1)); "
            "Console.WriteLine(Math.Min(g1, g2).CompareTo(g2));")),
        std::string("1\n0\n0\n-1\n"));
}

// =====================================================================
// CONTROLS — the fix must not have changed anything else.
// =====================================================================

// CONTROL — `Math.Max` over INTEGERS is unchanged (stdlib §2.2's numeric
// path): the greatest of the two, 7.
BEZEL_TEST(math_guid_min_max_clamp_7935, control_max_over_ints_is_unchanged) {
    BEZEL_EXPECT_EQ(outChecked(prog("Console.WriteLine(Math.Max(3, 7));")),
                    std::string("7\n"));
}

// CONTROL — `Math.Max` / `Math.Min` over the stdlib §2.5 temporal records
// (admitted by #7900) are unchanged: the later / the earlier `DateTime`.
BEZEL_TEST(math_guid_min_max_clamp_7935,
           control_max_over_datetimes_is_unchanged) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a := DateTime.Parse(\"2020-01-02T03:04:05\"); "
            "var b := DateTime.Parse(\"2021-01-02T03:04:05\"); "
            "Console.WriteLine(Math.Max(a, b).Ticks); "
            "Console.WriteLine(Math.Min(a, b).Ticks);")),
        std::string("637451534450000000\n637135310450000000\n"));
}

// CONTROL — a type that implements NO `IComparable` is STILL REFUSED. The
// gate must not have been widened past the built-in `Guid`.
BEZEL_TEST(math_guid_min_max_clamp_7935,
           control_non_comparable_record_is_still_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; public record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "var a := new Pt(1, 2); var b := new Pt(3, 4); "
        "var m := Math.Max(a, b); "
        "return 0; end Main; end C;",
        "type argument `Pt` does not satisfy the constraint"));
}

// CONTROL — a USER record named `Guid` is NOT the built-in: it is judged by
// its OWN declaration (here: it implements `IComparable<Guid>`, so it is
// admitted and ordered by ITS OWN `CompareTo`, which is a different order
// from the built-in's canonical-byte one).
BEZEL_TEST(math_guid_min_max_clamp_7935,
           control_a_user_record_named_guid_is_not_the_builtin) {
    BEZEL_EXPECT_EQ(
        outChecked(
            "module M; public record Guid(X: int): IComparable<Guid> "
            "  public method CompareTo(other: Guid): int "
            "    if X < other.X then return -1; end if; "
            "    if X > other.X then return 1; end if; "
            "    return 0; "
            "  end CompareTo; "
            "end Guid; "
            "class C public static method Main(): int "
            "var a := new Guid(1); var b := new Guid(3); "
            "Console.WriteLine(Math.Max(a, b).X); "
            "return 0; end Main; end C;"),
        std::string("3\n"));
    // The same shape WITHOUT the conformance is still refused.
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; public record Guid(X: int); "
        "class C public static method Main(): int "
        "var a := new Guid(1); var b := new Guid(3); "
        "var m := Math.Max(a, b); "
        "return 0; end Main; end C;",
        "type argument `Guid` does not satisfy the constraint"));
}

// CONTROL — a `Guid` beside a PRIMITIVE is a §14.5 disagreement ("all
// arguments for a type parameter must agree"), and the MIRROR-IMAGE
// spelling (`Math.Max(1, guid)`) is the one that used to ABORT the compiler
// inside the LLVM verifier ("Both operands to ICmp instruction are not of the
// same type!") instead of being rejected. Both spellings must be REJECTED
// with the §14.5 diagnostic now.
BEZEL_TEST(math_guid_min_max_clamp_7935,
           control_a_guid_beside_a_primitive_is_rejected) {
    const std::string needle =
        "inconsistent type-argument inference: type parameter `T` of "
        "`Math.Max` is `Guid`";
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C public static method Main(): int "
        "var g := Guid.Parse(\"11111111-1111-1111-1111-111111111111\"); "
        "var m := Math.Max(1, g); "
        "return 0; end Main; end C;",
        needle));
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C public static method Main(): int "
        "var g := Guid.Parse(\"11111111-1111-1111-1111-111111111111\"); "
        "var m := Math.Max(g, 1); "
        "return 0; end Main; end C;",
        needle));
}

// CONTROL — a NULLABLE `Guid` never satisfies a type bound (§14.2: "a
// type-bound constraint — naming a class or interface — implies non-null T"),
// so it is still refused.
BEZEL_TEST(math_guid_min_max_clamp_7935,
           control_a_nullable_guid_is_still_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C public static method Main(): int "
        "var g: Guid? := Guid.ParseOrNull(\"11111111-1111-1111-1111-111111111111\"); "
        "var h: Guid? := Guid.ParseOrNull(\"22222222-2222-2222-2222-222222222222\"); "
        "var m := Math.Max(g, h); "
        "return 0; end Main; end C;",
        "nullable"));
}
