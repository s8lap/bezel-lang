// Fix #615: record-destructuring sub-patterns — enum and string constants.
//
// §19.3 (v0.134): each constant sub-pattern in a DestructuringPattern
// compares against the corresponding field value using the same equality
// semantics as a top-level constant pattern:
//   • enum constant   → int equality on the underlying ordinal
//   • string constant → content equality (§4.6 / §8.4)
//   • int constant    → int equality (regression guard)
//   • bool constant   → bool equality
//
// Before the fix: the recursive patternMatches call for sub-patterns passed
// enumTy=nullptr and isStr=false, so enum NamedPattern sub-patterns always
// returned nullptr (unresolved) → the AND was skipped → always matched the
// first arm, and string ConstantPattern sub-patterns compared by pointer
// instead of content → always false → fell to the discard arm.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
int runProg(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Shared enum + record declarations used by most tests.
// record Pair(a: int, c: Color) — second field is an enum
const char* kColorEnum =
    "module M; "
    "enum Color Red, Green, Blue; end Color; "
    "record Colored(n: int, c: Color); ";

// record Named(tag: string, v: int) — first field is a string
const char* kNamedRec =
    "module M; "
    "record Named(tag: string, v: int); ";
} // namespace

// ── §19.3: enum constant sub-patterns in DestructuringPattern ────────────

// Each arm has a distinct enum sub-pattern; the correct one must fire.
BEZEL_TEST(record_pattern_constants_615, enum_subpat_red_matches) {
    // §19.3: Colored(_, Red) matches when c == Red.
    BEZEL_EXPECT_EQ(runProg(std::string(kColorEnum) +
        "class C public static method Main(): int "
        "  var p := new Colored(1, Color.Red); "
        "  return (case p of "
        "    Colored(n, Color.Red)   => 10, "
        "    Colored(n, Color.Green) => 20, "
        "    Colored(n, Color.Blue)  => 30, "
        "    _ => -1); "
        "end Main; end C;"), 10);
}

BEZEL_TEST(record_pattern_constants_615, enum_subpat_green_matches) {
    // §19.3: Green input must NOT fall to the first (Red) arm — it must
    // fall through to the Green arm and yield 20.
    BEZEL_EXPECT_EQ(runProg(std::string(kColorEnum) +
        "class C public static method Main(): int "
        "  var p := new Colored(1, Color.Green); "
        "  return (case p of "
        "    Colored(n, Color.Red)   => 10, "
        "    Colored(n, Color.Green) => 20, "
        "    Colored(n, Color.Blue)  => 30, "
        "    _ => -1); "
        "end Main; end C;"), 20);
}

BEZEL_TEST(record_pattern_constants_615, enum_subpat_blue_matches) {
    // §19.3: Blue input must reach the third arm.
    BEZEL_EXPECT_EQ(runProg(std::string(kColorEnum) +
        "class C public static method Main(): int "
        "  var p := new Colored(1, Color.Blue); "
        "  return (case p of "
        "    Colored(n, Color.Red)   => 10, "
        "    Colored(n, Color.Green) => 20, "
        "    Colored(n, Color.Blue)  => 30, "
        "    _ => -1); "
        "end Main; end C;"), 30);
}

BEZEL_TEST(record_pattern_constants_615, enum_subpat_discard_fallthrough) {
    // §19.3: a Color not covered by the first two arms hits the discard.
    BEZEL_EXPECT_EQ(runProg(std::string(kColorEnum) +
        "class C public static method Main(): int "
        "  var p := new Colored(1, Color.Blue); "
        "  return (case p of "
        "    Colored(n, Color.Red)   => 10, "
        "    Colored(n, Color.Green) => 20, "
        "    _ => 99); "
        "end Main; end C;"), 99);
}

// ── §19.3 / §4.6 / §8.4: string constant sub-patterns ───────────────────

BEZEL_TEST(record_pattern_constants_615, string_subpat_matches_foo) {
    // §19.3 / §4.6: Named("foo", v) must match when tag == "foo".
    BEZEL_EXPECT_EQ(runProg(std::string(kNamedRec) +
        "class C public static method Main(): int "
        "  var p := new Named(\"foo\", 7); "
        "  return (case p of "
        "    Named(\"foo\", v) => v, "
        "    _ => -1); "
        "end Main; end C;"), 7);
}

BEZEL_TEST(record_pattern_constants_615, string_subpat_matches_bar) {
    // §19.3 / §4.6: Named("bar", v) matches when tag == "bar".
    BEZEL_EXPECT_EQ(runProg(std::string(kNamedRec) +
        "class C public static method Main(): int "
        "  var p := new Named(\"bar\", 42); "
        "  return (case p of "
        "    Named(\"foo\", v) => 1, "
        "    Named(\"bar\", v) => v, "
        "    _ => -1); "
        "end Main; end C;"), 42);
}

BEZEL_TEST(record_pattern_constants_615, string_subpat_no_match_falls_through) {
    // §19.3 / §4.6: non-matching string sub-pattern must NOT match — falls to _.
    BEZEL_EXPECT_EQ(runProg(std::string(kNamedRec) +
        "class C public static method Main(): int "
        "  var p := new Named(\"other\", 5); "
        "  return (case p of "
        "    Named(\"foo\", v) => 1, "
        "    Named(\"bar\", v) => 2, "
        "    _ => 99); "
        "end Main; end C;"), 99);
}

// ── §19.3: int constant sub-pattern regression ───────────────────────────

BEZEL_TEST(record_pattern_constants_615, int_subpat_regression) {
    // §19.3: int sub-patterns must continue to work after the fix.
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "record IntPair(a: int, b: int); "
        "class C public static method Main(): int "
        "  var p := new IntPair(0, 5); "
        "  return (case p of "
        "    IntPair(0, b) => b, "
        "    _ => -1); "
        "end Main; end C;"), 5);
}

BEZEL_TEST(record_pattern_constants_615, int_subpat_no_match_regression) {
    // §19.3: int sub-pattern that doesn't match → falls to discard.
    // -1 does not round-trip through the 8-bit process exit code (runAotOut).
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "record IntPair(a: int, b: int); "
        "class C public static method Main(): int "
        "  var p := new IntPair(1, 5); "
        "  Console.WriteLine((case p of "
        "    IntPair(0, b) => b, "
        "    _ => -1)); "
        "  return 0; "
        "end Main; end C;"), "-1");
}

// ── §19.3: mixed enum-constant + binding sub-pattern ─────────────────────

BEZEL_TEST(record_pattern_constants_615, enum_constant_and_binding_mixed) {
    // §19.3: Colored(n, Color.Red) — enum constant in second position +
    // binding in first. When enum matches, binding exposes the int field.
    BEZEL_EXPECT_EQ(runProg(std::string(kColorEnum) +
        "class C public static method Main(): int "
        "  var p := new Colored(42, Color.Red); "
        "  return (case p of "
        "    Colored(n, Color.Red) => n, "
        "    _ => -1); "
        "end Main; end C;"), 42);
}

BEZEL_TEST(record_pattern_constants_615, enum_constant_and_binding_no_match) {
    // §19.3: enum sub-pattern doesn't match → discard arm.
    // -1 does not round-trip through the 8-bit process exit code (runAotOut).
    BEZEL_EXPECT_EQ(runAotOut(std::string(kColorEnum) +
        "class C public static method Main(): int "
        "  var p := new Colored(42, Color.Green); "
        "  Console.WriteLine((case p of "
        "    Colored(n, Color.Red) => n, "
        "    _ => -1)); "
        "  return 0; "
        "end Main; end C;"), "-1");
}

// ── §19.3: nested record with enum constant one level deep ────────────────

BEZEL_TEST(record_pattern_constants_615, nested_record_enum_subpat) {
    // §19.3: a record whose first field is itself a record value, second is
    // an enum. This exercises the recursive patternMatches path where the
    // outer DestructuringPattern sub-pattern is a NamedPattern for the enum.
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "enum Dir Up, Down; end Dir; "
        "record Step(n: int, d: Dir); "
        "class C public static method Main(): int "
        "  var s := new Step(3, Dir.Down); "
        "  return (case s of "
        "    Step(n, Dir.Up)   => n, "
        "    Step(n, Dir.Down) => n + 10, "
        "    _ => -1); "
        "end Main; end C;"), 13);
}

// ── §A.7 / §19.3 (#1300): a QualifiedName constant in a NON-FINAL slot ────
//
// The tests above always place the enum constant in the FINAL positional slot
// (`Colored(n, Color.Red)`), so the constant is never followed by a `,`. Bug
// #1300: when a QualifiedName constant sub-pattern (`Color.Red`) sits in a
// non-final slot (`TwoColor(Color.Red, Color.Blue)`), the parser's QualifiedName
// branch greedily absorbed the positional separator `,` as ConstantPattern
// alternatives — collapsing two sub-patterns into one → a spurious "binds 1
// value but record has 2 fields" reject (BAD_REJECT). Int-literal constants
// already went through the guarded catch-all path, so `IntPair(0, b)` worked;
// only QualifiedNames were affected. The fix guards the QualifiedName
// alternatives loop on `!inPositional`, mirroring the catch-all path.

BEZEL_TEST(record_pattern_constants_615, qualified_const_nonfinal_slot_matches) {
    // §A.7/§19.3: TwoColor(Color.Red, Color.Blue) — QN constant in the
    // non-final slot must parse as two sub-patterns and match.
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "record TwoColor(a: Color, b: Color); "
        "class C public static method Main(): int "
        "  var p := new TwoColor(Color.Red, Color.Blue); "
        "  return (case p of "
        "    TwoColor(Color.Red, Color.Blue) => 1, "
        "    _ => 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(record_pattern_constants_615, qualified_const_nonfinal_slot_no_match) {
    // §A.7/§19.3: a differing first (non-final) QN constant → falls to _.
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "record TwoColor(a: Color, b: Color); "
        "class C public static method Main(): int "
        "  var p := new TwoColor(Color.Green, Color.Blue); "
        "  return (case p of "
        "    TwoColor(Color.Red, Color.Blue) => 1, "
        "    _ => 0); "
        "end Main; end C;"), 0);
}

BEZEL_TEST(record_pattern_constants_615, qualified_const_nonfinal_then_binding) {
    // §A.7/§19.3: a QN constant in the non-final slot followed by a binding —
    // the binding exposes the trailing field when the constant matches.
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "record ColorInt(c: Color, n: int); "
        "class C public static method Main(): int "
        "  var p := new ColorInt(Color.Red, 42); "
        "  return (case p of "
        "    ColorInt(Color.Red, n) => n, "
        "    _ => -1); "
        "end Main; end C;"), 42);
}

BEZEL_TEST(record_pattern_constants_615, qualified_const_nonfinal_then_binding_no_match) {
    // §A.7/§19.3: non-final QN constant that doesn't match → discard arm
    // (the binding arm must NOT fire).
    // -1 does not round-trip through the 8-bit process exit code (runAotOut).
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "record ColorInt(c: Color, n: int); "
        "class C public static method Main(): int "
        "  var p := new ColorInt(Color.Green, 42); "
        "  Console.WriteLine((case p of "
        "    ColorInt(Color.Red, n) => n, "
        "    _ => -1)); "
        "  return 0; "
        "end Main; end C;"), "-1");
}

BEZEL_TEST(record_pattern_constants_615, qualified_const_two_nonfinal_slots) {
    // §A.7/§19.3: THREE-field record with QN constants in slots 1 AND 2 (both
    // non-final) plus a final QN constant — all three must parse distinctly.
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "record Triple(a: Color, b: Color, c: Color); "
        "class C public static method Main(): int "
        "  var p := new Triple(Color.Red, Color.Green, Color.Blue); "
        "  return (case p of "
        "    Triple(Color.Red, Color.Green, Color.Blue) => 7, "
        "    _ => 0); "
        "end Main; end C;"), 7);
}

BEZEL_TEST(record_pattern_constants_615, qualified_const_arm_alternatives_still_parse) {
    // §A.7 control: at the CASE-ARM level (inPositional=false) a `,`-separated
    // list of QualifiedName constants is a valid alternatives pattern — the
    // #1300 fix must NOT suppress it (it only guards positional slots).
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var c := Color.Green; "
        "  return (case c of "
        "    Color.Red | Color.Green => 5, "
        "    _ => 0); "
        "end Main; end C;"), 5);
}
