// Codegen — §8.5 / #2959: a DIRECT `.ToString()` call on an OWNING inline
// array (`[N owned]T`, §15.1.1 / §15.3) used to silently drop the `owned`
// marker, while the interpolation arm (`$"{x}"`) and the boxed dispatch arm
// (`o.ToString()` for `o: Object` holding an owning array) both already
// included it. Three spellings of one §8.5 rule, two right, one wrong.
//
// spec §8.5 (v0.240), the compiler-generated default `ToString`:
//
//   "For inline arrays (all forms, including move-only `[N owned]…` arrays,
//    whose receiver is borrowed for the call) and for the unit type: the
//    type's name — `"[3]int"`-style for an inline array … (element-listing
//    output is deliberately not synthesized — an inline array's elements may
//    be class-typed borrows whose formatting is not the container's
//    business)."
//
// "all forms" includes the move-only form. §15.1.1 / §15.3 make `owned` part
// of the type — `[2 owned]R` and `[2]R` are distinct types with distinct
// copy semantics — so the type's name for the owning form is `[2 owned]R`,
// not `[2]R`. A render that drops the marker is a wrong value, not a style
// choice, and the two-call-form disagreement (direct vs interpolation) made
// it observable in one program without the boxed shape at all.
//
// §15.1.1 NOTE (v0.307, added when `ade3b1d6` began enforcing the rule).
// This file originally spelled its owning rows over `int` and `string`
// elements. §15.1.1 admits the `owned` marker only over a CLASS or INTERFACE
// element type ("T must be a class or interface type; value types and
// runtime-managed reference types are not admitted with the `owned` marker"),
// so `[3 owned]int` and `[2 owned]string` are not well-formed programs and
// were never valid subjects for a rendering assertion. The owning rows now
// run over class `R`, class `S` and interface `I` — the two element kinds
// §15.1.1 does admit. Nothing about the SUBJECT moves: §8.5's rendering rule
// is element-kind-independent and the defect was in the direct-call arm's
// hand-built spelling, which never read the element kind either. The
// NON-owning rows keep `int` and `string` verbatim — §15.1.1 restricts the
// `owned` marker only — so the issue's original element axis survives there
// and now doubles as the control that the restriction is owning-form-only.
//
// MEASURED on `9c643e7` (i.e. with #2804's box fix and #2913's interpolation
// fix already in — the OTHER two renderings were correct, only the direct
// arm was wrong):
//
//     var own: [2 owned]R := [new R(), new R()];
//     own.ToString()     ->  "[2]R"        <-- WRONG, drops `owned`
//     $"{own}"           ->  "[2 owned]R"  <-- correct since #2913
//     (Object)own.ToString() is rejected by #2912 (move-only cannot be boxed),
//     so only the direct + interpolation pair is in scope; the box thunk is
//     covered by #2804's own test on the non-owning form and is the authority
//     for the spelling on the non-move-only side.
//
// THE CAUSE. The direct-call arm in `tryLowerInlineArrayCall`
// (src/codegen/LlvmBackend_Calls3.cpp) built the string by hand
// (`"[" + N + "]" + <element name>`) and the local element-name helper
// neither took nor emitted the marker. The interpolation arm and the box
// thunk both already routed through `Emitter::inlineArrayTypeSpelling`, which
// DOES keep the marker — so the disagreement was local to one renderer.
//
// THE FIX. Delegate the direct arm to the same `inlineArrayTypeSpelling` the
// other two arms already use. One §8.5 answer, one spelling, no way for the
// three paths to drift — exactly the discipline #2913's commit message
// pinned. (The local element-name helper had no other call site after this
// and was removed.)
//
// §15.1.1 / §15.3 OWNING-FORM NOTE.  `own.ToString()` does NOT consume the
// owning array — §8.5's parenthetical ("whose receiver is borrowed for the
// call") makes that explicit, and the lowering at this site is a literal
// pool reference with no read of the receiver's slots, so an `own` declared
// immediately above and printed in this program does not need a copy.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// Returns the program's stdout, or a marker string when it did not run — so a
// compile/run failure can never be mistaken for a rendering mismatch.
std::string out(const std::string& body) {
    auto r = runAot("module M; "
                    "class R public constructor() end constructor; end R; "
                    "class S public constructor() end constructor; end S; "
                    "interface I "
                    "  method GetV(): int; "
                    "end I; "
                    "class W: I "
                    "  public constructor() end constructor; "
                    "  public method GetV(): int => 1; "
                    "end W; "
                    "class C public static method Main(): int " +
                    body + " return 0; end Main; end C;");
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

} // namespace

// The issue's own repro (a,b,c axis: three distinct owning arrays in one
// program, each rendered both ways). Brackets around the part so a wrong
// render is visible as `[X]` rather than as a blank line.
//
// The three axes were int / class / string as the issue spelled them; under
// §15.1.1 (v0.307) the `owned` marker admits only class and interface
// elements, so they are now class `R` at arity 2, class `S` at arity 3, and
// interface `I` at arity 2 — still three distinct owning types, still both
// arities, still one program. The defect was in `tryLowerInlineArrayCall`'s
// hand-built `"[" + N + "]" + <element name>`, which read neither the
// element's KIND nor the marker, so the element kind is not a load-bearing
// axis of this assertion; the two CALL FORMS are.
//
// PRE-FIX this printed
//   [2]R
//   [2 owned]R
//   [3]S
//   [3 owned]S
//   [2]I
//   [2 owned]I
// (the direct calls dropped `owned`, the interpolations kept it). POST-FIX
// every line is the type name with its `owned` marker, in either form.
BEZEL_TEST(inline_array_owning_tostring_2959,
           direct_and_interpolated_agree_on_owning_form_spec_8_5) {
    BEZEL_EXPECT_EQ(
        out("  var own: [2 owned]R := [new R(), new R()]; "
            "  var os3: [3 owned]S := [new S(), new S(), new S()]; "
            "  var oif: [2 owned]I := [new W(), new W()]; "
            "  Console.WriteLine($\"[{own.ToString()}]\"); "
            "  Console.WriteLine($\"[{own}]\"); "
            "  Console.WriteLine($\"[{os3.ToString()}]\"); "
            "  Console.WriteLine($\"[{os3}]\"); "
            "  Console.WriteLine($\"[{oif.ToString()}]\"); "
            "  Console.WriteLine($\"[{oif}]\");"),
        "[[2 owned]R]\n"
        "[[2 owned]R]\n"
        "[[3 owned]S]\n"
        "[[3 owned]S]\n"
        "[[2 owned]I]\n"
        "[[2 owned]I]\n");
}

// AXIS (c) — class-typed element on its own. The axis "class element" is
// the one the issue's repro placed first; this pins it without the other
// axes crowding the assertion.
BEZEL_TEST(inline_array_owning_tostring_2959,
           owning_inline_array_of_class_renders_owned_spec_8_5) {
    BEZEL_EXPECT_EQ(out("  var own: [2 owned]R := [new R(), new R()]; "
                        "  Console.WriteLine(own.ToString()); "
                        "  Console.WriteLine($\"{own}\");"),
                    "[2 owned]R\n[2 owned]R\n");
}

// AXIS (a) — arity 3. The issue spelled this axis `[3 owned]int`; §15.1.1
// (v0.307) does not admit a value-type element under the `owned` marker, so
// the axis is carried at the same arity over class `S`. What this row pins
// is the ARITY-3 owning form against #2804's non-owning `[3]int` baseline
// (kept verbatim in non_owning_form_unaffected_spec_8_5 below): a direct
// call must keep the `owned` marker at an arity the two-element rows do not
// cover. The int element itself was never the subject — §8.5's spelling rule
// and the defective hand-built renderer are both element-kind-blind.
BEZEL_TEST(inline_array_owning_tostring_2959,
           owning_inline_array_of_class_at_arity_three_renders_owned_spec_8_5) {
    BEZEL_EXPECT_EQ(out("  var own: [3 owned]S := [new S(), new S(), new S()]; "
                        "  Console.WriteLine(own.ToString()); "
                        "  Console.WriteLine($\"{own}\");"),
                    "[3 owned]S\n[3 owned]S\n");
}

// ACCEPT-CONTROL — the non-owning form is the #2804/#2913 baseline and must
// stay unchanged. Without this, a fix that simply dropped the `owned` marker
// from the interpolation arm as well would look like it passed.
//
// (Both [3]int and [2]string are the element types the issue's repro used,
// kept verbatim. §15.1.1 restricts only the `owned` marker, so the NON-owning
// form still admits a value type and a runtime-managed reference type — which
// makes this row double as the control that the §15.1.1 element restriction
// is owning-form-only and did not leak into `[N]T`.)
BEZEL_TEST(inline_array_owning_tostring_2959,
           non_owning_form_unaffected_spec_8_5) {
    BEZEL_EXPECT_EQ(out("  var na: [3]int := [1, 2, 3]; "
                        "  var ns: [2]string := [\"x\", \"y\"]; "
                        "  Console.WriteLine(na.ToString()); "
                        "  Console.WriteLine($\"{na}\"); "
                        "  Console.WriteLine(ns.ToString()); "
                        "  Console.WriteLine($\"{ns}\");"),
                    "[3]int\n[3]int\n[2]string\n[2]string\n");
}

// THREE-WAY AGREEMENT. §8.5 fixes one answer; #2804 brought the box thunk
// into `inlineArrayTypeSpelling`; #2913 brought the interpolation arm into
// it; #2959 brings the direct-call arm in. Asserting the three on the
// NON-owning form here proves the fix did not perturb the box-thunk spelling
// (the box thunk is unreachable on the OWNING form — §8.6 forbids it — so
// the two-arm test above is the only one that can pin the direct arm on
// the move-only form, and this three-way test is the one that pins the
// non-move-only form is still a single spelling across all three arms).
BEZEL_TEST(inline_array_owning_tostring_2959,
           all_three_arms_agree_on_non_owning_form_spec_8_5) {
    BEZEL_EXPECT_EQ(out("  var a: [3]int := [1, 2, 3]; "
                        "  var o: Object := a; "
                        "  Console.WriteLine($\"[{a}]\"); "
                        "  Console.WriteLine($\"[{a.ToString()}]\"); "
                        "  Console.WriteLine($\"[{o.ToString()}]\");"),
                    "[[3]int]\n[[3]int]\n[[3]int]\n");
}
