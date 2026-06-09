// Spec-derived parser tests for nullable function types — §27.2, grammar A.5.
//
// §27.2 specifies:
//   var callback: ((int) -> bool)?;      // nullable function returning bool
//   var nullableReturn: (int) -> bool?;  // non-null function returning bool?
//
// "The parentheses around the function type are required before `?`. Without
//  them, the `?` would attach to the return type."
//
// Prior to fix #584, the outer `(...)` around a function type was parsed as
// a single-element function-type parameter list, and the missing `->` after
// the outer `)` (before `?`) generated: "expected '->' after function-type
// parameter list".
//
// The fix: when the outer `(FunctionType)` yields exactly one un-named,
// un-modified param whose type is a FunctionType, and `?` follows instead of
// `->`, the outer parens are a grouping operator (per §27.2); unwrap the
// inner function type and apply the nullable suffix.
//
// Grammar (A.5):
//   Type            ::= TypeNoNullable "?"?
//   TypeNoNullable  ::= ... | FunctionType | ...
//   FunctionType    ::= "(" (FuncParamType ("," FuncParamType)*)? ")" "->"
//                        "owned"? Type

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;
using bezel::test::typeToString;

namespace {

// Parse a field-declaration type annotation and round-trip it through
// typeToString. Same helper as type_extensions_test.cpp.
std::string renderType(const std::string& typeText) {
    const std::string src =
        "module M; class C public x: " + typeText + "; end C;";
    auto r = parseAll(src);
    if (r.diagnostics.hasErrors()) return "<errors>";
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td || td->members.empty()) return "<no-member>";
    const auto* fd = dynamic_cast<const bezel::ast::FieldDecl*>(
        td->members[0].get());
    if (!fd || !fd->type) return "<not-field>";
    return typeToString(*fd->type);
}

} // namespace

// --- §27.2: nullable function type `((T) -> R)?` ---------------------------

// §27.2 example 1: `((int) -> bool)?` — nullable function returning bool.
// Before fix #584: "expected '->' after function-type parameter list".
BEZEL_TEST(parser_nullable_func_type_584, single_param_spec_27_2_ex1) {
    BEZEL_EXPECT_EQ(
        renderType("((int) -> bool)?"),
        std::string("((int) -> bool)?"));
}

// §27.2: two-param variant.
BEZEL_TEST(parser_nullable_func_type_584, two_param_spec_27_2) {
    BEZEL_EXPECT_EQ(
        renderType("((int, string) -> int)?"),
        std::string("((int, string) -> int)?"));
}

// §27.2: zero-param nullable function type.
BEZEL_TEST(parser_nullable_func_type_584, zero_param_spec_27_2) {
    BEZEL_EXPECT_EQ(
        renderType("(() -> bool)?"),
        std::string("(() -> bool)?"));
}

// §27.2: nullable function with ref parameter.
BEZEL_TEST(parser_nullable_func_type_584, ref_param_spec_27_2) {
    BEZEL_EXPECT_EQ(
        renderType("((ref x: int) -> bool)?"),
        std::string("((ref x: int) -> bool)?"));
}

// --- §27.2 contrast: `?` on return type vs. `?` on the function itself ------

// §27.2 example 2: `(int) -> bool?` — non-null function returning nullable
// bool. This must NOT be affected by the fix (regression check).
BEZEL_TEST(parser_nullable_func_type_584, nullable_return_distinct_from_nullable_func) {
    // `(int) -> bool?` — `?` attaches to `bool` (the return type), not to
    // the function type. The result is a non-nullable function whose return
    // type is nullable. typeToString renders without outer parens.
    BEZEL_EXPECT_EQ(
        renderType("(int) -> bool?"),
        std::string("(int) -> bool?"));
    // Confirm this is NOT the same string as the nullable-function form.
    const std::string funcNullable  = renderType("((int) -> bool)?");
    const std::string returnNullable = renderType("(int) -> bool?");
    BEZEL_EXPECT_FALSE(funcNullable == returnNullable);
}

// Regression: non-nullable function type must still parse and render.
BEZEL_TEST(parser_nullable_func_type_584, non_nullable_function_type_regression) {
    BEZEL_EXPECT_EQ(
        renderType("(int) -> bool"),
        std::string("(int) -> bool"));
}

// Regression: unit type still works.
BEZEL_TEST(parser_nullable_func_type_584, unit_type_regression) {
    BEZEL_EXPECT_EQ(renderType("()"), std::string("()"));
}

// Regression: nullable named type still works.
BEZEL_TEST(parser_nullable_func_type_584, nullable_named_type_regression) {
    BEZEL_EXPECT_EQ(renderType("int?"), std::string("int?"));
}

// Regression: zero-param function type still works (non-nullable).
BEZEL_TEST(parser_nullable_func_type_584, zero_param_non_nullable_regression) {
    BEZEL_EXPECT_EQ(renderType("() -> int"), std::string("() -> int"));
}

// §27.2: var-decl with nullable callback type — the §27.2 spec example.
// Tests the full LocalDecl path, not just field annotations.
BEZEL_TEST(parser_nullable_func_type_584, var_decl_nullable_callback_spec_27_2) {
    // Parses `var callback: ((int) -> bool)? := null;` inside a method body.
    const std::string src =
        "module M; class C "
        "  public method run() "
        "    var callback: ((int) -> bool)? := null; "
        "  end run; "
        "end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
