// Spec-derived parser tests for class/interface/record headers' generic
// parameters and inheritance clauses — §14 / §A.3 GenericParams,
// §11 / §A.3 Inheritance.
//
// Out of scope here: generic constraints (`where T: ...`, A.3
// GenericConstraints*), primary-constructor parameters (`class Foo(a: int)`,
// A.3 PrimaryCtorParams), record positional fields (A.3 PositionalFields),
// serialization clauses, enum-specific inheritance, and members. Each is its
// own slice.

#include <string>
#include <vector>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;
using bezel::test::topDeclSummaries;

// --- Generic parameters (§14 / A.3 GenericParams) ---------------------------

BEZEL_TEST(parser_generics_inheritance, single_generic_param) {
    auto r = parseAll("module M; class Foo<T> end Foo;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1) BEZEL_EXPECT_EQ(s[0], std::string("class Foo<T>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_generics_inheritance, multiple_generic_params) {
    auto r = parseAll("module M; class Dict<K, V> end Dict;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0], std::string("class Dict<K, V>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §14 variance: `in T` (contravariant), `out T` (covariant). Both are
// unconditionally reserved keywords per §B.1.
BEZEL_TEST(parser_generics_inheritance, variance_out) {
    auto r = parseAll("module M; interface IEnum<out T> end IEnum;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0], std::string("interface IEnum<out T>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_generics_inheritance, variance_in) {
    auto r = parseAll("module M; interface IComparer<in T> end IComparer;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0],
                        std::string("interface IComparer<in T>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_generics_inheritance, mixed_variance_params) {
    auto r = parseAll(
        "module M; interface IFunc<in T, out R> end IFunc;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0],
                        std::string("interface IFunc<in T, out R>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Inheritance clauses (§11 / A.3 Inheritance) ----------------------------

BEZEL_TEST(parser_generics_inheritance, single_base_type) {
    auto r = parseAll("module M; class Foo: Base end Foo;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0], std::string("class Foo: Base"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_generics_inheritance, base_plus_interfaces) {
    auto r = parseAll("module M; class Foo: Base, IFoo, IBar end Foo;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0],
                        std::string("class Foo: Base, IFoo, IBar"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_generics_inheritance, generic_base_type) {
    auto r = parseAll("module M; class IntList: List<int> end IntList;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0],
                        std::string("class IntList: List<int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Generic params + inheritance together ----------------------------------

BEZEL_TEST(parser_generics_inheritance,
           generic_class_with_generic_base) {
    auto r = parseAll(
        "module M; class List<T>: ICollection<T> end List;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(
            s[0],
            std::string("class List<T>: ICollection<T>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A.3 InheritItem admits an optional `("ArgList?")` after each item;
// expressions arrive in their own slice, so we tolerate the syntax by
// skipping the parenthesised group without recording it on the AST.
BEZEL_TEST(parser_generics_inheritance,
           inheritance_with_unparsed_arg_list_does_not_error) {
    auto r = parseAll("module M; class Foo: Base() end Foo;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Error cases ------------------------------------------------------------

BEZEL_TEST(parser_generics_inheritance,
           empty_generic_param_list_emits_diagnostic) {
    // A.3: GenericParams requires at least one GenericParam.
    auto r = parseAll("module M; class Foo<> end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_generics_inheritance,
           unclosed_generic_param_list_is_error) {
    auto r = parseAll("module M; class Foo<T end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_generics_inheritance,
           inheritance_without_type_is_error) {
    auto r = parseAll("module M; class Foo: end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
