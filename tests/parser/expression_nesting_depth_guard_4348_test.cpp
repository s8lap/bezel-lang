// Regression test for #4348: the parser's expression-recursion path (a
// parenthesized sub-expression recurses through `parseExpression()` from
// `parsePrimaryExpressionImpl()`'s `(` arm) was unbounded — deeply nested but
// syntactically VALID parenthesized expressions overflowed the native call
// stack and SIGSEGV'd the compiler. Measured at issue filing: SIGSEGV at
// ~7000 paren levels on a default 8 MiB stack, with the threshold moving
// cleanly with `ulimit -s` (so the failure is stack-depth, not memory
// corruption).
//
// fix #4348 adds a depth guard at `parseExpressionImpl()` entry that emits
// a located "expression nesting too deep" diagnostic once a configurable
// ceiling (currently 1000) is exceeded, instead of recursing further.
// This file pins the BEHAVIOUR (a diagnostic is reported, exit 1) without
// hardcoding the exact ceiling — a future tightening is fine as long as
// deeply-nested valid input still produces a clean error rather than a
// SIGSEGV or an unbounded-cascade of "expected ')'" diagnostics.
//
// Spec sections touched: A.7 (Expression / PrimaryAtom grammar).
// Revision: v0.298.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;

namespace {

// Returns true iff the diagnostic engine contains an error mentioning
// "nesting too deep" (the fix's signature message). Filters out cascades of
// unrelated diagnostics so a regression that adds a SECOND "too deep"
// error (e.g. once per paren level) is still distinguishable from one
// that emits the message once.
bool hasNestingTooDeep(const bezel::DiagnosticEngine& d) {
    for (const auto& diag : d.diagnostics()) {
        if (diag.severity == bezel::Severity::Error &&
            diag.message.find("nesting too deep") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Count occurrences of the "nesting too deep" diagnostic. The fix emits
// exactly ONE; a regression that fails to suppress the cascade (or
// over-emits on the bail path) trips this count.
std::size_t countNestingTooDeep(const bezel::DiagnosticEngine& d) {
    std::size_t n = 0;
    for (const auto& diag : d.diagnostics()) {
        if (diag.severity == bezel::Severity::Error &&
            diag.message.find("nesting too deep") != std::string::npos) {
            ++n;
        }
    }
    return n;
}

// Build a source string with `n` levels of nested parens around `1`, in
// `return <expr>;` position inside a class method. `n` levels means `n`
// `(` tokens then `1` then `n` `)` tokens.
std::string nestedParensSource(std::size_t n) {
    std::string src = "module M;\nclass CC\n  public static method Main(): int\n";
    src += "    return ";
    src += std::string(n, '(');
    src += "1";
    src += std::string(n, ')');
    src += ";\n  end Main;\nend CC;\n";
    return src;
}

}  // namespace

// --- Within the ceiling: compiles cleanly (no diagnostic at all) ----------

// A small, comfortably-under-the-limit nesting — pins that the guard does
// not regress ordinary parenthesized expressions.
BEZEL_TEST(expression_nesting_depth_guard_4348, within_limit_parses_clean) {
    auto r = parseAll(nestedParensSource(10));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(countNestingTooDeep(r.diagnostics), std::size_t{0});
}

// Near but still under the ceiling — leaves slack for the exact threshold
// being moved by a future revision (the test asserts the OUTCOME, not the
// constant).
BEZEL_TEST(expression_nesting_depth_guard_4348, just_under_limit_parses_clean) {
    auto r = parseAll(nestedParensSource(100));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(countNestingTooDeep(r.diagnostics), std::size_t{0});
}

// --- Over the ceiling: clean error, NOT a SIGSEGV -------------------------

// Just over the cap — the diagnostic fires. exit-status parity with
// runAot isn't asserted here (parseAll is an in-process lexer+parse);
// the assertion is on the diagnostic set the AOT harness would observe.
BEZEL_TEST(expression_nesting_depth_guard_4348, just_over_limit_errors_cleanly) {
    auto r = parseAll(nestedParensSource(2000));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNestingTooDeep(r.diagnostics));
    // Exactly one — a regression that emits per-level (no skip-loop) would
    // produce 1000+ copies and this asserts the cascade is collapsed.
    BEZEL_EXPECT_EQ(countNestingTooDeep(r.diagnostics), std::size_t{1});
}

// Well over the cap — same shape as the just-over case. Pins that the
// behaviour holds for arbitrarily deep input (not just barely-over-the-
// threshold), which is the actual adversarial case the guard addresses.
// N=10000 is chosen because on a default 8 MiB stack the unfixed parser
// SIGSEGVs at ~7000 nesting levels (issue #4348 measurement) — so this
// test crashes the test process without the fix and is therefore a true
// FAIL-WITHOUT proof (not merely "no diagnostic").
BEZEL_TEST(expression_nesting_depth_guard_4348, deeply_over_limit_errors_cleanly) {
    auto r = parseAll(nestedParensSource(10000));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNestingTooDeep(r.diagnostics));
    BEZEL_EXPECT_EQ(countNestingTooDeep(r.diagnostics), std::size_t{1});
}
