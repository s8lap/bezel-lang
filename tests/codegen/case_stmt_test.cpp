// Codegen slice CG38 — statement-position `case` (§19.4, §19.6). The
// scrutinee is evaluated once; the first arm whose pattern matches (and
// whose optional `when` guard holds) runs its body. Statement-position arm
// bodies are written as `do … end` blocks (a single statement keeps its own
// `;`); both parse as `Block` arms. An unmatched value falls through with no
// action (§19.6) — no MatchFailureException. Observable via exit code.

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

BEZEL_TEST(codegen_cg38, arm_selected_by_value) {
    BEZEL_EXPECT_EQ(run(
        "var x := 2; var r := 0; "
        "case x "
        "  1 => do r := 10; end, "
        "  2 => do r := 20; end, "
        "  3 => do r := 30; end, "
        "end; "
        "return r;"), 20);
}

BEZEL_TEST(codegen_cg38, first_match_wins) {
    BEZEL_EXPECT_EQ(run(
        "var x := 1; var r := 0; "
        "case x "
        "  1 => do r := 7; end, "
        "  1 => do r := 99; end, "
        "end; "
        "return r;"), 7);
}

BEZEL_TEST(codegen_cg38, no_match_falls_through) {
    // §19.6: an unmatched value in statement position falls through with
    // no action — r keeps its initial value, no exception is raised.
    BEZEL_EXPECT_EQ(run(
        "var x := 5; var r := 42; "
        "case x "
        "  1 => do r := 0; end, "
        "  2 => do r := 0; end, "
        "end; "
        "return r;"), 42);
}

BEZEL_TEST(codegen_cg38, multi_statement_do_block) {
    BEZEL_EXPECT_EQ(run(
        "var x := 3; var r := 0; "
        "case x "
        "  3 => do r := 40; r += 2; end, "
        "  _ => do r := 1; end, "
        "end; "
        "return r;"), 42);
}

BEZEL_TEST(codegen_cg38, discard_catch_all) {
    BEZEL_EXPECT_EQ(run(
        "var x := 99; var r := 0; "
        "case x "
        "  1 => do r := 1; end, "
        "  _ => do r := 5; end, "
        "end; "
        "return r;"), 5);
}

BEZEL_TEST(codegen_cg38, guard_on_arm) {
    BEZEL_EXPECT_EQ(run(
        "var x := 2; var r := 0; "
        "case x "
        "  2 when false => do r := 1; end, "
        "  2 when true => do r := 8; end, "
        "end; "
        "return r;"), 8);
}

BEZEL_TEST(codegen_cg38, arm_with_return) {
    // An arm body may terminate (here with `return`); the case still
    // compiles and the post-case code is reachable on the no-match path.
    BEZEL_EXPECT_EQ(run(
        "var x := 1; "
        "case x "
        "  1 => do return 11; end, "
        "  2 => do return 22; end, "
        "end; "
        "return 0;"), 11);
}

// CG38b: binding patterns (`var n`) in statement-position case arms.
// §19.2: the bound name is in scope for the arm guard and body.

BEZEL_TEST(codegen_cg38b, binding_pattern_stores_scrutinee) {
    // `var n` always matches; body stores scrutinee into `r` via `n`.
    BEZEL_EXPECT_EQ(run(
        "var x := 5; var r := 0; "
        "case x "
        "  var n => do r := n; end, "
        "end; "
        "return r;"), 5);
}

BEZEL_TEST(codegen_cg38b, binding_pattern_arithmetic_in_body) {
    BEZEL_EXPECT_EQ(run(
        "var x := 6; var r := 0; "
        "case x "
        "  var n => do r := n * 3; end, "
        "end; "
        "return r;"), 18);
}

BEZEL_TEST(codegen_cg38b, binding_pattern_after_constant_arms) {
    BEZEL_EXPECT_EQ(run(
        "var x := 8; var r := 0; "
        "case x "
        "  1 => do r := 1; end, "
        "  2 => do r := 2; end, "
        "  var n => do r := n + 100; end, "
        "end; "
        "return r;"), 108);
}

BEZEL_TEST(codegen_cg38b, binding_pattern_with_guard) {
    // Guard references `n`; when it holds the arm runs.
    BEZEL_EXPECT_EQ(run(
        "var x := 10; var r := 0; "
        "case x "
        "  var n when n > 5 => do r := n + 1; end, "
        "  _ => do r := 99; end, "
        "end; "
        "return r;"), 11);
}
