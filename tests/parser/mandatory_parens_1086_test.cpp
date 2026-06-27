// Spec-derived parser tests for §17.1 "Mandatory parenthesization" (#1086).
// The compiler must REJECT an unparenthesized mix of these non-associative
// cross-category operator pairs (and accept the parenthesized forms and the
// non-mandated mixes):
//   - bitwise (`&`,`|`,`^`) with equality (`=`,`<>`) or relational (`<`..`>=`)
//   - bitwise with logical (`and`,`or`)
//   - shift (`<<`,`>>`) with arithmetic (`+`,`-`)
//   - range (`..`,`..<`) with bitwise
// The check runs in the expression parser (it needs the source-level paren
// grouping, which leaves no node in the AST), so it is a syntax error per the
// spec's "rejects ... with a syntax error" wording.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;

namespace {
// Parse `<expr>` in an initializer position and report whether a §17.1
// mandatory-parenthesization diagnostic fired.
bool rejectsMandatoryParen(const std::string& expr) {
    auto r = parseAll(std::string("module M; class Foo public x: int := ") +
                      expr + std::string("; end Foo;"));
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("§17.1") != std::string::npos)
            return true;
    return false;
}
// Does the expression parse with NO error at all?
bool parsesClean(const std::string& expr) {
    auto r = parseAll(std::string("module M; class Foo public x: int := ") +
                      expr + std::string("; end Foo;"));
    return !r.diagnostics.hasErrors();
}
}  // namespace

// --- Rejected cross-category mixes -----------------------------------------

// §17.1: shift with arithmetic (`a << 2 + 1`).
BEZEL_TEST(mandatory_parens_1086, shift_with_additive_rejected) {
    BEZEL_EXPECT_TRUE(rejectsMandatoryParen("a << 2 + 1"));
    BEZEL_EXPECT_TRUE(rejectsMandatoryParen("1 + a << 2"));
    BEZEL_EXPECT_TRUE(rejectsMandatoryParen("a >> b - 1"));
}

// §17.1: bitwise with equality (`a & 1 = 0` — the `a & MASK = 0` pitfall).
BEZEL_TEST(mandatory_parens_1086, bitwise_with_equality_rejected) {
    BEZEL_EXPECT_TRUE(rejectsMandatoryParen("a & 1 = 0"));
    BEZEL_EXPECT_TRUE(rejectsMandatoryParen("a | 1 <> 0"));
    BEZEL_EXPECT_TRUE(rejectsMandatoryParen("a ^ 1 = 0"));
}

// §17.1: bitwise with relational.
BEZEL_TEST(mandatory_parens_1086, bitwise_with_relational_rejected) {
    BEZEL_EXPECT_TRUE(rejectsMandatoryParen("a & 1 < 2"));
    BEZEL_EXPECT_TRUE(rejectsMandatoryParen("2 > a & 1"));
}

// §17.1: bitwise with logical.
BEZEL_TEST(mandatory_parens_1086, bitwise_with_logical_rejected) {
    BEZEL_EXPECT_TRUE(rejectsMandatoryParen("a & 1 and b"));
    BEZEL_EXPECT_TRUE(rejectsMandatoryParen("b or a & 1"));
}

// §17.1: range with bitwise (`mask & 0..n`).
BEZEL_TEST(mandatory_parens_1086, range_with_bitwise_rejected) {
    BEZEL_EXPECT_TRUE(rejectsMandatoryParen("mask & 0 .. n"));
    BEZEL_EXPECT_TRUE(rejectsMandatoryParen("0 & 1 .. n"));
}

// --- Accepted forms --------------------------------------------------------

// The explicit-paren forms of each rejected mix parse cleanly.
BEZEL_TEST(mandatory_parens_1086, parenthesized_forms_accepted) {
    BEZEL_EXPECT_TRUE(parsesClean("a << (2 + 1)"));
    BEZEL_EXPECT_TRUE(parsesClean("(a << 2) + 1"));
    BEZEL_EXPECT_TRUE(parsesClean("(a & 1) = 0"));
    BEZEL_EXPECT_TRUE(parsesClean("(a & 1) < 2"));
    BEZEL_EXPECT_TRUE(parsesClean("(a & 1) <> 0 and b"));
    BEZEL_EXPECT_TRUE(parsesClean("mask & (0 .. n)"));
}

// Non-mandated mixes still parse without parentheses: additive with
// multiplicative, relational with additive, shift/additive INSIDE range bounds
// (range bounds are Shift-level, so `0..n+1` and `1<<2..1<<10` are legal),
// shift with bitwise (not a mandated pair), and logical chains.
BEZEL_TEST(mandatory_parens_1086, non_mandated_mixes_accepted) {
    BEZEL_EXPECT_TRUE(parsesClean("1 + 2 * 3"));
    BEZEL_EXPECT_TRUE(parsesClean("a < b + 1"));
    BEZEL_EXPECT_TRUE(parsesClean("0 .. n + 1"));
    BEZEL_EXPECT_TRUE(parsesClean("1 << 2 .. 1 << 10"));
    BEZEL_EXPECT_TRUE(parsesClean("a | b << c"));
    BEZEL_EXPECT_TRUE(parsesClean("a and b or c"));
}
