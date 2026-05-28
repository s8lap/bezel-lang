// Spec-derived parser tests for import declarations — §2.2 and grammar A.2:
// `Import ::= "import" QualifiedName ";"`.

#include <string>
#include <vector>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::importNames;
using bezel::test::parseAll;

BEZEL_TEST(parser_imports, none_when_only_module_decl) {
    auto r = parseAll("module Foo;");
    BEZEL_EXPECT_EQ(importNames(r).size(), std::size_t{0});
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §2.2 example: a single import with a dotted name.
BEZEL_TEST(parser_imports, spec_2_2_single_import) {
    auto r = parseAll("module Foo; import System.Collections.Generic;");
    const auto names = importNames(r);
    BEZEL_EXPECT_EQ(names.size(), std::size_t{1});
    if (!names.empty()) {
        BEZEL_EXPECT_EQ(names[0], std::string("System.Collections.Generic"));
    }
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_imports, multiple_imports_in_order) {
    auto r = parseAll("module Foo; import A; import B.C; import D;");
    const auto names = importNames(r);
    BEZEL_EXPECT_EQ(names.size(), std::size_t{3});
    if (names.size() == 3) {
        BEZEL_EXPECT_EQ(names[0], std::string("A"));
        BEZEL_EXPECT_EQ(names[1], std::string("B.C"));
        BEZEL_EXPECT_EQ(names[2], std::string("D"));
    }
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_imports, missing_name_after_import_is_error) {
    auto r = parseAll("module Foo; import ;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_imports, missing_semicolon_is_error) {
    auto r = parseAll("module Foo; import Bar");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_imports, trailing_dot_in_import_name_is_error) {
    auto r = parseAll("module Foo; import Bar.;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
