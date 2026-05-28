// Spec-derived parser tests for the remaining binary operators — §17.1
// levels 5 (shift), 6 (range), 7–9 (bitwise), and 12 (null-coalescing).
// Doesn't cover the keyword relationals `in`/`is`/`is not`/`as` (their own
// slice — they bind types or differ in associativity rules).
//
// Reminder: the §17.1 mandatory-parenthesization rules (e.g. shift mixed
// with arithmetic, range mixed with bitwise) are sema's job — the parser
// accepts the syntax; sema rejects it. The tests below exercise the parse,
// not the mandatory-paren rule.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

static std::string parseInitText(const std::string& expr) {
    auto r = parseAll(std::string("module M; class Foo public x: int := ") +
                      expr + std::string("; end Foo;"));
    if (r.diagnostics.hasErrors()) return "<error>";
    const auto ms = memberSummaries(r);
    if (ms.size() != 1) return "<no member>";
    const std::string prefix = "public x: int := ";
    if (ms[0].rfind(prefix, 0) != 0) return "<bad prefix>";
    return ms[0].substr(prefix.size());
}

// --- Shift (§17.1 L5) -------------------------------------------------------

BEZEL_TEST(parser_more_binary, shift_left) {
    BEZEL_EXPECT_EQ(parseInitText("a << b"), std::string("(a << b)"));
}

BEZEL_TEST(parser_more_binary, shift_right) {
    BEZEL_EXPECT_EQ(parseInitText("a >> b"), std::string("(a >> b)"));
}

// --- Bitwise (§17.1 L7–9) ---------------------------------------------------

BEZEL_TEST(parser_more_binary, bitwise_and) {
    BEZEL_EXPECT_EQ(parseInitText("a & b"), std::string("(a & b)"));
}

BEZEL_TEST(parser_more_binary, bitwise_xor) {
    BEZEL_EXPECT_EQ(parseInitText("a ^ b"), std::string("(a ^ b)"));
}

BEZEL_TEST(parser_more_binary, bitwise_or) {
    BEZEL_EXPECT_EQ(parseInitText("a | b"), std::string("(a | b)"));
}

BEZEL_TEST(parser_more_binary, bitwise_and_tighter_than_xor_tighter_than_or) {
    // & (L7) tighter than ^ (L8) tighter than | (L9).
    BEZEL_EXPECT_EQ(parseInitText("a | b ^ c & d"),
                    std::string("(a | (b ^ (c & d)))"));
}

// --- Range (§17.1 L6) -------------------------------------------------------

BEZEL_TEST(parser_more_binary, range_inclusive) {
    BEZEL_EXPECT_EQ(parseInitText("a .. b"), std::string("(a .. b)"));
}

BEZEL_TEST(parser_more_binary, range_exclusive) {
    BEZEL_EXPECT_EQ(parseInitText("a ..< b"), std::string("(a ..< b)"));
}

// §17.1 note: Range looser than Additive but tighter than Relational.
BEZEL_TEST(parser_more_binary, range_looser_than_additive) {
    BEZEL_EXPECT_EQ(parseInitText("a .. b + c"),
                    std::string("(a .. (b + c))"));
}

// --- Null-coalescing (§17.1 L12, right-associative) -------------------------

BEZEL_TEST(parser_more_binary, null_coalescing) {
    BEZEL_EXPECT_EQ(parseInitText("a ?? b"), std::string("(a ?? b)"));
}

BEZEL_TEST(parser_more_binary,
           null_coalescing_is_right_associative_per_17_1) {
    // §17.1 note: `a ?? b ?? c` means `a ?? (b ?? c)`.
    BEZEL_EXPECT_EQ(parseInitText("a ?? b ?? c"),
                    std::string("(a ?? (b ?? c))"));
}

BEZEL_TEST(parser_more_binary, coalescing_binds_tighter_than_logical_or) {
    // §17.1 note: `a ?? b or c` parses as `(a ?? b) or c`.
    BEZEL_EXPECT_EQ(parseInitText("a ?? b or c"),
                    std::string("((a ?? b) or c)"));
}

// --- Mixed precedence ------------------------------------------------------

BEZEL_TEST(parser_more_binary, shift_tighter_than_bitwise) {
    BEZEL_EXPECT_EQ(parseInitText("a | b << c"),
                    std::string("(a | (b << c))"));
}

BEZEL_TEST(parser_more_binary, bitwise_tighter_than_equality) {
    BEZEL_EXPECT_EQ(parseInitText("a & b = c & d"),
                    std::string("((a & b) = (c & d))"));
}

BEZEL_TEST(parser_more_binary, null_coalescing_looser_than_equality) {
    // = (L11) tighter than ?? (L12).
    BEZEL_EXPECT_EQ(parseInitText("a ?? b = c"),
                    std::string("(a ?? (b = c))"));
}
