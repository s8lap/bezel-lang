// Regression tests for #2661 (language spec §8.5 / §18.5 / §8.6, v0.252;
// stdlib §4.14 `Span<T>`/`ReadOnlySpan<T>`, §4.15 `ArraySpan<T>`/
// `ReadOnlyArraySpan<T>`, §4.16 `IReadOnlySpan<TElement>`, §9.8 `Range<T>`,
// §3.3 `ReadOnlyStringSpan`).
//
// LINEAGE. This file was `tests/sema/span_tostring_deferred_2311_test.cpp`,
// which pinned #2311's DEFERRAL DIAGNOSTIC — the holding position taken while
// the §8.5 question was open. The owner ruling on #2661 (v0.241, PR #3043)
// settled it: §8.5's defaults are UNIVERSAL, so the deferral becomes this
// lowering's tests, exactly as the ruling says. Every "…_deferred" test below
// is now the same program asserting the RENDERED TEXT.
//
// WHAT §8.5 NOW STATES, verbatim, and which bullet each type takes:
//
//   "The defaults above are generated for **every** type of the applicable
//    kind, wherever declared — user modules and the standard library alike. The
//    standard library's positional records (`Span<T>`, `ReadOnlySpan<T>`,
//    `ArraySpan<T>`, `ReadOnlyArraySpan<T>`, …) carry the positional-record
//    default like any other record …; `Range<T>` (a compiler-intrinsic body
//    record, stdlib §9.8) carries the auto-equality body-record default of the
//    bullet above, rendering its auto-properties."
//
// ⚠ `Range<T>` takes the BODY-RECORD bullet, not the positional one — the
// train's gate corrected an initial positional misclassification, and stdlib
// §9.8 declares it with a body (`public readonly property Start: T?;` …).
// Getting this backwards is the specific error the ruling warns about, so the
// Range tests below assert all six auto-properties in §9.8's declaration order.
//
// ⚠ `ReadOnlyStringSpan` (§3.3) is EXCLUDED: it declares its own MATERIALIZING
// `ToString`, so §8.5's default does not apply. Its control is below.
//
// THE FOUR COEXISTING BEHAVIOURS this closes, all measured on 663a68e:
//
//   sp.ToString()          -> the #2311 deferral Error (holding position)
//   $"{sp}"                -> silently the EMPTY STRING, exit 0
//   r.ToString() on Range  -> the WHOLE STATEMENT silently dropped, exit 0
//   s.ToString() through an
//     IReadOnlySpan<T>     -> SIGSEGV; ASan "pc points to the zero page"
//                             (box vtable slot 3 was left NULL)
//
// and one more found while fixing them: `Console.WriteLine(sp)` handed the raw
// `bz.arrayspan` struct to `bz_string_to_utf8z` and aborted the IR verifier.
//
// §9.7 IS UNAFFECTED, which is what makes rendering a borrow-bearing record
// safe: the renderer reads two i32 fields and never loads the source pointer,
// so §8.5's "call-bounded borrow of the receiver and its components" holds by
// construction. `span_render_is_leak_free` runs the render 20 times under
// ASan/LSan to pin the +1 discipline as well.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// §4.14 source: `OwningBuffer<int>` -> Span<T> / ReadOnlySpan<T>.
std::string buf3(const std::string& body) {
    return "module M; import System.Collections.Generic; "
           "class C public static method Main(): int "
           "  using var buf := new OwningBuffer<int>(3); "
           "  buf[0] := 10; buf[1] := 11; buf[2] := 12; "
           + body +
           "end Main; end C;";
}

// §4.15 source: `Array<int>` -> ArraySpan<T> / ReadOnlyArraySpan<T>.
std::string arr3(const std::string& body) {
    return "module M; import System.Collections.Generic; "
           "class C public static method Main(): int "
           "  using var arr := new Array<int>(3, 0); "
           + body +
           "end Main; end C;";
}

// A bare `module M; class C … Main` wrapper for the Range programs.
std::string plain(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           "end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// §4.14 — the owning spans. Head UNQUALIFIED (§8.5 keeps record heads bare);
// the component names are stdlib §4.14's own: `Buffer: OwningBuffer<T>`,
// `Start: int`, `Length: int`.
// ---------------------------------------------------------------------------

BEZEL_TEST(span_range_tostring_2661, readonlyspan_tostring_renders_fields) {
    BEZEL_EXPECT_EQ(runAotOut(buf3(
        "  var sp := buf.AsReadOnlySpan(); "
        "  Console.WriteLine(sp.ToString()); "
        "  return 0; ")),
        "ReadOnlySpan(Buffer: OwningBuffer, Start: 0, Length: 3)");
}

BEZEL_TEST(span_range_tostring_2661, span_tostring_renders_fields) {
    BEZEL_EXPECT_EQ(runAotOut(buf3(
        "  var sp := buf.AsSpan(); "
        "  Console.WriteLine(sp.ToString()); "
        "  return 0; ")),
        "Span(Buffer: OwningBuffer, Start: 0, Length: 3)");
}

// A SLICED span: `Start`/`Length` are read from the value, not assumed.
BEZEL_TEST(span_range_tostring_2661, sliced_span_reports_its_own_offsets) {
    BEZEL_EXPECT_EQ(runAotOut(buf3(
        "  var sp := buf.AsSpan(1, 2); "
        "  Console.WriteLine(sp.ToString()); "
        "  return 0; ")),
        "Span(Buffer: OwningBuffer, Start: 1, Length: 2)");
}

// The statement-position shape, which pre-#2311 emitted NO diagnostic at all
// and silently dropped the whole `Console.WriteLine`.
BEZEL_TEST(span_range_tostring_2661, span_tostring_in_statement_position) {
    BEZEL_EXPECT_EQ(runAotOut(buf3(
        "  var sp := buf.AsReadOnlySpan(); "
        "  Console.WriteLine(\"got: \" + sp.ToString()); "
        "  return 0; ")),
        "got: ReadOnlySpan(Buffer: OwningBuffer, Start: 0, Length: 3)");
}

// ---------------------------------------------------------------------------
// §4.15 — the array-backed pair. stdlib §4.15 names the component `Source:
// Array<T>`, so the rendering differs from §4.14's in that one word.
// ---------------------------------------------------------------------------

BEZEL_TEST(span_range_tostring_2661, arrayspan_tostring_renders_fields) {
    BEZEL_EXPECT_EQ(runAotOut(arr3(
        "  var sp := arr.AsSpan(); "
        "  Console.WriteLine(sp.ToString()); "
        "  return 0; ")),
        "ArraySpan(Source: Array, Start: 0, Length: 3)");
}

BEZEL_TEST(span_range_tostring_2661, readonly_arrayspan_tostring_renders_fields) {
    BEZEL_EXPECT_EQ(runAotOut(arr3(
        "  var sp := arr.AsReadOnlySpan(); "
        "  Console.WriteLine(sp.ToString()); "
        "  return 0; ")),
        "ReadOnlyArraySpan(Source: Array, Start: 0, Length: 3)");
}

// ---------------------------------------------------------------------------
// §18.5 — the interpolation hole (section A of the original report: a silent
// EMPTY STRING). It must render what the explicit call renders, since §8.5
// defines interpolation as invoking `ToString` implicitly.
// ---------------------------------------------------------------------------

BEZEL_TEST(span_range_tostring_2661, span_interpolation_agrees_with_call) {
    BEZEL_EXPECT_EQ(runAotOut(buf3(
        "  var sp := buf.AsReadOnlySpan(); "
        "  var i := $\"{sp}\"; "
        "  var e := sp.ToString(); "
        "  Console.WriteLine((if i = e then i else \"DISAGREE\")); "
        "  return 0; ")),
        "ReadOnlySpan(Buffer: OwningBuffer, Start: 0, Length: 3)");
}

// stdlib §2.1: `Console.WriteLine(value)` renders its argument. §9.7 (v0.330)
// rejects `Console.WriteLine(sp)` where `sp: Span<int>` — `Object` is a
// borrow-erasing position and `Span` is borrow-bearing. The correct form is
// `sp.ToString()`, already pinned by `span_tostring_renders_fields`. This test
// pins the console render (via ToString) for a span passed AS AN ARGUMENT
// (vs. the statement-position direct render of `span_tostring_renders_fields`).
BEZEL_TEST(span_range_tostring_2661, span_console_argument_renders) {
    BEZEL_EXPECT_EQ(runAotOut(buf3(
        "  var sp := buf.AsSpan(); "
        "  Console.WriteLine(sp.ToString()); "
        "  return 0; ")),
        "Span(Buffer: OwningBuffer, Start: 0, Length: 3)");
}

// ---------------------------------------------------------------------------
// §4.16 / §8.6 — THE MEMORY-SAFETY HALF, and the one the ruling names
// explicitly ("the boxed `IReadOnlySpan<T>` receiver needs its non-vtable
// dispatch path covered either way"). Pre-#2311 both SEGFAULTED: the box
// vtable's slot 3 was NULL, so the dispatch jumped through a null pointer
// (ASan: "pc points to the zero page"). The slot now holds a real renderer,
// and — because the box vtable is keyed by (element type, HEAD) since this fix
// — the boxed rendering names the span that was actually boxed.
// ---------------------------------------------------------------------------

// §8.6: "The canonical sanctioned use is passing a span to a parameter of type
// `IReadOnlySpan<TElement>`".
BEZEL_TEST(span_range_tostring_2661, ireadonlyspan_param_tostring_renders) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Collections.Generic; "
        "class C "
        "  private static method F(s: IReadOnlySpan<int>): int "
        "    Console.WriteLine(s.ToString()); "
        "    return 0; "
        "  end F; "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(3); "
        "    buf[0] := 10; buf[1] := 11; buf[2] := 12; "
        "    var sp := buf.AsReadOnlySpan(); "
        "    return F(sp); "
        "  end Main; "
        "end C;"),
        "ReadOnlySpan(Buffer: OwningBuffer, Start: 0, Length: 3)");
}

// The interface-typed LOCAL, reached by the implicit span -> IReadOnlySpan
// coercion rather than by a call. Same dispatch, different syntax.
BEZEL_TEST(span_range_tostring_2661, ireadonlyspan_local_tostring_renders) {
    BEZEL_EXPECT_EQ(runAotOut(buf3(
        "  var sp := buf.AsReadOnlySpan(); "
        "  var i: IReadOnlySpan<int> := sp; "
        "  Console.WriteLine(i.ToString()); "
        "  return 0; ")),
        "ReadOnlySpan(Buffer: OwningBuffer, Start: 0, Length: 3)");
}

// The boxed head must be the head of the span that was boxed — the property
// that forced the box vtable to be keyed by head as well as element type. A
// WRITABLE span through the same `IReadOnlySpan<int>` slot renders `Span`,
// not `ReadOnlySpan`.
BEZEL_TEST(span_range_tostring_2661, boxed_head_is_the_boxed_spans_head) {
    BEZEL_EXPECT_EQ(runAotOut(buf3(
        "  var sp := buf.AsSpan(); "
        "  var i: IReadOnlySpan<int> := sp; "
        "  Console.WriteLine(i.ToString()); "
        "  return 0; ")),
        "Span(Buffer: OwningBuffer, Start: 0, Length: 3)");
}

// ---------------------------------------------------------------------------
// §9.8 `Range<T>` — section B of the original report: `r.ToString()` silently
// dropped the WHOLE statement. §8.5 gives it the AUTO-EQUALITY BODY-RECORD
// default over its auto-properties, in stdlib §9.8's declaration order:
// Start, End, Step, IsEndInclusive, IsOpenEnded, IsEmpty.
// ---------------------------------------------------------------------------

// The report's own repro, which printed only "done".
BEZEL_TEST(span_range_tostring_2661, range_tostring_renders_auto_properties) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var r := 1..<3; "
        "  Console.WriteLine(\"range: \" + r.ToString()); "
        "  return 0; ")),
        "range: Range(Start: 1, End: 3, Step: 1, IsEndInclusive: False, "
        "IsOpenEnded: False, IsEmpty: False)");
}

BEZEL_TEST(span_range_tostring_2661, range_inclusive_flag_is_rendered) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var r := 1..3; "
        "  Console.WriteLine(r.ToString()); "
        "  return 0; ")),
        "Range(Start: 1, End: 3, Step: 1, IsEndInclusive: True, "
        "IsOpenEnded: False, IsEmpty: False)");
}

// §16.4: a negative step renders as written, and §16.3's direction rule makes
// the descending-written-but-ascending-stepped range EMPTY.
BEZEL_TEST(span_range_tostring_2661, range_step_and_emptiness_are_computed) {
    // BOTH halves are asserted, and the second is the point: `5..1` is written
    // descending but carries the default +1 step, so §16.3's direction rule
    // makes it EMPTY. An earlier draft of this test built `e` and never looked
    // at it — the `.find()` only covered `d` — so the emptiness half the name
    // promises went unchecked. Assert the whole two-line output instead.
    auto r = runAot(plain(
        "  var d := 10..0 step -1; "
        "  var e := 5..1; "
        "  Console.WriteLine(d.ToString()); "
        "  Console.WriteLine(e.ToString()); "
        "  return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        std::string(
            "Range(Start: 10, End: 0, Step: -1, IsEndInclusive: True, "
            "IsOpenEnded: False, IsEmpty: False)\n"
            "Range(Start: 5, End: 1, Step: 1, IsEndInclusive: True, "
            "IsOpenEnded: False, IsEmpty: True)\n"));
}

// §16.2 / §9.8: an OPEN-ENDED endpoint is `T?` and is `null`; §18.5's rule ("a
// `null`-valued nullable component renders as the text `null`") is what §8.5
// defers to. §16.3: an open-ended range is never empty.
BEZEL_TEST(span_range_tostring_2661, range_open_lower_endpoint_renders_null) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var r: Range<int> := ..<5; "
        "  Console.WriteLine(r.ToString()); "
        "  return 0; ")),
        "Range(Start: null, End: 5, Step: 1, IsEndInclusive: False, "
        "IsOpenEnded: True, IsEmpty: False)");
}

BEZEL_TEST(span_range_tostring_2661, range_open_upper_endpoint_renders_null) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var r: Range<int> := 10..; "
        "  Console.WriteLine(r.ToString()); "
        "  return 0; ")),
        "Range(Start: 10, End: null, Step: 1, IsEndInclusive: True, "
        "IsOpenEnded: True, IsEmpty: False)");
}

// §8.5's component rule applied to the ENDPOINTS: "Each component renders as
// its own `ToString` renders it". `Range<char>` is first-class and spec'd
// (§16.3, "Iteration over `Range<char>`"), and its `Start`/`End` are `T?` =
// `char?`, so they must render CHARACTERS. An earlier draft rendered every
// endpoint through the signed-64 path and printed `Start: 97, End: 122`.
BEZEL_TEST(span_range_tostring_2661, range_char_endpoints_render_as_chars) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var r: Range<char> := 'a'..'z'; "
        "  Console.WriteLine(r.ToString()); "
        "  return 0; ")),
        "Range(Start: a, End: z, Step: 1, IsEndInclusive: True, "
        "IsOpenEnded: False, IsEmpty: False)");
}

// The same through the §18.5 hole, so the two spellings agree on the element
// type as well as on the shape.
BEZEL_TEST(span_range_tostring_2661, range_char_interpolation_agrees) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var r: Range<char> := 'a'..<'d'; "
        "  Console.WriteLine((if $\"{r}\" = r.ToString() then $\"{r}\" "
        "                     else \"DISAGREE\")); "
        "  return 0; ")),
        "Range(Start: a, End: d, Step: 1, IsEndInclusive: False, "
        "IsOpenEnded: False, IsEmpty: False)");
}

// §10.3-style width: a `ulong` endpoint above `long.MaxValue` printed through
// the signed path renders NEGATIVE. The bits are the same; the format is not.
BEZEL_TEST(span_range_tostring_2661, range_ulong_endpoints_render_unsigned) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var r: Range<ulong> := 18000000000000000000uL..18000000000000000002uL; "
        "  Console.WriteLine(r.ToString()); "
        "  return 0; ")),
        "Range(Start: 18000000000000000000, End: 18000000000000000002, "
        "Step: 1, IsEndInclusive: True, IsOpenEnded: False, IsEmpty: False)");
}

BEZEL_TEST(span_range_tostring_2661, range_interpolation_agrees_with_call) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var r := 1..<3; "
        "  var i := $\"{r}\"; "
        "  var e := r.ToString(); "
        "  Console.WriteLine((if i = e then \"AGREE\" else \"DISAGREE\")); "
        "  return 0; ")),
        "AGREE");
}

BEZEL_TEST(span_range_tostring_2661, range_console_argument_renders) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var r := 0..10 step 2; "
        "  Console.WriteLine(r); "
        "  return 0; ")),
        "Range(Start: 0, End: 10, Step: 2, IsEndInclusive: True, "
        "IsOpenEnded: False, IsEmpty: False)");
}

// The rendered `IsEmpty` must be the same answer `r.IsEmpty` gives — one §16.3
// rule, two readers. (The renderer recomputes it rather than reading storage,
// because `bz.range` has no `IsEmpty` field.)
BEZEL_TEST(span_range_tostring_2661, rendered_isempty_agrees_with_the_member) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var a := 3..<3; "
        "  var b := 1..<3; "
        "  var okA := (if a.IsEmpty then \"True\" else \"False\"); "
        "  var okB := (if b.IsEmpty then \"True\" else \"False\"); "
        "  var sa := a.ToString(); "
        "  var sb := b.ToString(); "
        "  var m := 0; "
        "  if sa.Contains(\"IsEmpty: \" + okA) then m := m + 1; end; "
        "  if sb.Contains(\"IsEmpty: \" + okB) then m := m + 2; end; "
        "  Console.WriteLine(m); "
        "  return 0; ")),
        "3");
}

// ---------------------------------------------------------------------------
// MEMORY. §9.7 is unaffected (nothing is retained, consumed or extended) and
// the render is a +1 the binding consumes exactly once.
// ---------------------------------------------------------------------------

BEZEL_TEST(span_range_tostring_2661, span_and_range_renders_are_leak_free) {
    // Four consumers, because they discharge the +1 in four different places:
    // the bound local (isPlusOneStringProducer), the plain §18.5 hole, the
    // ALIGNMENT-SPEC hole (`emitWithFormatSpec`, which byte-copies the render
    // into a padded string and had never released the intermediate — a leak
    // this fix newly routed spans and ranges into, and discharges), and
    // `Console.Write`, whose own tail release is keyed on `releaseRender`.
    auto r = runAotChecked(buf3(
        "  var sp := buf.AsReadOnlySpan(); "
        "  var rg := 1..<3; "
        "  var i := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    var s: string := sp.ToString(); "
        "    var t: string := rg.ToString(); "
        "    var u: string := $\"{sp}|{rg}\"; "
        "    var v: string := $\"{sp,80}|{rg,120}\"; "
        "    Console.Write(\"\"); "
        "    if s.Length < 1 then return 7; end; "
        "    if t.Length < 1 then return 8; end; "
        "    if u.Length < 1 then return 9; end; "
        "    if v.Length < 80 then return 10; end; "
        "  end while; "
        "  return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// The span outlives nothing and nothing outlives it: the buffer is still
// usable after the render, and the span still reads through it (§9.7's
// borrow is untouched — the renderer never loads the source pointer at all).
BEZEL_TEST(span_range_tostring_2661, rendering_does_not_disturb_the_borrow) {
    BEZEL_EXPECT_EQ(runAotOut(buf3(
        "  var sp := buf.AsReadOnlySpan(); "
        "  var before := sp[1]; "
        "  var s := sp.ToString(); "
        "  var after := sp[1]; "
        "  buf[1] := 99; "
        "  var again := sp[1]; "
        "  Console.WriteLine(before.ToString() + \",\" + "
        "                    after.ToString() + \",\" + "
        "                    again.ToString() + \",\" + s); "
        "  return 0; ")),
        "11,11,99,ReadOnlySpan(Buffer: OwningBuffer, Start: 0, Length: 3)");
}

// ---------------------------------------------------------------------------
// ACCEPT controls — the false-reject guards inherited from #2311. The lowering
// keys on a head name shared with receivers that legitimately answer the same
// structural tests, so each must keep working.
// ---------------------------------------------------------------------------

// stdlib §3.3 DOES declare `ToString(): string` on `ReadOnlyStringSpan`
// ("Materialization"), so §8.5's default does NOT apply to it — the one
// exclusion the ruling names. It must keep materializing its code points.
BEZEL_TEST(span_range_tostring_2661, control_readonlystringspan_materializes) {
    std::string out = runAotOut(
        "module M; class C public static method Main(): int "
        "  Console.WriteLine(\"hello\".AsSpan(1, 3).ToString()); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(out.find("ell") != std::string::npos);
}

// §8.5 / #608: a builtin exception type is also a Named TypeRef with no user
// TypeSymbol — the case the universal rule was written for. It carries no
// astNode, which is what keeps it out of the span carve-out. (It renders
// unqualified because a reserved built-in has no declaring module on its
// synthetic symbol — the residual filed as #3119.)
BEZEL_TEST(span_range_tostring_2661, control_builtin_exception_tostring_works) {
    std::string out = runAotOut(
        "module M; class C public static method Main(): int "
        "  try "
        "    throw new ArgumentException(\"boom\"); "
        "  catch var e: Exception => Console.WriteLine(\"EXN[\" + e.ToString() + \"]\"); "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    // fix #3119 (spec §8.5, v0.252; stdlib §2.13 rule 1): the rendered name
    // is the QUALIFIED module path "System.ArgumentException", not the bare
    // identifier "ArgumentException".
    BEZEL_EXPECT_TRUE(out.find("EXN[System.ArgumentException]") != std::string::npos);
}

// §8.5: a user positional record's default `ToString` — the shape the deferral
// declined to rule on, since `Span<T>` is declared the same way. It now renders
// identically in form, which is the whole point of the ruling.
BEZEL_TEST(span_range_tostring_2661, control_user_record_tostring_works) {
    std::string out = runAotOut(
        "module M; "
        "public record Point(X: int, Y: int) end Point; "
        "class C public static method Main(): int "
        "  var p := new Point(10, 20); "
        "  Console.WriteLine(p.ToString()); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(out.find("Point(X: 10, Y: 20)") != std::string::npos);
}

// The rest of the documented span surface must be unaffected.
//
// Each result is bound to a LOCAL rather than tested through an inline guard.
// That is deliberate and was a review finding on #2311's first draft: by this
// issue's own mechanism, a guard whose operand never lowers is a DROPPED guard,
// so an inline-guard version would still fall through to `return 33` and pass
// even if the whole surface regressed to non-lowering.
BEZEL_TEST(span_range_tostring_2661, control_documented_span_surface_still_works) {
    JitResult r = runAot(buf3(
        "  var sp := buf.AsReadOnlySpan(); "
        "  var len := sp.Length; "
        "  var empty := sp.IsEmpty; "
        "  var idx := sp.IndexOf(11); "
        "  var has := sp.Contains(12); "
        "  var sl := sp.Slice(1); "
        "  var slLen := sl.Length; "
        "  if len <> 3 then return 1; end; "
        "  if empty then return 2; end; "
        "  if idx <> 1 then return 3; end; "
        "  if has = false then return 4; end; "
        "  if slLen <> 2 then return 5; end; "
        "  return 33; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 33);
}

// The §4.16 interface's OWN documented surface must still work — filling the
// box vtable's ToString slot must not disturb the members §4.16 does declare,
// which live in the itable the same function builds.
BEZEL_TEST(span_range_tostring_2661, control_ireadonlyspan_surface_still_works) {
    JitResult r = runAot(
        "module M; import System.Collections.Generic; "
        "class C "
        "  private static method F(s: IReadOnlySpan<int>): int "
        "    var len := s.Length; "
        "    var first := s[0]; "
        "    if len <> 3 then return 1; end; "
        "    if first <> 10 then return 2; end; "
        "    return 44; "
        "  end F; "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(3); "
        "    buf[0] := 10; buf[1] := 11; buf[2] := 12; "
        "    var sp := buf.AsReadOnlySpan(); "
        "    return F(sp); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// §16.3's own members must still answer what they answered — the renderer
// recomputes `IsOpenEnded`/`IsEmpty` from the same expressions, and a
// regression in either direction would show here.
BEZEL_TEST(span_range_tostring_2661, control_range_members_still_work) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var r := 1..<3; "
        "  var m := 0; "
        "  if r.Start = 1 then m := m + 1; end; "
        "  if r.End = 3 then m := m + 2; end; "
        "  if r.Step = 1L then m := m + 4; end; "
        "  if r.IsEndInclusive = false then m := m + 8; end; "
        "  if r.IsOpenEnded = false then m := m + 16; end; "
        "  if r.IsEmpty = false then m := m + 32; end; "
        "  if r.Contains(2) then m := m + 64; end; "
        "  Console.WriteLine(m); "
        "  return 0; ")),
        "127");
}

// ---------------------------------------------------------------------------
// #3127 (spec §8.6 / §8.5, v0.252; stdlib §9.8): a BOXED Range<T> —
// `var o: Object := r; o.ToString()` — must produce the same rendering the
// unboxed `r.ToString()` produces above. The pre-fix code path went through
// `boxValue` → `boxVtableFor(srcTy)`; `Range<T>` had no arm in any of the
// three box predicates (#2793's drift hazard — the predicate, the vtable
// minter, and the descriptor minter must agree) and `boxVtableFor` returned
// nullptr, so `boxValue` returned the raw `{i64,i64,i64,i8}` struct where
// the `Object` slot expected a pointer. The compiled program exited 0 with
// no output, indistinguishable from a #2661-style "statement silently
// dropped". #3127's fix wires the range box vtable, descriptor, and
// predicate through the SAME `rangeBoxable` (the #2793 fix) so the three
// cannot drift, and the vtable's ToString slot forwards to the same
// `emitRangeToString` the unboxed path uses. The two programs below are
// the same pair #2661 wrote for the unboxed path, re-tested through the
// boxed path; both renderings must agree.
// ---------------------------------------------------------------------------

// §8.6 / §8.5 / §18.5: `o.ToString()` on a boxed Range<int> renders all six
// auto-properties in §9.8's declaration order — the EXACT same text
// `r.ToString()` produces (the unboxed test above), so the boxed and
// unboxed answers cannot disagree. Pre-fix this prints nothing and exits
// 0; the assertion fails on stdout-text mismatch rather than on a crash,
// so the regression file names the bug rather than the failure mode.
BEZEL_TEST(span_range_tostring_2661, boxed_range_object_tostring_renders) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var r := 1..<3; "
        "  var o: Object := r; "
        "  Console.WriteLine(o.ToString()); "
        "  return 0; ")),
        "Range(Start: 1, End: 3, Step: 1, IsEndInclusive: False, "
        "IsOpenEnded: False, IsEmpty: False)");
}

// §18.5: the §18.5 hole is defined to invoke `ToString` implicitly, so the
// boxed-range interpolation must render the same text the boxed call
// renders — and both must render the same text the unboxed call renders.
// The `if i = e` guard is what makes this a real agreement test: a
// silently-broken `o.ToString()` would also return `""`, `""` would equal
// itself, and the test would pass vacuously against the wrong output.
BEZEL_TEST(span_range_tostring_2661, boxed_range_interpolation_agrees_with_call) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var r := 1..<3; "
        "  var o: Object := r; "
        "  var i := $\"{o}\"; "
        "  var c := o.ToString(); "
        "  var u := r.ToString(); "
        "  Console.WriteLine((if i = c then \"I=C\" else \"I!=C\")); "
        "  Console.WriteLine((if c = u then \"C=U\" else \"C!=U\")); "
        "  if i = c then "
        "    if c = u then Console.WriteLine(\"AGREE\"); "
        "    else Console.WriteLine(\"DISAGREE\"); "
        "    end; "
        "  else Console.WriteLine(\"DISAGREE\"); "
        "  end; "
        "  return 0; ")),
        "I=C\nC=U\nAGREE");
}

// `Range<char>` first-class (§16.3): the element-type handling the boxed
// path mints per-T for must propagate through ToString, so the boxed
// rendering of `Range<char>` names the endpoints as characters — not as
// the integer code points the unsigned-8 stored value also represents.
// Pre-fix the print was empty (the box path was the no-op), and the test
// would have read against `Range(Start: 97, End: 122, ...)` — the same
// wrong-shape failure the unboxed version had before #2661.
BEZEL_TEST(span_range_tostring_2661, boxed_range_char_object_tostring_renders_chars) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var r: Range<char> := 'a'..'z'; "
        "  var o: Object := r; "
        "  Console.WriteLine(o.ToString()); "
        "  return 0; ")),
        "Range(Start: a, End: z, Step: 1, IsEndInclusive: True, "
        "IsOpenEnded: False, IsEmpty: False)");
}

// §8.6 / #422: a boxed Range<T> is +1 at the boxing site and released at
// scope end by the §28.9 placement machinery. The render is itself +1
// (each emitStringConcat), so the consumer must discharge it; this test
// exercises 20 round trips through the boxed ToString to surface any
// leak in either direction (LSan would catch the box-leak half; the
// string-leak half is the #2661 control already pinned). The exit-0
// success line keeps the test from passing vacuously on a crash.
BEZEL_TEST(span_range_tostring_2661, boxed_range_renders_are_leak_free) {
    auto r = runAotChecked(plain(
        "  var i := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    var r := 1..<3; "
        "    var o: Object := r; "
        "    var s: string := o.ToString(); "
        "    if s.Length < 1 then return 7; end; "
        "  end while; "
        "  return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
