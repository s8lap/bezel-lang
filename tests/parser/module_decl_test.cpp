// Spec-derived parser tests for module declarations — §2.1 and grammar A.2:
// `ModuleDecl ::= "module" QualifiedName ";"`.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::moduleName;
using bezel::test::parseAll;

// §2.1 example: single-identifier module name.
BEZEL_TEST(parser_module_decl, spec_2_1_single_identifier) {
    auto result = parseAll("module Foo;");
    BEZEL_EXPECT_EQ(moduleName(result), std::string("Foo"));
    BEZEL_EXPECT_FALSE(result.diagnostics.hasErrors());
}

// §2.1 example: dotted module name. "The dotted form is a single name; it
// does not establish nesting" — the AST just records the parts in order.
BEZEL_TEST(parser_module_decl, spec_2_1_dotted_name) {
    auto result = parseAll("module BankingApp.Transactions;");
    BEZEL_EXPECT_EQ(moduleName(result),
                    std::string("BankingApp.Transactions"));
    BEZEL_EXPECT_FALSE(result.diagnostics.hasErrors());
}

BEZEL_TEST(parser_module_decl, three_part_qualified_name) {
    auto result = parseAll("module A.B.C;");
    BEZEL_EXPECT_EQ(moduleName(result), std::string("A.B.C"));
    BEZEL_EXPECT_FALSE(result.diagnostics.hasErrors());
}

// §2.1: "Every source file declares its module on its first non-comment,
// non-import line" — so a missing module declaration is an error.
BEZEL_TEST(parser_module_decl, spec_2_1_missing_module_is_error) {
    auto result = parseAll("");
    BEZEL_EXPECT_TRUE(result.diagnostics.hasErrors());
}

// Missing semicolon (grammar A.2 requires the trailing `;`).
BEZEL_TEST(parser_module_decl, missing_semicolon_is_error) {
    auto result = parseAll("module Foo");
    BEZEL_EXPECT_TRUE(result.diagnostics.hasErrors());
}

// Missing identifier after `module`.
BEZEL_TEST(parser_module_decl, missing_name_is_error) {
    auto result = parseAll("module ;");
    BEZEL_EXPECT_TRUE(result.diagnostics.hasErrors());
}

// Trailing dot with no identifier after — malformed QualifiedName.
BEZEL_TEST(parser_module_decl, trailing_dot_in_name_is_error) {
    auto result = parseAll("module Foo.;");
    BEZEL_EXPECT_TRUE(result.diagnostics.hasErrors());
}

// §2.1: "on its first non-comment, non-import line" — leading comments are
// permitted and do not interfere with module-decl recognition.
BEZEL_TEST(parser_module_decl, leading_line_comment_is_ok) {
    auto result = parseAll("// header comment\nmodule Foo;");
    BEZEL_EXPECT_EQ(moduleName(result), std::string("Foo"));
    BEZEL_EXPECT_FALSE(result.diagnostics.hasErrors());
}

BEZEL_TEST(parser_module_decl, leading_block_comment_is_ok) {
    auto result = parseAll("/* preamble */ module Foo;");
    BEZEL_EXPECT_EQ(moduleName(result), std::string("Foo"));
    BEZEL_EXPECT_FALSE(result.diagnostics.hasErrors());
}
