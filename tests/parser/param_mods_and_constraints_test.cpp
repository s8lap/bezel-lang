// Spec-derived parser tests for ParamModifiers (`ref`/`out`/`out owned`/
// `owned`), default-value parameters, and GenericConstraints
// (`where T: …`). A.3 / A.4.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

// --- ParamModifiers (A.4) --------------------------------------------------

static bool succeedsParse(const std::string& source) {
    auto r = parseAll(source);
    return !r.diagnostics.hasErrors();
}

BEZEL_TEST(parser_params, ref_modifier) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; class Foo "
        "public method run(ref x: int): int => x; end Foo;"));
}

BEZEL_TEST(parser_params, out_modifier) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; class Foo "
        "public method run(out x: int): int => 0; end Foo;"));
}

BEZEL_TEST(parser_params, out_owned_modifier) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; class Foo "
        "public method run(out owned x: Connection): bool => true; "
        "end Foo;"));
}

BEZEL_TEST(parser_params, owned_modifier) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; class Foo "
        "public method run(owned x: Connection): bool => true; "
        "end Foo;"));
}

BEZEL_TEST(parser_params, default_value) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; class Foo "
        "public method run(x: int := 0): int => x; end Foo;"));
}

BEZEL_TEST(parser_params, modifier_and_default_value) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; class Foo "
        "public method run(ref x: int := 0): int => x; end Foo;"));
}

BEZEL_TEST(parser_params, mixed_params) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; class Foo "
        "public method run(ref a: int, b: int := 0, out c: bool): bool => true; "
        "end Foo;"));
}

// --- GenericConstraints (A.3) ----------------------------------------------

BEZEL_TEST(parser_constraints, class_constraint) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; class Foo<T> where T: class end Foo;"));
}

BEZEL_TEST(parser_constraints, value_constraint) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; class Foo<T> where T: value end Foo;"));
}

BEZEL_TEST(parser_constraints, notnull_constraint) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; class Foo<T> where T: notnull end Foo;"));
}

BEZEL_TEST(parser_constraints, new_constraint) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; class Foo<T> where T: new() end Foo;"));
}

BEZEL_TEST(parser_constraints, type_constraint) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; class Foo<T> where T: IDisposable end Foo;"));
}

// §14: constraints may be combined per the spec example.
BEZEL_TEST(parser_constraints, multiple_constraints_combined) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; class Foo<T> "
        "where T: class, IDisposable, new() end Foo;"));
}

BEZEL_TEST(parser_constraints, multiple_clauses) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; class Foo<T, U> "
        "where T: class where U: value end Foo;"));
}

BEZEL_TEST(parser_constraints, constraints_on_generic_method) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; class Foo "
        "public method id<T>(x: T): T where T: notnull => x; "
        "end Foo;"));
}

// --- Errors -----------------------------------------------------------------

BEZEL_TEST(parser_constraints, where_without_type_param_is_error) {
    auto r = parseAll(
        "module M; class Foo<T> where : class end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_constraints, where_without_constraint_is_error) {
    auto r = parseAll(
        "module M; class Foo<T> where T: end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
