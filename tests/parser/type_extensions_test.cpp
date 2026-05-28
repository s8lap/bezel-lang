// Spec-derived parser tests for the §A.5 Type alternatives beyond the
// slice-4 `NamedType`-only subset:
//
//   TypeNoNullable ::= QualifiedName GenericArgs?
//                    | InlineArrayType
//                    | FunctionType
//                    | "(" ")"            /* unit */
//   InlineArrayType ::= "[" ConstIntExpr "owned"? "]" Type
//   FunctionType    ::= "(" (FuncParamType ("," FuncParamType)*)? ")"
//                       "->" "owned"? Type
//   FuncParamType   ::= FuncParamMods? (Identifier ":")? Type
//   FuncParamMods   ::= "ref" | "out" "owned"? | "owned"
//
// We assert via the typeToString round-trip: parse a field decl whose
// `:` annotation carries the type and read it back.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;
using bezel::test::typeToString;

namespace {

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

BEZEL_TEST(parser_type_ext, unit_type) {
    BEZEL_EXPECT_EQ(renderType("()"), std::string("()"));
}

BEZEL_TEST(parser_type_ext, zero_param_function) {
    BEZEL_EXPECT_EQ(renderType("() -> int"), std::string("() -> int"));
}

BEZEL_TEST(parser_type_ext, zero_param_function_owned_return) {
    BEZEL_EXPECT_EQ(
        renderType("() -> owned T"),
        std::string("() -> owned T"));
}

BEZEL_TEST(parser_type_ext, single_param_function) {
    BEZEL_EXPECT_EQ(
        renderType("(int) -> string"),
        std::string("(int) -> string"));
}

BEZEL_TEST(parser_type_ext, multi_param_function) {
    BEZEL_EXPECT_EQ(
        renderType("(int, string, bool) -> int"),
        std::string("(int, string, bool) -> int"));
}

BEZEL_TEST(parser_type_ext, function_with_named_params) {
    BEZEL_EXPECT_EQ(
        renderType("(x: int, y: int) -> int"),
        std::string("(x: int, y: int) -> int"));
}

BEZEL_TEST(parser_type_ext, function_with_param_modifiers) {
    BEZEL_EXPECT_EQ(
        renderType("(ref x: int, out y: int) -> ()"),
        std::string("(ref x: int, out y: int) -> ()"));
}

BEZEL_TEST(parser_type_ext, function_with_out_owned_param) {
    BEZEL_EXPECT_EQ(
        renderType("(out owned res: Resource) -> ()"),
        std::string("(out owned res: Resource) -> ()"));
}

BEZEL_TEST(parser_type_ext, function_returning_function) {
    BEZEL_EXPECT_EQ(
        renderType("(int) -> (int) -> int"),
        std::string("(int) -> (int) -> int"));
}

BEZEL_TEST(parser_type_ext, inline_array) {
    BEZEL_EXPECT_EQ(
        renderType("[10]int"),
        std::string("[10]int"));
}

BEZEL_TEST(parser_type_ext, inline_array_owned) {
    BEZEL_EXPECT_EQ(
        renderType("[16 owned]Resource"),
        std::string("[16 owned]Resource"));
}

BEZEL_TEST(parser_type_ext, inline_array_of_nullable_elements) {
    // Per the §A.5 inline-array note, the inner Type is parsed greedily —
    // `[3]int?` means an inline array of nullable int, not a nullable
    // inline array.
    BEZEL_EXPECT_EQ(
        renderType("[3]int?"),
        std::string("[3]int?"));
}

BEZEL_TEST(parser_type_ext, nested_inline_array) {
    BEZEL_EXPECT_EQ(
        renderType("[3][4]int"),
        std::string("[3][4]int"));
}

BEZEL_TEST(parser_type_ext, named_type_with_unit_in_generic) {
    BEZEL_EXPECT_EQ(
        renderType("Task<()>"),
        std::string("Task<()>"));
}

BEZEL_TEST(parser_type_ext, function_inside_generic) {
    BEZEL_EXPECT_EQ(
        renderType("Action<(int) -> int>"),
        std::string("Action<(int) -> int>"));
}

BEZEL_TEST(parser_type_ext, named_type_still_works) {
    // Regression: existing NamedType shapes must still parse + render.
    BEZEL_EXPECT_EQ(
        renderType("List<int>?"),
        std::string("List<int>?"));
}
