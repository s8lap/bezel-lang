// #1586 -- Spec Sec 16.3 (v0.183): `Range<char>` iteration SKIPS the UTF-16
// surrogate gap U+D800-U+DFFF instead of terminating at the boundary (the
// v0.78 rule, which no prior codegen actually implemented -- this slice adds
// the gap-skip behavior from scratch). The successor of U+D7FF is U+E000 and
// the predecessor of U+E000 is U+D7FF; a stride of k is k successor (or,
// descending, predecessor) applications, so a stepped range continues across
// the gap instead of stopping at it. No surrogate is ever yielded.
//
// Covers the spec's own worked example (U+D7F0..U+E010 step 4) plus the
// default-step (1) ascending/descending cases, both the counted-loop `for`
// optimization (literal range, range-typed variable) and the lazy LINQ
// receiver path (.Count()), which must agree per Sec 16.5's "observable
// behavior is identical" requirement for the compiler's loop optimization.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// Sec 16.3 spec worked example: U+D7F0..U+E010 step 4 yields
// D7F0, D7F4, D7F8, D7FC, E000, E004, E008, E00C, E010 -- 9 values, the
// fourth successor of D7FC is E000 (skipping D800..DFFF entirely).
BEZEL_TEST(range_char_surrogate_gap_1586, spec_16_3_worked_example_count) {
    BEZEL_EXPECT_EQ(
        run("var n := 0; "
            "for var c in '\\uD7F0'..'\\uE010' step 4 do n := n + 1; end; "
            "return n;"),
        9);
}

// Same example, split by which side of the gap each value falls on: 4 values
// below the gap (D7F0, D7F4, D7F8, D7FC) and 5 at/above U+E000 (E000, E004,
// E008, E00C, E010). Confirms the stepping actually crosses the gap rather
// than, say, wrapping or stopping early -- a wrong implementation that
// silently terminated at the old v0.78 boundary would report 4 and 0.
BEZEL_TEST(range_char_surrogate_gap_1586, spec_16_3_worked_example_split_counts) {
    BEZEL_EXPECT_EQ(
        run("var below := 0; var above := 0; "
            "for var c in '\\uD7F0'..'\\uE010' step 4 do "
            "  if int(c) < 0xE000 then below := below + 1; "
            "  else above := above + 1; end; "
            "end; "
            "return below * 10 + above;"),
        45);
}

// No yielded value ever falls inside the surrogate block U+D800-U+DFFF.
BEZEL_TEST(range_char_surrogate_gap_1586, spec_16_3_never_yields_a_surrogate) {
    BEZEL_EXPECT_EQ(
        run("var hitGap := false; "
            "for var c in '\\uD7F0'..'\\uE010' step 4 do "
            "  if int(c) >= 0xD800 and int(c) <= 0xDFFF then hitGap := true; end; "
            "end; "
            "return (if hitGap then 1 else 0);"),
        0);
}

// Default step (1), ascending: U+D7FE..U+E002 yields
// D7FE, D7FF, E000, E001, E002 -- 5 values.
BEZEL_TEST(range_char_surrogate_gap_1586, default_step_ascending_count) {
    BEZEL_EXPECT_EQ(
        run("var n := 0; "
            "for var c in '\\uD7FE'..'\\uE002' do n := n + 1; end; "
            "return n;"),
        5);
}

// Sec 16.4 descending (step -1): U+E002..U+D7FE step -1 yields
// E002, E001, E000, D7FF, D7FE -- 5 values, predecessor of E000 is D7FF.
BEZEL_TEST(range_char_surrogate_gap_1586, descending_step_count) {
    BEZEL_EXPECT_EQ(
        run("var n := 0; "
            "for var c in '\\uE002'..'\\uD7FE' step -1 do n := n + 1; end; "
            "return n;"),
        5);
}

// Descending: the LAST value visited is the range's lower bound U+D7FE
// (the predecessor chain must land exactly there, not short by one or
// wrapped through the gap).
BEZEL_TEST(range_char_surrogate_gap_1586, descending_step_lands_on_lower_bound) {
    BEZEL_EXPECT_EQ(
        run("var last := char(0); "
            "for var c in '\\uE002'..'\\uD7FE' step -1 do last := c; end; "
            "return (if last = '\\uD7FE' then 1 else 0);"),
        1);
}

// Sec 16.3 (v0.183) / #499: the Range-typed-VARIABLE counted-loop path
// (distinct codegen from the inline-literal path above) must apply the same
// gap skip.
BEZEL_TEST(range_char_surrogate_gap_1586, range_variable_path_matches_literal) {
    BEZEL_EXPECT_EQ(
        run("var r: Range<char> := '\\uD7F0'..'\\uE010' step 4; "
            "var n := 0; "
            "for var c in r do n := n + 1; end; "
            "return n;"),
        9);
}

// Sec 16.5: "the compiler may optimize... provided the observable behavior is
// identical" -- the lazy LINQ receiver path (bz_lazy_from_range /
// BZ_LAZY_RANGE, used by .Count() and any non-counted-loop consumer) must
// agree with the counted-loop `for` optimization above.
BEZEL_TEST(range_char_surrogate_gap_1586, linq_count_matches_for_loop) {
    BEZEL_EXPECT_EQ(
        run("var total: int := ('\\uD7F0'..'\\uE010' step 4).Count(); "
            "return total;"),
        9);
}

// A runtime-computed (non-constant) step also crosses the gap correctly --
// distinct codegen from the compile-time-constant-step path above.
BEZEL_TEST(range_char_surrogate_gap_1586, runtime_step_crosses_gap) {
    BEZEL_EXPECT_EQ(
        run("var s := 4; var n := 0; "
            "for var c in '\\uD7F0'..'\\uE010' step s do n := n + 1; end; "
            "return n;"),
        9);
}
