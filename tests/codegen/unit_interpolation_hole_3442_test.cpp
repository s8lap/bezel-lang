// fix #3442 — a UNIT-TYPED §18.5 INTERPOLATION HOLE rendered `0` for a local
// and HARD-ICEd `#1386` for a literal or a call; §8.5 requires `"()"`
// (spec §18.5 *Interpolated strings*, §8.5 *`ToString`*, §4.7 *The unit type* —
// v0.283).
//
// MEASURED on a3ec90b (this file's parent commit), one statement per compile:
//
//   | source                       | observed         | §8.5 / §18.5 answer |
//   |------------------------------|------------------|---------------------|
//   | `$"[{u}]"`,  `u: ()`         | prints `[0]`     | `[()]`              |
//   | `$"[{p}]"`,  `p: ()?` present| prints `[0]`     | `[()]`              |
//   | `$"[{u,5}]"`                 | prints `[    0]` | `[   ()]`           |
//   | `$"[{()}]"`                  | COMPILE ERROR    | `[()]`              |
//   | `$"[{C.Nothing()}]"`         | COMPILE ERROR    | `side` then `[()]`  |
//
// The two compile errors were both
//
//     :1:1: internal: lowerExpr returned null for interpolated-string
//           expression part — #1386
//
// on programs §4.7 makes well-formed. The `.ToString()` spelling of the very
// same hole was CORRECT on every row (`$"[{u.ToString()}]"` printed `[()]`), so
// the §8.5 answer existed and the interpolation path simply did not consult it —
// two spellings of one rule disagreeing inside one program, which is the shape
// #2913 (the inline-array hole) and #2804 (the boxed inline array) each closed
// one type over.
//
// ── THE TWO SYMPTOMS ARE TWO DIFFERENT MECHANISMS ────────────────────────────
//
//   * A `()` LOCAL has storage, and that storage is the one INERT placeholder
//     byte `unitPlaceholderTy()` — the byte #2915 / #3421 / `isNullableUnitTy`
//     all describe as one whose "CONTENTS are never meaningfully read". So
//     `lowerExpr` returned a NON-null `i8 0`, the hole fell past every arm to
//     the trailing LLVM-shape fallback, `isIntegerTy()` matched, and the
//     placeholder's contents were rendered as `"0"`. Reading that byte was the
//     bug.
//   * A `()` LITERAL or a unit-returning CALL has no storage at all: `()` lowers
//     to LLVM `void`, so `lowerExpr` returns nullptr by construction (#3421
//     normalizes the producer). That is exactly what the `#1386` arm treats as
//     "an expression kind codegen does not handle" — an arm that cannot tell
//     "unhandled" from "this is the one value of `()`".
//
// A fix for either half alone leaves the other standing, so both are pinned
// here, in the same file, with exact stdout.
//
// ── WHY IT IS A DEFECT ───────────────────────────────────────────────────────
//
//   * §8.5 states the rendering: "For inline arrays (all forms, including
//     move-only `[N owned]…` arrays, whose receiver is borrowed for the call)
//     and for the unit type: the type's name — `"[3]int"`-style for an inline
//     array, `"()"` for unit".
//   * §8.5's first sentence puts interpolation squarely in scope: "`ToString` is
//     invoked implicitly inside string interpolation ([§18.5]) via virtual
//     dispatch — for present values; a `null` value of a nullable-typed hole
//     renders as the text `null` with no dispatch ([§18.5])."
//   * §4.7: "Wrapping the unit type with `?` (yielding `()?`) is permitted by
//     the grammar and produces an optional with two values, `null` and `()`."
//     The two values must therefore be DISTINGUISHABLE in a hole — T2 is that
//     row, and pre-fix they rendered `0` and `null`, which is distinguishable by
//     accident and wrong on the present side.
//   * §18.5 on alignment: "The alignment applies to the rendered text `null`
//     exactly as to any other rendered text (it participates in field padding)"
//     — so T5's `,5` pads §8.5's `"()"`, and T5's `null` row pads `"null"`.
//
// `0` is a rendering the document permits for no unit-typed expression.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// §4.7: "It is the return type of methods declared without an explicit return
// type" — so `C.Nothing()` is a unit-valued expression that also performs
// observable I/O, and `C.Say` is the same with a caller-chosen marker.
constexpr const char* kDecls =
    "module M3442; "
    "class C "
    "  public static method Nothing() "
    "    Console.WriteLine(\"side\"); "
    "  end Nothing; "
    "  public static method Say(s: string) "
    "    Console.WriteLine(s); "
    "  end Say; ";

std::string prog(const std::string& body) {
    return std::string(kDecls) +
           "  public static method Main(): int " + body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ── T1 — the `()` LOCAL hole. Pre-fix `[0]`: the placeholder byte's contents
// rendered as an integer. Post-fix §8.5's `"()"`.
BEZEL_TEST(codegen_unit_interpolation_hole_3442,
           unit_local_hole_renders_the_spec_8_5_text_spec_18_5_4_7) {
    auto r = runAot(prog(
        "    var u: () := (); "
        "    Console.WriteLine($\"[{u}]\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("[()]\n"));
}

// ── T2 — the `()?` pair, BOTH values in one program, so §4.7's "an optional
// with two values, `null` and `()`" is pinned as a distinction and not just as
// two independent renderings. Pre-fix: `[0]` and `[null]`. The present arm goes
// through the nullable-value tag branch and recurses with the non-nullable inner
// type, so this row also proves the new §8.5 arm is reached through that
// recursion and not only at the top level.
BEZEL_TEST(codegen_unit_interpolation_hole_3442,
           nullable_unit_hole_distinguishes_its_two_values_spec_4_7_18_5) {
    auto r = runAot(prog(
        "    var p: ()? := (); "
        "    var z: ()? := null; "
        "    Console.WriteLine($\"[{p}]\"); "
        "    Console.WriteLine($\"[{z}]\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("[()]\n[null]\n"));
}

// ── T3 — the `()` LITERAL hole. Pre-fix this did not COMPILE at all (`#1386`,
// exit 1), so `r.ran` is false there and the row cannot pass vacuously.
BEZEL_TEST(codegen_unit_interpolation_hole_3442,
           unit_literal_hole_compiles_and_renders_spec_4_7_18_5) {
    auto r = runAot(prog(
        "    Console.WriteLine($\"[{()}]\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("[()]\n"));
}

// ── T4 — the unit-returning CALL hole: the other pre-fix `#1386`. The call's
// own effect must run AND the hole must render — asserting both together is
// what distinguishes this fix from one that merely silenced the ICE by dropping
// the part (§22.5: the order "is observable through side-effecting arguments …
// and is normative").
BEZEL_TEST(codegen_unit_interpolation_hole_3442,
           unit_call_hole_runs_its_effect_and_renders_spec_18_5_22_5) {
    auto r = runAot(prog(
        "    Console.WriteLine($\"[{C.Nothing()}]\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\n[()]\n"));
}

// ── T5 — ALIGNMENT. §18.5: "positive right-aligns in a field of that width,
// negative left-aligns", and "The alignment applies to the rendered text `null`
// exactly as to any other rendered text". Pre-fix the unit rows printed
// `[    0]` / `[0    ]` — the padding was applied, to the wrong text. The `()?`
// rows are here because a nullable hole reaches the padding through a different
// branch than the plain one.
BEZEL_TEST(codegen_unit_interpolation_hole_3442,
           unit_hole_alignment_pads_the_spec_8_5_text_spec_18_5) {
    auto r = runAot(prog(
        "    var u: () := (); "
        "    var p: ()? := (); "
        "    var z: ()? := null; "
        "    Console.WriteLine($\"[{u,5}]\"); "
        "    Console.WriteLine($\"[{u,-5}]|\"); "
        "    Console.WriteLine($\"[{p,5}]\"); "
        "    Console.WriteLine($\"[{z,6}]\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("[   ()]\n[()   ]|\n[   ()]\n[  null]\n"));
}

// ── T6 — SEVERAL unit holes in one string, with markers, so the count and the
// §22.5 textual order of the holes' own effects are pinned and not merely the
// rendering. A part lowered twice, dropped, or reordered fails here.
BEZEL_TEST(codegen_unit_interpolation_hole_3442,
           several_unit_holes_run_in_textual_order_spec_18_5_22_5) {
    auto r = runAot(prog(
        "    var u: () := (); "
        "    Console.WriteLine($\"{C.Say(\"L\")}|{u}|{C.Say(\"R\")}\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("L\nR\n()|()|()\n"));
}

// ── T7 — THE SINGLE-ANSWER INVARIANT. Four spellings of §8.5's unit rendering
// must agree, and this fix routes all four through one constant
// (`kUnitToStringSource`): the bare hole, the explicit `.ToString()` hole, the
// direct unboxed call, and the BOXED form (`var o: Object := u`, §8.6's "the
// value is boxed"), which dispatches through the unit box vtable's own thunk.
// The last two already agreed before the fix — they are the control half — so a
// fix that changed the interpolation answer to anything other than `"()"` breaks
// this row even though every line is generated by the same rule.
BEZEL_TEST(codegen_unit_interpolation_hole_3442,
           all_four_unit_tostring_spellings_agree_spec_8_5_8_6) {
    auto r = runAot(prog(
        "    var u: () := (); "
        "    var o: Object := u; "
        "    Console.WriteLine($\"[{u}]\"); "
        "    Console.WriteLine($\"[{u.ToString()}]\"); "
        "    Console.WriteLine(\"[\" + u.ToString() + \"]\"); "
        "    Console.WriteLine(\"[\" + o.ToString() + \"]\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("[()]\n[()]\n[()]\n[()]\n"));
}

// ── T8 — CONTROL: the neighbouring hole kinds must be byte-identical. An `int`,
// a `string`, a `bool`, a present and an absent `int?`, and an INLINE ARRAY
// (§8.5's other member of the same sentence, #2913's row) — all reached through
// the same part-lowering chain this fix edits, three of them through the very
// arms it touches (the nullable-value branch, the non-primitive fallback, the
// alignment path).
BEZEL_TEST(codegen_unit_interpolation_hole_3442,
           non_unit_holes_unchanged_spec_18_5_8_5) {
    auto r = runAot(prog(
        "    var i: int := 7; "
        "    var s: string := \"hi\"; "
        "    var b: bool := true; "
        "    var n: int? := 3; "
        "    var m: int? := null; "
        "    var a: [2]int := [1, 2]; "
        "    Console.WriteLine($\"[{i}][{s}][{b}]\"); "
        "    Console.WriteLine($\"[{n}][{m}]\"); "
        "    Console.WriteLine($\"[{a}]\"); "
        "    Console.WriteLine($\"[{i,5}][{s,-4}|]\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("[7][hi][True]\n[3][null]\n[[2]int]\n"
                                "[    7][hi  |]\n"));
}
