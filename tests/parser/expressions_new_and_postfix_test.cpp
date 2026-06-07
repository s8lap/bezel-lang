// Spec-derived parser tests for `new` expressions and the additional
// postfix-level forms — §A.5 NewExpr; §17.1 level 1 postfix `?.`, `?(`,
// `[…]`, and `!`.

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

// --- NewExpr (§A.5) --------------------------------------------------------

BEZEL_TEST(parser_new_postfix, new_simple_no_args) {
    BEZEL_EXPECT_EQ(parseInitText("new Foo()"), std::string("new Foo()"));
}

BEZEL_TEST(parser_new_postfix, new_with_args) {
    BEZEL_EXPECT_EQ(parseInitText("new Foo(1, true, \"hi\")"),
                    std::string("new Foo(1, true, \"hi\")"));
}

BEZEL_TEST(parser_new_postfix, new_generic_type) {
    BEZEL_EXPECT_EQ(parseInitText("new List<int>()"),
                    std::string("new List<int>()"));
}

// §A.5 shorthand: `new(args)` target-typed.
BEZEL_TEST(parser_new_postfix, new_shorthand_no_args) {
    BEZEL_EXPECT_EQ(parseInitText("new()"), std::string("new ()"));
}

BEZEL_TEST(parser_new_postfix, new_shorthand_with_args) {
    BEZEL_EXPECT_EQ(parseInitText("new(a, b)"),
                    std::string("new (a, b)"));
}

// --- Null-conditional postfix ----------------------------------------------

BEZEL_TEST(parser_new_postfix, null_conditional_member_access) {
    BEZEL_EXPECT_EQ(parseInitText("obj?.field"),
                    std::string("obj?.field"));
}

BEZEL_TEST(parser_new_postfix, null_conditional_call) {
    BEZEL_EXPECT_EQ(parseInitText("obj?(arg)"),
                    std::string("obj?(arg)"));
}

BEZEL_TEST(parser_new_postfix, null_conditional_chain) {
    // `?.` chains the same way regular `.` does.
    BEZEL_EXPECT_EQ(parseInitText("obj?.a?.b"),
                    std::string("obj?.a?.b"));
}

// --- Indexing ---------------------------------------------------------------

BEZEL_TEST(parser_new_postfix, single_index) {
    BEZEL_EXPECT_EQ(parseInitText("arr[i]"), std::string("arr[i]"));
}

BEZEL_TEST(parser_new_postfix, multi_dimensional_index) {
    BEZEL_EXPECT_EQ(parseInitText("matrix[r, c]"),
                    std::string("matrix[r, c]"));
}

BEZEL_TEST(parser_new_postfix, index_with_complex_expression) {
    BEZEL_EXPECT_EQ(parseInitText("arr[i + 1]"),
                    std::string("arr[(i + 1)]"));
}

// --- Postfix `!` (null-forgiving) -------------------------------------------

BEZEL_TEST(parser_new_postfix, postfix_bang) {
    BEZEL_EXPECT_EQ(parseInitText("value!"), std::string("value!"));
}

BEZEL_TEST(parser_new_postfix, postfix_bang_chains_with_member_access) {
    BEZEL_EXPECT_EQ(parseInitText("obj!.field"),
                    std::string("obj!.field"));
}

// --- Combinations -----------------------------------------------------------

BEZEL_TEST(parser_new_postfix, new_chained_with_method_call) {
    BEZEL_EXPECT_EQ(parseInitText("new Box(1).value"),
                    std::string("new Box(1).value"));
}

BEZEL_TEST(parser_new_postfix, index_chained_with_member_access) {
    BEZEL_EXPECT_EQ(parseInitText("arr[0].field"),
                    std::string("arr[0].field"));
}

BEZEL_TEST(parser_new_postfix, mixed_call_and_index_chain) {
    BEZEL_EXPECT_EQ(parseInitText("f(a)[i].g(b)"),
                    std::string("f(a)[i].g(b)"));
}

// §5.2 explicit conversion `int(x)` — a primitive type keyword in
// expression position parses as a conversion applied with call syntax.
// (Regression: a primitive keyword was previously left unconsumed in
// expression position, spinning the enclosing statement-list parser.)
BEZEL_TEST(parser_new_postfix, primitive_conversion_call) {
    BEZEL_EXPECT_EQ(parseInitText("int(x)"), std::string("int(x)"));
}

BEZEL_TEST(parser_new_postfix, primitive_conversion_in_arithmetic) {
    BEZEL_EXPECT_EQ(parseInitText("int(d) + 1"),
                    std::string("(int(d) + 1)"));
}

// --- Nested-type construction (#483) — Grammar A.8 / §11.3 ----------------
//
// NewExpr ::= "new" Type "(" ArgList? ")"
// Type → TypeNoNullable → QualifiedName (A.8 allows "." in type names)
// QualifiedName ::= Identifier ("." Identifier)*
//
// The dotted segments BEFORE the "(" arg list are part of the type name;
// a "." AFTER the "()" is a member access on the constructed result.

// Bug #483 regression: `new Outer.Inner()` must parse the *full* qualified
// name `Outer.Inner` as the type, not just `Outer`.
BEZEL_TEST(parser_new_postfix_483, new_nested_type_no_args_spec_A8) {
    // Grammar A.8 / §11.3: `new Outer.Inner()` → NewExpr{type=Outer.Inner, args=[]}
    BEZEL_EXPECT_EQ(parseInitText("new Outer.Inner()"),
                    std::string("new Outer.Inner()"));
}

// Two-level nesting with constructor arguments — the args follow the full type name.
BEZEL_TEST(parser_new_postfix_483, new_nested_type_with_args_spec_A8) {
    BEZEL_EXPECT_EQ(parseInitText("new Outer.Inner(1, 2)"),
                    std::string("new Outer.Inner(1, 2)"));
}

// Disambiguation: dotted segments BEFORE "(" are type; "." AFTER "()" is member access.
// `new Outer.Inner()._v` → MemberAccess{NewExpr{Outer.Inner, []}, "_v"}
BEZEL_TEST(parser_new_postfix_483, new_nested_type_then_member_access_spec_A8) {
    BEZEL_EXPECT_EQ(parseInitText("new Outer.Inner()._v"),
                    std::string("new Outer.Inner()._v"));
}

// Chained call on the constructed result — `new Outer.Inner().M()`.
BEZEL_TEST(parser_new_postfix_483, new_nested_type_then_method_call_spec_A8) {
    BEZEL_EXPECT_EQ(parseInitText("new Outer.Inner().M()"),
                    std::string("new Outer.Inner().M()"));
}

// Regression: single-identifier `new Foo()` must still parse correctly.
BEZEL_TEST(parser_new_postfix_483, new_simple_regression_spec_A8) {
    BEZEL_EXPECT_EQ(parseInitText("new Foo()"), std::string("new Foo()"));
}

// Regression: generic type `new Foo<int>()` must still parse correctly.
BEZEL_TEST(parser_new_postfix_483, new_generic_regression_spec_A8) {
    BEZEL_EXPECT_EQ(parseInitText("new Foo<int>()"),
                    std::string("new Foo<int>()"));
}

// Three-level nested type name `new A.B.C()` — grammar allows arbitrarily
// deep QualifiedName.
BEZEL_TEST(parser_new_postfix_483, new_triple_nested_type_spec_A8) {
    BEZEL_EXPECT_EQ(parseInitText("new A.B.C()"),
                    std::string("new A.B.C()"));
}
