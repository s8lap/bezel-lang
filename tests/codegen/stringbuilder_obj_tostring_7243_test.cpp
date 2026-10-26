// Regression tests for #7243 — the StringBuilder's content reached its
// consumers by TWO routes, and the `Object`-vtable route still rendered it in
// the BYTE domain, so a wide code point was MANGLED there while the direct
// route rendered it correctly.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   spec §4.6 (v0.344), verbatim:
//     "The user-visible model is 'a sequence of code points indexed in O(1).'
//      The internal representation is implementation-defined: an implementation
//      is expected to choose a per-string storage width — typically 1, 2, or 4
//      bytes per code point, selected at construction based on the largest code
//      point in the value — to keep the common case (predominantly-ASCII text)
//      compact while preserving O(1) indexing for every string."
//   and, verbatim:
//     "`s.Length` returns the number of code points in `s`. O(1)."
//     "`s[i]` returns the `char` at code-point position `i`, zero-indexed."
//
//   stdlib §3.1 (v0.344), verbatim:
//     "public override method ToString(): string;"
//   — so a `StringBuilder`'s text reaches the program through `ToString()`,
//   WHICHEVER ROUTE dispatches it. §8.5's rule ("a hole without a format spec
//   formats via the value's ordinary `ToString()`") makes the same demand of
//   the interpolation hole.
//
//   stdlib §9.6 (v0.344), verbatim:
//     "public pure readonly property IsAscii: bool;  // true iff every code
//      point < 128 (O(1))"
//   and spec §4.6 (v0.344) exposes the same predicate. ⇒ THE STORAGE WIDTH IS
//   OBSERVABLE: the runtime answers `IsAscii` from the width in O(1), so a
//   string stored WIDER than its value requires answers `IsAscii` differently
//   from a `=`-equal string stored at its natural width. §4.6's own rule —
//   "selected at construction based on the largest code point in the value" —
//   is therefore the rule the width must follow, and it is the rule the
//   builder's own `ToString()` already followed after #7229.
//
// THE DEFECT (measured; see the handback).
//
//   `bz_sb_obj_tostring` (src/runtime/bezel_runtime_refcounting.cpp) — the
//   `Object` vtable's ToString slot for a StringBuilder — called the LEGACY
//   BYTE-LEVEL `bz_strbuilder_to_string`, which concatenates the parts' RAW
//   BYTES, and then `bz_make_string`, a WIDTH-1 constructor that takes no
//   width. The width was dropped on the floor, so "日" (U+65E5, stored as the
//   two bytes E5 65 at width 2) came back as the TWO code points 229 and 101
//   — while `sb.ToString()` on the SAME builder returned the ONE code point
//   26085. THE TWO ROUTES OF ONE OPERATION DISAGREE.
//
//   The fix: the vtable route renders the content in the CODE-POINT domain at
//   the width the content's largest code point requires, exactly as the direct
//   route does — one runtime entry point that flattens the parts and packs
//   them at the narrowest width that fits (§4.6).
//
// THE ORACLE. stdout, byte-for-byte. A mangled string still exits 0, so the
// exit code alone would be VACUOUS. Every row prints the LENGTH and the code
// points as INTS (so the expectation is pure ASCII and platform-independent),
// the `IsAscii` predicate, and the result of the CONTENT comparison
// `t = "…"` — the strongest form of the assertion, since it pins the content
// and not merely a length.
//
// WHAT EACH ROW PINS ON THE WIDTH (the width is the whole point):
//
//   * a WIDTH-1 control: ASCII content must answer `IsAscii` TRUE, which a
//     width-2 storage of "abc" would answer FALSE. A fix that made every
//     StringBuilder's `ToString()` width-2 is a REGRESSION and this row
//     catches it;
//   * a WIDTH-2 case: U+65E5 cannot be held by a width-1 storage at all (the
//     low byte 229 is a different code point), so the code-point assertion
//     pins the width at 2 or more;
//   * a WIDTH-4 case: U+1F600 (128512) cannot be held by a width-1 or a
//     width-2 storage (they truncate to 0xF600 = 63040), so the code-point
//     assertion pins the width at exactly 4;
//   * an EMPTY builder: the zero-length content, where the width is
//     unobservable but the length and the emptiness are not.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string main_(const std::string& body) {
    return std::string("module M; class C public static method Main(): int ") +
           body + "  return 0; end Main; end C;";
}

}  // namespace

// #7243 / spec §4.6 + stdlib §3.1 (v0.344): the `Object` vtable's ToString
// slot renders the builder's CONTENT. "日" is U+65E5 = 26085, one code point
// at a width of 2. Pre-fix the vtable route returned the two code points 229
// and 101 (the raw bytes E5 65 read one byte per code point) and a length of
// 2; the direct route on the SAME builder returned 26085 and a length of 1.
BEZEL_TEST(codegen_sb_obj_tostring_7243, object_vtable_returns_the_wide_code_point) {
    auto r = runAot(main_(
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"日\"); "
        "  var o: Object := sb; "
        "  var t := o.ToString(); "
        "  Console.WriteLine(t.Length.ToString()); "
        "  Console.WriteLine(int(t[0]).ToString()); "
        "  Console.WriteLine(t.IsAscii.ToString()); "
        "  var eq: bool := t = \"日\"; "
        "  Console.WriteLine(eq.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n26085\nFalse\nTrue\n");
}

// #7243 / spec §4.6 + stdlib §3.1 (v0.344): THE TWO ROUTES OF ONE OPERATION
// MUST AGREE. The builder's own `ToString()` (stdlib §3.1's declared member)
// and the `Object` vtable's ToString slot render the SAME content, so the
// length, the code points and the content comparison must all match. Pre-fix
// the direct route gave 1/26085 and the vtable route 2/229 — the two rows
// disagreed about one operation.
BEZEL_TEST(codegen_sb_obj_tostring_7243, the_two_routes_of_one_operation_agree) {
    auto r = runAot(main_(
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"日\"); "
        "  var o: Object := sb; "
        "  var direct := sb.ToString(); "
        "  var viaObject := o.ToString(); "
        "  Console.WriteLine(direct.Length.ToString()); "
        "  Console.WriteLine(viaObject.Length.ToString()); "
        "  Console.WriteLine((direct.Length = viaObject.Length).ToString()); "
        "  Console.WriteLine((direct[0] = viaObject[0]).ToString()); "
        "  Console.WriteLine((direct = viaObject).ToString()); "
        "  Console.WriteLine(direct.IsAscii.ToString()); "
        "  Console.WriteLine(viaObject.IsAscii.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "1\n1\nTrue\nTrue\nTrue\nFalse\nFalse\n");
}

// #7243 / spec §4.6 + stdlib §9.6 (v0.344) — THE WIDTH-1 CONTROL.
//
// §4.6: the width is "selected at construction based on the largest code
// point in the value"; stdlib §9.6: `IsAscii` is "true iff every code point
// < 128", and the runtime answers it from the width in O(1) (`if (s->width !=
// 1) return 0;`). ⇒ an ASCII content stored at a width of 2 answers FALSE
// while the `=`-equal literal "abc" answers TRUE. A fix that made EVERY
// StringBuilder's `ToString()` width-2 is a REGRESSION, and this row is what
// catches it.
//
// The row therefore asserts `IsAscii` TRUE on both the rendered string and the
// literal it must agree with, plus the length and the content comparison.
BEZEL_TEST(codegen_sb_obj_tostring_7243, ascii_content_is_stored_at_width_one) {
    auto r = runAot(main_(
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"abc\"); "
        "  var o: Object := sb; "
        "  var t := o.ToString(); "
        "  var lit: string := \"abc\"; "
        "  Console.WriteLine(t.Length.ToString()); "
        "  Console.WriteLine(int(t[0]).ToString()); "
        "  Console.WriteLine(int(t[2]).ToString()); "
        "  Console.WriteLine(t.IsAscii.ToString()); "
        "  Console.WriteLine(lit.IsAscii.ToString()); "
        "  var eq: bool := t = lit; "
        "  Console.WriteLine(eq.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n97\n99\nTrue\nTrue\nTrue\n");
}

// #7243 / spec §4.6 + stdlib §9.6 (v0.344): THE EMPTY BUILDER. The content
// is empty: the length is 0, the content compares equal to the empty literal,
// and an empty string is ASCII (there is no code point that is not < 128).
// This row is a CONTROL — it is green on both arms — and it exists to pin the
// zero-length case, where the width is unobservable but the emptiness is not.
BEZEL_TEST(codegen_sb_obj_tostring_7243, empty_builder_renders_the_empty_string) {
    auto r = runAot(main_(
        "  using var sb := new StringBuilder(); "
        "  var o: Object := sb; "
        "  var t := o.ToString(); "
        "  Console.WriteLine(t.Length.ToString()); "
        "  Console.WriteLine(t.IsAscii.ToString()); "
        "  var eq: bool := t = \"\"; "
        "  Console.WriteLine(eq.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\nTrue\nTrue\n");
}

// #7243 / spec §4.5 + §4.6 (v0.344): THE WIDTH-4 CASE. A `char` is a 32-bit
// Unicode scalar value (§4.5) and §4.6 allows "1, 2, or 4 bytes per code
// point", so U+1F600 (128512) is ONE code point stored in FOUR bytes. A
// width-1 or a width-2 storage would truncate it (to 0x60 and to 0xF600 =
// 63040 respectively), so the code-point assertion pins the width at exactly
// 4 — a DIFFERENT code path in the runtime from the width-2 case.
BEZEL_TEST(codegen_sb_obj_tostring_7243, astral_code_point_is_one_code_point) {
    auto r = runAot(main_(
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"😀\"); "
        "  var o: Object := sb; "
        "  var t := o.ToString(); "
        "  Console.WriteLine(t.Length.ToString()); "
        "  Console.WriteLine(int(t[0]).ToString()); "
        "  Console.WriteLine(t.IsAscii.ToString()); "
        "  var eq: bool := t = \"😀\"; "
        "  Console.WriteLine(eq.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n128512\nFalse\nTrue\n");
}

// #7243 / spec §4.6 (v0.344): parts of DIFFERENT widths concatenate as CODE
// POINTS, not as bytes. "a" is width 1, "日" is width 2, "b" is width 1: a
// byte-level concatenation of the three parts' raw bytes is 5 bytes and 5
// code points, not the 3 code points (97, 26085, 98) the content holds. This
// is the row that pins the fix as more than "store width 2": a fix that
// concatenated the parts' bytes and then widened the result would still get
// the content wrong.
BEZEL_TEST(codegen_sb_obj_tostring_7243, mixed_width_parts_concatenate_as_code_points) {
    auto r = runAot(main_(
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"a\"); "
        "  sb.Append(\"日\"); "
        "  sb.Append(\"b\"); "
        "  var o: Object := sb; "
        "  var t := o.ToString(); "
        "  Console.WriteLine(t.Length.ToString()); "
        "  Console.WriteLine(int(t[0]).ToString()); "
        "  Console.WriteLine(int(t[1]).ToString()); "
        "  Console.WriteLine(int(t[2]).ToString()); "
        "  var eq: bool := t = \"a日b\"; "
        "  Console.WriteLine(eq.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n97\n26085\n98\nTrue\n");
}

// #7243 / stdlib §3.1 (v0.344): the vtable route is the one a CALLER's own
// `Object`-typed value takes. `Render(o: Object)` receives the builder
// through an `Object` PARAMETER, so nothing statically narrows the slot to a
// StringBuilder and the call dispatches through the vtable — the same route
// as the local, and the same mangling pre-fix.
BEZEL_TEST(codegen_sb_obj_tostring_7243, object_parameter_route_returns_the_wide_code_point) {
    auto r = runAot(
        "module M; class C "
        "  public static method Render(o: Object): string "
        "    return o.ToString(); "
        "  end Render; "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(); "
        "    sb.Append(\"日\"); "
        "    var t := Render(sb); "
        "    Console.WriteLine(t.Length.ToString()); "
        "    Console.WriteLine(int(t[0]).ToString()); "
        "    var eq: bool := t = \"日\"; "
        "    Console.WriteLine(eq.ToString()); "
        "    return 0; "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n26085\nTrue\n");
}

// #7243 / stdlib §3.1 (v0.344): `Append(value: Object)` is defined as
// "Append(value.ToString())" (stdlib §3.1's sendability note says the same),
// so appending a builder to another builder keeps the TEXT of the first one.
// The appended text must be the content's code points, not its bytes.
BEZEL_TEST(codegen_sb_obj_tostring_7243, append_object_keeps_the_rendered_text) {
    auto r = runAot(main_(
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"日\"); "
        "  var o: Object := sb; "
        "  using var sb2 := new StringBuilder(); "
        "  sb2.Append(\"[\"); "
        "  sb2.Append(o); "
        "  sb2.Append(\"]\"); "
        "  var t := sb2.ToString(); "
        "  Console.WriteLine(t.Length.ToString()); "
        "  Console.WriteLine(int(t[0]).ToString()); "
        "  Console.WriteLine(int(t[1]).ToString()); "
        "  Console.WriteLine(int(t[2]).ToString()); "
        "  var eq: bool := t = \"[日]\"; "
        "  Console.WriteLine(eq.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n91\n26085\n93\nTrue\n");
}

// #7243 / spec §8.5 + stdlib §3.1 (v0.344): THE §18.5 INTERPOLATION HOLE.
// §8.5: "A hole without a format spec formats via the value's ordinary
// `ToString()`" — so a hole holding a builder renders the builder's CONTENT
// through the very same operation. The receiver here is an `Object` PARAMETER,
// so nothing statically narrows the slot to a StringBuilder and the hole
// dispatches through the vtable — the mangled route. Pre-fix the hole
// rendered the two code points 229 and 101.
BEZEL_TEST(codegen_sb_obj_tostring_7243, interpolation_hole_renders_the_content) {
    auto r = runAot(
        "module M; class C "
        "  public static method Render(o: Object): string "
        "    return $\"{o}\"; "
        "  end Render; "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(); "
        "    sb.Append(\"日\"); "
        "    var s := Render(sb); "
        "    Console.WriteLine(s.Length.ToString()); "
        "    Console.WriteLine(int(s[0]).ToString()); "
        "    var eq: bool := s = \"日\"; "
        "    Console.WriteLine(eq.ToString()); "
        "    return 0; "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n26085\nTrue\n");
}

// #7243 / stdlib §3.1 (v0.344): `Append(value: Object)` is "Append(value.
// ToString())", so the TEXT a builder renders is what another builder keeps.
// The argument here is an `Object` PARAMETER, so the append dispatches through
// the vtable and the appended text is the rendered one.
BEZEL_TEST(codegen_sb_obj_tostring_7243, append_object_parameter_keeps_the_rendered_text) {
    auto r = runAot(
        "module M; class C "
        "  public static method Add(sb: StringBuilder, o: Object): StringBuilder "
        "    sb.Append(o); "
        "    return sb; "
        "  end Add; "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(); "
        "    sb.Append(\"日\"); "
        "    using var sb2 := new StringBuilder(); "
        "    _ := Add(sb2, sb); "
        "    var t := sb2.ToString(); "
        "    Console.WriteLine(t.Length.ToString()); "
        "    Console.WriteLine(int(t[0]).ToString()); "
        "    var eq: bool := t = \"日\"; "
        "    Console.WriteLine(eq.ToString()); "
        "    return 0; "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n26085\nTrue\n");
}

// #7243 / spec §4.6 + stdlib §3.1 (v0.344): the vtable route is reached
// through EVERY holder of an `Object` value, not only through a local. An
// element of an `Array<Object>` holds the builder and dispatches its
// `ToString()` through the vtable.
BEZEL_TEST(codegen_sb_obj_tostring_7243, array_element_route_returns_the_wide_code_point) {
    auto r = runAot(main_(
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"日\"); "
        "  using var arr: Array<Object> := new Array<Object>(); "
        "  arr.Add(sb); "
        "  var t := arr[0].ToString(); "
        "  Console.WriteLine(t.Length.ToString()); "
        "  Console.WriteLine(int(t[0]).ToString()); "
        "  var eq: bool := t = \"日\"; "
        "  Console.WriteLine(eq.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n26085\nTrue\n");
}

// #7243 / spec §4.6 + stdlib §3.1 (v0.344): THE DIRECT ROUTE — the
// builder's OWN `ToString()` — is the sibling of the vtable route and must
// keep rendering the content correctly. This row is a CONTROL: it is green on
// both arms (the direct route was repaired by #7229), and it exists so a
// future change that breaks the direct route cannot hide behind the vtable
// rows.
BEZEL_TEST(codegen_sb_obj_tostring_7243, the_direct_route_still_renders_the_content) {
    auto r = runAot(main_(
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"日\"); "
        "  var t := sb.ToString(); "
        "  Console.WriteLine(t.Length.ToString()); "
        "  Console.WriteLine(int(t[0]).ToString()); "
        "  Console.WriteLine(t.IsAscii.ToString()); "
        "  var eq: bool := t = \"日\"; "
        "  Console.WriteLine(eq.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n26085\nFalse\nTrue\n");
}
