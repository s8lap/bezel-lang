// Codegen slice CG28 — `case` expression / pattern matching (§19). The
// scrutinee is evaluated once; arms are tried in order; the first whose
// pattern (and optional `when` guard) matches yields the result. A
// non-matching value raises MatchFailureException (§19.6). This slice
// handles constant, relational-operator, and discard patterns.
// Observable via the int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& body) {
    auto r = runJit(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg28, constant_match) {
    BEZEL_EXPECT_EQ(run(
        "var x := 2; "
        "var r := case x 1 => 10, 2 => 20, 3 => 30 end; return r;"), 20);
}

BEZEL_TEST(codegen_cg28, first_arm_match) {
    BEZEL_EXPECT_EQ(run(
        "var x := 1; "
        "var r := case x 1 => 10, 2 => 20 end; return r;"), 10);
}

BEZEL_TEST(codegen_cg28, constant_alternatives) {
    // One arm listing several constants matches any of them.
    BEZEL_EXPECT_EQ(run(
        "var x := 3; "
        "var r := case x 1, 2, 3 => 99, 4 => 0 end; return r;"), 99);
}

BEZEL_TEST(codegen_cg28, discard_is_catch_all) {
    BEZEL_EXPECT_EQ(run(
        "var x := 42; "
        "var r := case x 1 => 1, _ => 7 end; return r;"), 7);
}

// NOTE: relational-operator patterns (`< EXPR`, `>= EXPR`) and range /
// type / binding / null / destructure patterns are deferred — only
// constant, constant-alternative, and discard patterns (plus `when`
// guards) lower in this slice.

BEZEL_TEST(codegen_cg28, guard) {
    // A `when` guard refining a constant pattern (guard uses an outer var).
    BEZEL_EXPECT_EQ(run(
        "var x := 2; var y := 1; "
        "var r := case x "
        "  2 when y > 0 => 50, "
        "  2 => 60, "
        "  _ => 0 "
        "end; return r;"), 50);
}

BEZEL_TEST(codegen_cg28, no_match_raises_match_failure) {
    // No arm matches and there is no discard → MatchFailureException.
    BEZEL_EXPECT_EQ(run(
        "var x := 9; "
        "try var r := case x 1 => 10, 2 => 20 end; return r; "
        "catch MatchFailureException => return 77; end;"), 77);
}

// §19.3: range patterns in case arms — `lo..hi` and `lo..<hi`.
BEZEL_TEST(codegen_cg28, range_pattern_hit) {
    BEZEL_EXPECT_EQ(run(
        "var x := 5; "
        "var r := case x 1..3 => 1, 4..6 => 2, _ => 3 end; return r;"), 2);
}

BEZEL_TEST(codegen_cg28, range_pattern_low_arm) {
    BEZEL_EXPECT_EQ(run(
        "var x := 2; "
        "var r := case x 1..3 => 1, 4..6 => 2, _ => 3 end; return r;"), 1);
}

BEZEL_TEST(codegen_cg28, range_pattern_wildcard_fallthrough) {
    BEZEL_EXPECT_EQ(run(
        "var x := 9; "
        "var r := case x 1..3 => 1, 4..6 => 2, _ => 3 end; return r;"), 3);
}

BEZEL_TEST(codegen_cg28, range_pattern_half_open) {
    // `..<` excludes the upper bound.
    BEZEL_EXPECT_EQ(run(
        "var x := 6; "
        "var r := case x 1..<6 => 1, 6..10 => 2, _ => 0 end; return r;"), 2);
}

BEZEL_TEST(codegen_cg28, range_pattern_combined_with_constant) {
    // A range pattern and a constant pattern in the same case.
    BEZEL_EXPECT_EQ(run(
        "var x := 0; "
        "var r := case x 0 => 10, 1..5 => 20, _ => 30 end; return r;"), 10);
}

// CG28b / CG38b: binding patterns (`var n`) in case expressions / statements.
// §19.2: the bound name is in scope for the arm guard and body; the binding
// is a borrow of the scrutinee value (same type, always matches).

BEZEL_TEST(codegen_cg28b, binding_pattern_basic) {
    // `var n` always matches; `n` should equal the scrutinee (5).
    BEZEL_EXPECT_EQ(run(
        "var x := 5; "
        "var r := case x var n => n end; return r;"), 5);
}

BEZEL_TEST(codegen_cg28b, binding_pattern_with_arithmetic) {
    // Body can compute with the bound name.
    BEZEL_EXPECT_EQ(run(
        "var x := 7; "
        "var r := case x var n => n + 1 end; return r;"), 8);
}

BEZEL_TEST(codegen_cg28b, binding_pattern_after_non_match) {
    // A binding pattern following unmatched constant arms catches the value.
    BEZEL_EXPECT_EQ(run(
        "var x := 9; "
        "var r := case x 1 => 0, 2 => 0, var n => n * 2 end; return r;"), 18);
}

BEZEL_TEST(codegen_cg28b, binding_pattern_with_guard_true) {
    // A `when` guard can reference the bound name; guard passes.
    BEZEL_EXPECT_EQ(run(
        "var x := 4; "
        "var r := case x var n when n > 2 => n + 10, _ => 0 end; return r;"),
        14);
}

BEZEL_TEST(codegen_cg28b, binding_pattern_with_guard_false) {
    // Guard fails → fall through to wildcard.
    BEZEL_EXPECT_EQ(run(
        "var x := 1; "
        "var r := case x var n when n > 2 => n + 10, _ => 99 end; return r;"),
        99);
}
