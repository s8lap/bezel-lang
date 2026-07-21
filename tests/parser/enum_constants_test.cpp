// Spec-derived parser tests for enum constants — §A.3 `EnumConstants` and
// `EnumConstant`. SerializedClause on enum constants is deferred to its own
// slice (it shares grammar with the field-level serialized clause).

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;
using bezel::test::topDeclSummaries;

BEZEL_TEST(parser_enum_constants, single_constant) {
    auto r = parseAll("module M; enum Color Red; end Color;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0], std::string("enum Color { Red }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_enum_constants, multiple_constants) {
    auto r = parseAll(
        "module M; enum Color Red, Green, Blue; end Color;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(
            s[0], std::string("enum Color { Red, Green, Blue }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_enum_constants, constant_with_value) {
    auto r = parseAll(
        "module M; enum HttpStatus "
        "  OK := 200, NotFound := 404; "
        "end HttpStatus;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(
            s[0],
            std::string(
                "enum HttpStatus { OK := 200, NotFound := 404 }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_enum_constants, mixed_with_and_without_values) {
    auto r = parseAll(
        "module M; enum E A, B := 5, C, D := 10; end E;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(
            s[0],
            std::string(
                "enum E { A, B := 5, C, D := 10 }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Errors -----------------------------------------------------------------

BEZEL_TEST(parser_enum_constants, missing_semicolon_after_constants_is_error) {
    auto r = parseAll("module M; enum E A, B end E;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_enum_constants, trailing_comma_is_allowed) {
    // §1.6 / proposal 0059 (v0.214): the uniform-trailing-comma rule admits
    // a single `,` directly before the `;` that closes the enum-constants
    // list. `A, ;` is now legal (not a parse error).
    auto r = parseAll("module M; enum E A, ; end E;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
