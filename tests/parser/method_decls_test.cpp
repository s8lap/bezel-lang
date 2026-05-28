// Spec-derived parser tests for method declarations — §A.4 MethodDecl /
// MethodBody.
//
// Slice 10 covers the signature (visibility + modifiers + name + generic
// params + param list + optional return type) and the three body shapes:
// expression body (`=> Expression ;`), abstract (`;`), and empty block
// (`end Identifier? ;`). Non-empty block bodies require statements — they
// arrive with the statements slice. ParamModifiers (`ref`/`out`/`owned`)
// and default values likewise.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

// --- Expression-body methods ------------------------------------------------

BEZEL_TEST(parser_method_decls, expression_body_no_params_no_return) {
    auto r = parseAll(
        "module M; class Foo public method run() => 42; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0],
                        std::string("public method run() => 42"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_method_decls, expression_body_with_return_type) {
    auto r = parseAll(
        "module M; class Foo public method run(): int => 42; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0],
                        std::string("public method run(): int => 42"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_method_decls, owned_return_type) {
    // §28.8: `owned` on a return type marks ownership-transfer methods.
    auto r = parseAll(
        "module M; class Foo "
        "public method create(): owned Connection => other; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string(
                "public method create(): owned Connection => other"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_method_decls, single_parameter) {
    auto r = parseAll(
        "module M; class Foo public method run(x: int): int => x; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public method run(x: int): int => x"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_method_decls, multiple_parameters) {
    auto r = parseAll(
        "module M; class Foo "
        "public method add(a: int, b: int): int => a + b; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string(
                "public method add(a: int, b: int): int => (a + b)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Modifiers --------------------------------------------------------------

BEZEL_TEST(parser_method_decls, static_modifier) {
    auto r = parseAll(
        "module M; class Foo public static method run() => 42; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public static method run() => 42"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_method_decls, async_modifier) {
    auto r = parseAll(
        "module M; class Foo public async method run() => x; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public async method run() => x"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_method_decls, virtualkind_abstract) {
    auto r = parseAll(
        "module M; class Foo public abstract method run(); end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0],
                        std::string("public abstract method run() ; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_method_decls, virtualkind_override) {
    auto r = parseAll(
        "module M; class Foo "
        "public override method run() => 42; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public override method run() => 42"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Generic parameters on methods -----------------------------------------

BEZEL_TEST(parser_method_decls, generic_method) {
    auto r = parseAll(
        "module M; class Foo public method id<T>(x: T): T => x; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public method id<T>(x: T): T => x"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Body shapes ------------------------------------------------------------

BEZEL_TEST(parser_method_decls, empty_block_body) {
    auto r = parseAll(
        "module M; class Foo public method run() end run; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0],
                        std::string("public method run() { }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §1.6 end-label mismatch is rejected.
BEZEL_TEST(parser_method_decls, end_label_mismatch_is_error) {
    auto r = parseAll(
        "module M; class Foo public method run() end other; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Multiple methods on one class.
BEZEL_TEST(parser_method_decls, multiple_methods) {
    auto r = parseAll(
        "module M; class Foo "
        "public method a(): int => 1; "
        "public method b(): int => 2; "
        "end Foo;");
    const auto ms = memberSummaries(r);
    BEZEL_EXPECT_EQ(ms.size(), std::size_t{2});
    if (ms.size() == 2) {
        BEZEL_EXPECT_EQ(ms[0],
                        std::string("public method a(): int => 1"));
        BEZEL_EXPECT_EQ(ms[1],
                        std::string("public method b(): int => 2"));
    }
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Fields and methods mixed.
BEZEL_TEST(parser_method_decls, fields_and_methods_mixed) {
    auto r = parseAll(
        "module M; class Foo "
        "public field: int; "
        "public method run(): int => field; "
        "end Foo;");
    const auto ms = memberSummaries(r);
    BEZEL_EXPECT_EQ(ms.size(), std::size_t{2});
    if (ms.size() == 2) {
        BEZEL_EXPECT_EQ(ms[0], std::string("public field: int"));
        BEZEL_EXPECT_EQ(
            ms[1],
            std::string("public method run(): int => field"));
    }
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Error cases ------------------------------------------------------------

BEZEL_TEST(parser_method_decls, missing_name_is_error) {
    auto r = parseAll(
        "module M; class Foo public method () => 42; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_method_decls, missing_paren_open_is_error) {
    auto r = parseAll(
        "module M; class Foo public method run => 42; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_method_decls, missing_semicolon_after_expr_body_is_error) {
    auto r = parseAll(
        "module M; class Foo public method run() => 42 end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
