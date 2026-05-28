// Spec-derived parser tests for class primary-constructor parameters
// (§A.3 ClassDecl `PrimaryCtorParams?`). Shares the AST representation with
// record positional fields (slice 21).

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;
using bezel::test::topDeclSummaries;

BEZEL_TEST(parser_class_primary_ctor, single_param) {
    auto r = parseAll("module M; class Foo(name: string) end Foo;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0],
                        std::string("class Foo(name: string)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_class_primary_ctor, multiple_params) {
    auto r = parseAll(
        "module M; class Foo(name: string, age: int) end Foo;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(
            s[0],
            std::string("class Foo(name: string, age: int)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_class_primary_ctor, primary_ctor_with_inheritance) {
    auto r = parseAll(
        "module M; class Sub(name: string): Base end Sub;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(
            s[0],
            std::string("class Sub(name: string): Base"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_class_primary_ctor, generic_with_primary_ctor) {
    auto r = parseAll(
        "module M; class Box<T>(value: T) end Box;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(
            s[0],
            std::string("class Box(value: T)<T>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_class_primary_ctor, primary_ctor_with_members) {
    auto r = parseAll(
        "module M; class Foo(name: string) "
        "public id: int; "
        "public method greet(): string => name; "
        "end Foo;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
