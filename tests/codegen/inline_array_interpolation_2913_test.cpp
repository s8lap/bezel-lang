// fix #2913 — an INLINE ARRAY interpolated with `$"{a}"` rendered the EMPTY
// STRING, with no diagnostic (spec §8.5 inline-array row, §18.5 String
// interpolation, §15.1 Inline arrays — v0.240).
//
// WHAT THE DOCUMENTS REQUIRE.  §8.5's compiler-generated default `ToString`:
//
//   "For inline arrays (all forms, including move-only `[N owned]…` arrays,
//    whose receiver is borrowed for the call) and for the unit type: the type's
//    name — `"[3]int"`-style for an inline array … (element-listing output is
//    deliberately not synthesized — an inline array's elements may be
//    class-typed borrows whose formatting is not the container's business)."
//
// and §8.5's opening: "`ToString` is invoked implicitly inside string
// interpolation (§18.5) via virtual dispatch."
//
// MEASURED on `267e53f` (i.e. WITH #2804's boxing fix already in):
//     var a: [3]int := [1, 2, 3];
//     $"[{a}]"                     ->  "[]"        <- WRONG, silently
//     var o: Object := a; o.ToString()  ->  "[3]int"    <- correct since #2804
//
// So the same value, in the same program, rendered two different ways depending
// only on whether it went through the box. That disagreement is the sharpest
// statement of the bug: #2804 fixed the BOXED half of one §8.5 rule and the
// unboxed interpolation path had no arm at all.
//
// THE CAUSE.  The interpolation lowering carries its own dispatch chain — a
// second one, mirroring `emitValueToString`'s. An inline array matched no arm,
// fell through to the trailing LLVM-type integer/float fallback, matched no
// width there either, and left the part value null — which renders as the empty
// string.
//
// THE FIX delegates, rather than rendering in place: both the interpolation arm
// and `emitValueToString`'s new InlineArray arm bottom out in the single
// `inlineArrayTypeSpelling` that #2804's box thunk already used. That is
// deliberate — the interpolation chain is already a second place the §8.5
// answer is decided, and writing a renderer there would have made it a third.
//
// ⚠ NOTE ON THE ASSERTED TEXT.  §8.5 fixes the SPELLING, not just
// "non-empty" — so these assert the exact string. A test that only checked
// "not empty" would pass on `"0"`, on an element listing, or on a raw pointer
// render, all of which §8.5 forbids (the element listing explicitly).

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Returns the program's stdout, or a marker string when it did not run — so a
// compile/run failure can never be mistaken for a rendering mismatch.
std::string out(const std::string& body) {
    auto r = runAot("module M; class C public static method Main(): int " +
                    body + " return 0; end Main; end C;");
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

} // namespace

// The issue's own repro. Brackets around the part so an empty render is
// visible as `[]` rather than as a blank line.
BEZEL_TEST(inline_array_interpolation_2913,
           interpolated_inline_array_renders_its_type_name_spec_8_5) {
    BEZEL_EXPECT_EQ(out("  var a: [3]int := [1, 2, 3]; "
                        "  Console.WriteLine($\"[{a}]\");"),
                    "[[3]int]\n");
}

// A second element type and size: the spelling is computed from the type, so a
// hardcoded answer would pass the case above and fail this one.
BEZEL_TEST(inline_array_interpolation_2913,
           interpolated_inline_array_of_string_spec_8_5) {
    BEZEL_EXPECT_EQ(out("  var s: [2]string := [\"x\", \"y\"]; "
                        "  Console.WriteLine($\"[{s}]\");"),
                    "[[2]string]\n");
}

// ★ THE POINT OF THE FIX: the boxed and unboxed spellings of ONE §8.5 rule must
// agree. Pre-fix these printed "[]" and "[[3]int]" respectively — the same
// value rendering two ways in one program. Asserting them together, in one
// program, is what pins the agreement rather than the two values separately.
BEZEL_TEST(inline_array_interpolation_2913,
           boxed_and_unboxed_renderings_agree_spec_8_5) {
    BEZEL_EXPECT_EQ(out("  var a: [3]int := [1, 2, 3]; "
                        "  var o: Object := a; "
                        "  Console.WriteLine($\"[{a}]\"); "
                        "  Console.WriteLine(\"[\" + o.ToString() + \"]\"); "
                        "  Console.WriteLine(\"[\" + a.ToString() + \"]\");"),
                    "[[3]int]\n[[3]int]\n[[3]int]\n");
}

// ACCEPT-CONTROL: ordinary interpolation is untouched. The fix adds an arm to a
// dispatch chain every interpolated part walks, so the guard that matters is
// that the parts which already worked still do — including the `string` part,
// which reaches the chain by a different route than the value types.
//
// ⚠ This case caught a mistake — MINE, not the compiler's. I first asserted
// `true` for the bool part; §8.5 renders it `True`. Confirmed it was my
// expectation and not a regression by running the identical program on the
// pre-change and post-change compilers: byte-identical `7|2.5|True|hi` on both.
// Worth leaving recorded, because "an accept-control went red" and "the fix
// broke something" are not the same claim, and only measuring both binaries
// separates them.
BEZEL_TEST(inline_array_interpolation_2913,
           ordinary_interpolation_unaffected_spec_18_5) {
    BEZEL_EXPECT_EQ(out("  var i := 7; var d := 2.5; var b := true; "
                        "  var t := \"hi\"; "
                        "  Console.WriteLine($\"{i}|{d}|{b}|{t}\");"),
                    "7|2.5|True|hi\n");
}

// ACCEPT-CONTROL: a nullable value part still renders "null" rather than being
// captured by the new arm. `int?` is a struct like an inline array is an
// aggregate, and the nullable branch runs BEFORE the new one — this pins that
// ordering.
BEZEL_TEST(inline_array_interpolation_2913,
           nullable_value_part_still_renders_null_spec_8_5) {
    BEZEL_EXPECT_EQ(out("  var n: int? := null; var m: int? := 4; "
                        "  Console.WriteLine($\"{n}|{m}\");"),
                    "null|4\n");
}
