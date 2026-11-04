// Regression tests for issue #8108: `Array<H>.Min/Max(lambda(x: H) => x.Dec)`
// over a RECORD that carries a `decimal` field returned a GARBAGE value (or
// crashed): the decimal-selector arm loaded the ELEMENT as the selector's
// RESULT type and called the closure with that type, so the callee received a
// truncated element — or a wild pointer — instead of the element.
//
// WHAT THE DOCUMENTS REQUIRE (stdlib §5.1, v0.355, this tree):
//
//     public method Min<U>(selector: (T) -> U): U where U: IComparable<U>;
//     public method Max<U>(selector: (T) -> U): U where U: IComparable<U>;
//
// so the RESULT of the selector form is the SELECTED VALUE — the projected
// `decimal` — ordered by the `decimal`'s own order, and (stdlib §5, the
// empty-source rule) "Min/Max — selector and no-selector forms alike — throw
// InvalidOperationException on an empty source".
//
// THE DEFECT (MEASURED on r30-c1 @ f132ac82 — this lane's own fresh build,
// the issue's repro verbatim):
//
//     direct=2.5
//     min=107374182400
//     max=107374182400
//
// The `decimal` arm (`decSelectorMM`) loaded the element with the RESULT type
// — a 16-byte `%bz.decimal` — and gave the CLOSURE that type as its
// parameter. The closure's parameter is the ELEMENT (`lambda(x: T) => …`),
// so for any element that is not itself a `decimal` the callee read a
// truncated element (the record's first 16 bytes) or a wild pointer, and the
// projected value was garbage. The GENERIC selector arm — the same call with
// an `int` result, on the SAME record — was CORRECT (`min=1`, `max=2`),
// because it loads the element at the element's own type; that isolates the
// defect to the decimal arm and to the LAYOUT of the record, not to the
// record's field order as such.
//
// ⇒ the fix loads the element — and types the closure's parameter — at the
// ELEMENT's own LLVM type, the discipline the generic arm already follows.
//
// ORACLE. `runAot` / `runAotChecked` (the AOT path). Every row asserts the
// EXACT STDOUT — a VALUE oracle, never "no diagnostic was produced" — so a
// legal `Min`/`Max` over a `decimal` selector must compile and print the
// value stdlib §5.1's ordering selects.
//
// ⚠ THE ROWS ARE NOT ALL THE SAME SHAPE: the `decimal` field's OFFSET inside
// the record changes with the field order (`decimal` after a `long`, after an
// `int`, first), and the ELEMENT KIND changes with the record/class choice
// (a record element is stored INLINE, a class element is a POINTER). A fix
// that only worked for one of them would pass a single-layout test.

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

// The same, with the GENERATED code run under AddressSanitizer/LSan: the
// selector arm LOADS the element out of the array buffer and hands it to the
// closure, so a wrongly-sized or out-of-bounds load is a MEMORY ERROR rather
// than a wrong number. (The elements are value types, so the program owns no
// heap object and LSan has nothing to report.)
std::string outChecked(const std::string& program) {
    auto r = bezel::test::runAotChecked(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

// `decls` are the module-level type declarations; `body` is Main's body.
std::string prog(const std::string& decls, const std::string& body) {
    return "module M; " + decls +
           " public class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

}  // namespace

// =====================================================================
// ROW 1 — THE ISSUE'S OWN REPRO (its deterministic program, verbatim).
// `record H(L: long, Dec: decimal, N: int)` — the `decimal` sits AFTER a
// `long`, so the field is not at offset 0.
//
// Pre-fix: "direct=2.5" / "min=107374182400" / "max=107374182400".
// Post-fix: "direct=2.5" / "min=-2.5" / "max=2.5".
// =====================================================================
BEZEL_TEST(min_max_decimal_selector_layout_8108,
           the_issue_repro_a_decimal_field_after_a_long) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "public record H(L: long, Dec: decimal, N: int);",
            "var a := new Array<H>(); "
            "a.Add(new H(L := 5L, Dec := 2.5m, N := 1)); "
            "a.Add(new H(L := 6L, Dec := -2.5m, N := 2)); "
            "Console.WriteLine(\"direct=\" + a[0].Dec.ToString()); "
            "Console.WriteLine(\"min=\" + "
            "a.Min(lambda(x: H) => x.Dec).ToString()); "
            "Console.WriteLine(\"max=\" + "
            "a.Max(lambda(x: H) => x.Dec).ToString()); ")),
        "direct=2.5\n"
        "min=-2.5\n"
        "max=2.5\n");
}

// =====================================================================
// ROW 2 — THE `decimal` AFTER AN `int`. The record's layout is different
// (the `int` is 4 bytes and the `decimal` follows it at offset 4), so a fix
// that read the field at a hard-coded offset would fail here.
//
// Pre-fix: garbage. Post-fix: "min=-2.5" / "max=2.5".
// =====================================================================
BEZEL_TEST(min_max_decimal_selector_layout_8108,
           a_decimal_field_after_an_int) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "public record H(N: int, Dec: decimal);",
            "var a := new Array<H>(); "
            "a.Add(new H(N := 1, Dec := 2.5m)); "
            "a.Add(new H(N := 2, Dec := -2.5m)); "
            "Console.WriteLine(\"direct=\" + a[0].Dec.ToString()); "
            "Console.WriteLine(\"min=\" + "
            "a.Min(lambda(x: H) => x.Dec).ToString()); "
            "Console.WriteLine(\"max=\" + "
            "a.Max(lambda(x: H) => x.Dec).ToString()); ")),
        "direct=2.5\n"
        "min=-2.5\n"
        "max=2.5\n");
}

// =====================================================================
// ROW 3 — THE `decimal` FIRST. The field is at offset 0 of the record, so
// the element's first bytes ARE the `decimal` — the shape a fix that read
// the element's first 16 bytes would get RIGHT by accident.
//
// Pre-fix: garbage. Post-fix: "min=-2.5" / "max=2.5".
// =====================================================================
BEZEL_TEST(min_max_decimal_selector_layout_8108,
           a_decimal_field_first) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "public record H(Dec: decimal, N: int);",
            "var a := new Array<H>(); "
            "a.Add(new H(Dec := 2.5m, N := 1)); "
            "a.Add(new H(Dec := -2.5m, N := 2)); "
            "Console.WriteLine(\"direct=\" + a[0].Dec.ToString()); "
            "Console.WriteLine(\"min=\" + "
            "a.Min(lambda(x: H) => x.Dec).ToString()); "
            "Console.WriteLine(\"max=\" + "
            "a.Max(lambda(x: H) => x.Dec).ToString()); ")),
        "direct=2.5\n"
        "min=-2.5\n"
        "max=2.5\n");
}

// =====================================================================
// ROW 4 — THE `decimal` IN A CLASS ELEMENT. The array's element is a class
// INSTANCE (a POINTER), so the closure's parameter is a pointer and the
// `decimal` is read out of the object. The pre-fix arm handed the closure a
// 16-byte value where it expected a pointer — the callee dereferenced a
// register as an address (a SEGV on some builds, garbage on others).
//
// Pre-fix: garbage or a crash. Post-fix: "min=-2.5" / "max=2.5".
// =====================================================================
BEZEL_TEST(min_max_decimal_selector_layout_8108,
           a_decimal_field_in_a_class_element) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "public class K(d: decimal) "
            "  public property Dec: decimal := d; "
            "end K;",
            "using var k0 := new K(2.5m); "
            "using var k1 := new K(-2.5m); "
            "var a := new Array<K>(); "
            "a.Add(k0); a.Add(k1); "
            "Console.WriteLine(\"direct=\" + a[0].Dec.ToString()); "
            "Console.WriteLine(\"min=\" + "
            "a.Min(lambda(x: K) => x.Dec).ToString()); "
            "Console.WriteLine(\"max=\" + "
            "a.Max(lambda(x: K) => x.Dec).ToString()); ")),
        "direct=2.5\n"
        "min=-2.5\n"
        "max=2.5\n");
}

// =====================================================================
// ROW 4b — THE SAME CLASS ELEMENT UNDER AddressSanitizer. This row is
// the DISCRIMINATING one for the class-element shape: without the fix the
// pre-fix arm handed the closure a 16-byte VALUE where it expected the
// element POINTER, so the callee dereferenced a register as an address —
// MEASURED on the base: "AddressSanitizer: SEGV on unknown address" in
// `lam$0` (the plain `runAot` leg of ROW 4 survives only because the
// loaded value's first 8 bytes happen to BE the element pointer, and it
// reads 8 bytes PAST the 8-byte element slot besides).
//
// Pre-fix: a SEGV under ASan. Post-fix: the exact minimum and maximum.
// =====================================================================
BEZEL_TEST(min_max_decimal_selector_layout_8108,
           a_decimal_field_in_a_class_element_under_asan) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "public class K(d: decimal) "
            "  public property Dec: decimal := d; "
            "end K;",
            "using var k0 := new K(2.5m); "
            "using var k1 := new K(-2.5m); "
            "var a := new Array<K>(); "
            "a.Add(k0); a.Add(k1); "
            "Console.WriteLine(a.Min(lambda(x: K) => x.Dec).ToString()); "
            "Console.WriteLine(a.Max(lambda(x: K) => x.Dec).ToString()); ")),
        "-2.5\n"
        "2.5\n");
}

// =====================================================================
// ROW 5 — NEGATIVE VALUES, AND MORE THAN ONE OF THEM. The pre-fix answer
// was the same garbage for the minimum AND the maximum; with several
// negatives the ordering must still be the `decimal`'s own (stdlib §9.3's
// `decimal` order — a signed value), not the bit pattern.
//
// Pre-fix: garbage. Post-fix: "min=-30.5" / "max=4.25".
// =====================================================================
BEZEL_TEST(min_max_decimal_selector_layout_8108,
           several_negative_values_order_by_value) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "public record H(L: long, Dec: decimal, N: int);",
            "var a := new Array<H>(); "
            "a.Add(new H(L := 1L, Dec := 4.25m, N := 1)); "
            "a.Add(new H(L := 2L, Dec := -30.5m, N := 2)); "
            "a.Add(new H(L := 3L, Dec := -2.5m, N := 3)); "
            "a.Add(new H(L := 4L, Dec := 0m, N := 4)); "
            "Console.WriteLine(\"min=\" + "
            "a.Min(lambda(x: H) => x.Dec).ToString()); "
            "Console.WriteLine(\"max=\" + "
            "a.Max(lambda(x: H) => x.Dec).ToString()); ")),
        "min=-30.5\n"
        "max=4.25\n");
}

// =====================================================================
// ROW 6 — THE SAME UNDER AddressSanitizer. The selector arm LOADS the
// element out of the array buffer and hands it to the closure, so a
// wrongly-sized load is a MEMORY ERROR rather than a wrong number.
//
// Pre-fix: garbage (or a crash). Post-fix: the exact minimum and maximum.
// =====================================================================
BEZEL_TEST(min_max_decimal_selector_layout_8108,
           the_decimal_selector_over_a_record_under_asan) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "public record H(L: long, Dec: decimal, N: int);",
            "var a := new Array<H>(); "
            "a.Add(new H(L := 5L, Dec := 2.5m, N := 1)); "
            "a.Add(new H(L := 6L, Dec := -2.5m, N := 2)); "
            "Console.WriteLine(a.Min(lambda(x: H) => x.Dec).ToString()); "
            "Console.WriteLine(a.Max(lambda(x: H) => x.Dec).ToString()); ")),
        "-2.5\n"
        "2.5\n");
}

// =====================================================================
// ROW 7 — CONTROL: the `decimal` ELEMENT (`Array<decimal>`) is unchanged.
// The element IS a `decimal` there, so the element's type and the result's
// type coincide; the #686/#8048 rows pin this shape and it must stay green.
//
// Pre-fix and post-fix: "1.75" / "9.25".
// =====================================================================
BEZEL_TEST(min_max_decimal_selector_layout_8108,
           control_a_decimal_element_is_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "",
            "var a := new Array<decimal>(); "
            "a.Add(9.25m); a.Add(5.5m); a.Add(1.75m); "
            "Console.WriteLine(a.Min(lambda(x: decimal) => x).ToString()); "
            "Console.WriteLine(a.Max(lambda(x: decimal) => x).ToString()); ")),
        "1.75\n"
        "9.25\n");
}

// =====================================================================
// ROW 8 — CONTROL: the NULLABLE `decimal` ELEMENT (`Array<decimal?>`) is
// unchanged. Its element is `{i8, %bz.decimal}` — the element's own type is
// NOT the result's type here either, and the #686 rows pin this shape.
//
// Pre-fix and post-fix: "2" / "8".
// =====================================================================
BEZEL_TEST(min_max_decimal_selector_layout_8108,
           control_a_nullable_decimal_element_is_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "",
            "var a := new Array<decimal?>(); "
            "a.Add(5m); a.Add(2m); a.Add(8m); "
            "Console.WriteLine(a.Min(lambda(x: decimal?) => x ?? 9999m)"
            ".ToString()); "
            "Console.WriteLine(a.Max(lambda(x: decimal?) => x ?? -9999m)"
            ".ToString()); ")),
        "2\n"
        "8\n");
}

// =====================================================================
// ROW 9 — CONTROL: the GENERIC selector arm over the SAME record is
// unchanged (an `int` projection). It already loaded the element at the
// element's own type, so it was CORRECT before this fix and must stay
// correct after it.
//
// Pre-fix and post-fix: "min=1" / "max=2".
// =====================================================================
BEZEL_TEST(min_max_decimal_selector_layout_8108,
           control_an_int_selector_over_the_same_record_is_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "public record H(L: long, Dec: decimal, N: int);",
            "var a := new Array<H>(); "
            "a.Add(new H(L := 5L, Dec := 2.5m, N := 1)); "
            "a.Add(new H(L := 6L, Dec := -2.5m, N := 2)); "
            "Console.WriteLine(\"min=\" + a.Min(lambda(x: H) => x.N).ToString()); "
            "Console.WriteLine(\"max=\" + a.Max(lambda(x: H) => x.N).ToString()); ")),
        "min=1\n"
        "max=2\n");
}

// =====================================================================
// ROW 10 — CONTROL: the EMPTY SOURCE (stdlib §5's empty-source rule: "Min/Max
// — selector and no-selector forms alike — throw InvalidOperationException
// on an empty source"). The selector arm keeps that guard, so an EMPTY array
// still throws rather than reading element 0.
//
// Pre-fix and post-fix: "True".
// =====================================================================
BEZEL_TEST(min_max_decimal_selector_layout_8108,
           control_an_empty_source_throws_invalid_operation_exception) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "public record H(L: long, Dec: decimal, N: int);",
            "var a := new Array<H>(); "
            "var caught := false; "
            "try "
            "  Console.WriteLine(a.Min(lambda(x: H) => x.Dec).ToString()); "
            "catch InvalidOperationException => caught := true; end; "
            "Console.WriteLine(caught); ")),
        "True\n");
}
