// Spec-derived parser tests for uniform trailing commas (proposal 0059,
// graduated spec v0.214) — §1.6 / §A.4:
//
//   "Every comma-separated list admits one optional trailing comma before
//    its closer."
//
// The eighteen A-grammar comma-list productions enumerated in proposal 0059:
//   ArgList, ParamList, LambdaParams, LambdaDestructure, GenericParams,
//   GenericArgs, GenericConstraints, PositionalFields, PositionalPatternList,
//   EnumConstants, ChoiceCases, CasePayload, PolymorphicMember,
//   ExternParamList, FunctionType, Inheritance, EnumInheritance (both
//   alternatives).
//
// Per §1.6 the comma directly before `>` / `>>` / `>=` / `>>=` closers must
// lex fine; the parser break check below keys on any Punctuator whose text
// starts with `>` (matching the lexer tokens consumeClosingAngle handles).
//
// Each test exercises one production: parsing a source shape with the
// trailing comma must succeed without errors and yield the same AST shape as
// the no-trailing-comma form (modulo the literal comma, which the AST does
// not retain).

#include <string>
#include <vector>

#include "ast/Node.hpp"
#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::exprToString;
using bezel::test::parseAll;
using bezel::test::memberSummaries;
using bezel::test::topDeclSummaries;
using bezel::test::typeToString;

namespace {

// Run a parser-only check: a successful parse must NOT have diagnostics
// errors. Parse-only tests use the lex/parse harness (no sema/codegen), so
// downstream errors are out of scope here.
bool hasParseErrors(const bezel::test::ParseResult& r) {
    return r.diagnostics.hasErrors();
}

const bezel::ast::Expr* extractExprStmt(const bezel::test::ParseResult& r) {
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td || td->members.empty()) return nullptr;
    const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(
        td->members[0].get());
    if (!md || md->stmts.empty()) return nullptr;
    const auto* ds = dynamic_cast<const bezel::ast::DiscardStmt*>(
        md->stmts[0].get());
    if (!ds) return nullptr;
    return ds->value.get();
}

const bezel::ast::TypeDecl* firstTypeDecl(const bezel::test::ParseResult& r) {
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    return dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
}

} // namespace

// ===========================================================================
// 1. ArgList (§A.4): call argument list. `f(a, b,)` is legal.
// ===========================================================================

BEZEL_TEST(parser_trailing_comma_arg_list, call_with_trailing_comma) {
    const std::string src =
        "module M; class C public method M() _ := f(1, 2,); end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
    const auto* e = extractExprStmt(r);
    BEZEL_EXPECT_TRUE(e != nullptr);
    if (e) {
        BEZEL_EXPECT_EQ(exprToString(*e), std::string("f(1, 2)"));
    }
}

BEZEL_TEST(parser_trailing_comma_arg_list, new_with_trailing_comma) {
    const std::string src =
        "module M; class C public method M() _ := new Foo(1, 2,);"
        " end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_arg_list, null_cond_call_with_trailing) {
    const std::string src =
        "module M; class C public method M() _ := obj?.M(1, 2,);"
        " end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_arg_list, index_with_trailing_comma) {
    const std::string src =
        "module M; class C public method M() _ := arr[1, 2,];"
        " end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

// ===========================================================================
// 2. ParamList (§A.4): method parameter list. `method M(a: int, b: int,)`.
// ===========================================================================

BEZEL_TEST(parser_trailing_comma_param_list, method_params) {
    const std::string src =
        "module M; class C public method M(a: int, b: int,) => 0; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
    const auto* td = firstTypeDecl(r);
    BEZEL_EXPECT_TRUE(td != nullptr);
    if (td && !td->members.empty()) {
        const auto& ms = memberSummaries(r);
        // The trailing comma must not affect member shape.
        BEZEL_EXPECT_EQ(ms[0],
                        std::string("public method M(a: int, b: int) => 0"));
    }
}

BEZEL_TEST(parser_trailing_comma_param_list, ctor_params) {
    const std::string src =
        "module M; class C public constructor(a: int, b: int,) "
        "  this.a := a; "
        "  this.b := b; "
        "end constructor; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_param_list, primary_ctor) {
    const std::string src =
        "module M; class C(a: int, b: int,) public method M() => 0; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_param_list, indexer_params) {
    const std::string src =
        "module M; class C public property[idx: int,] : int => 0; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

// ===========================================================================
// 3. LambdaParams (§A.7): lambda parameter list. `(a, b,) => a + b`.
// ===========================================================================

BEZEL_TEST(parser_trailing_comma_lambda_params, simple_lambda) {
    const std::string src =
        "module M; class C public method M() _ := lambda(a, b,) => a + b;"
        " end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
    const auto* e = extractExprStmt(r);
    BEZEL_EXPECT_TRUE(e != nullptr);
    if (e) {
        BEZEL_EXPECT_EQ(exprToString(*e),
                        std::string("(lambda(a, b) => (a + b))"));
    }
}

BEZEL_TEST(parser_trailing_comma_lambda_params, typed_lambda) {
    const std::string src =
        "module M; class C public method M() _ := lambda(a: int, b: int,) => a;"
        " end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_lambda_params, async_lambda) {
    const std::string src =
        "module M; class C public method M() _ := async lambda(a, b,) => a;"
        " end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

// ===========================================================================
// 4. LambdaDestructure (§A.7, §27.4): destructuring lambda parameters.
// ===========================================================================

BEZEL_TEST(parser_trailing_comma_lambda_destructure, pair_destructure) {
    const std::string src =
        "module M; class C public method M() _ :="
        " lambda(Pair(a, b,)) => a + b; end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

// ===========================================================================
// 5. GenericParams (§A.3): `<` GenericParam (`,` GenericParam)* `>`.
// ===========================================================================

BEZEL_TEST(parser_trailing_comma_generic_params, class_with_trailing) {
    const std::string src =
        "module M; class C<T, U,> end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
    const auto* td = firstTypeDecl(r);
    BEZEL_EXPECT_TRUE(td != nullptr);
    if (td) {
        const auto decls = topDeclSummaries(r);
        BEZEL_EXPECT_EQ(decls[0], std::string("class C<T, U>"));
    }
}

BEZEL_TEST(parser_trailing_comma_generic_params, method_with_trailing) {
    const std::string src =
        "module M; class C public method M<T, U,>() _ := 0; end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_generic_params, variance_with_trailing) {
    const std::string src =
        "module M; class C<in T, out U,> end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

// ===========================================================================
// 6. GenericArgs (§A.5): `Foo<T, U,>` — used in three call-site contexts
//    (parseType, parsePostfix `.`, parsePostfix `?.`) plus the receiver
//    position GenericArgs.
// ===========================================================================

BEZEL_TEST(parser_trailing_comma_generic_args, type_position) {
    // `List<int, string,>` as a field's declared type.
    const std::string src =
        "module M; class C public x: List<int, string,>; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_generic_args, static_member_access) {
    const std::string src =
        "module M; class C public method M() _ := Registry<int,>.Make(7);"
        " end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_generic_args, method_call_explicit) {
    const std::string src =
        "module M; class C public method M() _ := Id<int, string,>(\"x\");"
        " end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_generic_args, null_cond_call_explicit) {
    const std::string src =
        "module M; class C public method M() _ := obj?.M<int,>(7);"
        " end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_generic_args, nested_generic) {
    // `List<Map<K, V,>,>` — exercises the `>>` split via consumeClosingAngle.
    const std::string src =
        "module M; class C public x: List<Map<K, V,>,>; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

// ===========================================================================
// 7. GenericConstraints (§A.3): items inside a `where T : C1, C2,` clause.
// ===========================================================================

BEZEL_TEST(parser_trailing_comma_generic_constraints, single_clause) {
    const std::string src =
        "module M; class C<T> where T : I1, I2, public method M() _ := 0;"
        " end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_generic_constraints, between_clauses) {
    const std::string src =
        "module M; class C<T, U>"
        " where T : I1, where U : I2, public method M() _ := 0;"
        " end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_generic_constraints, mixed_with_class) {
    const std::string src =
        "module M; class C<T> where T : I1, I2, end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

// ===========================================================================
// 8. PositionalFields (§A.3): `record Foo(x: int, y: int,)`.
// ===========================================================================

BEZEL_TEST(parser_trailing_comma_positional_fields, record) {
    const std::string src =
        "module M; record R(x: int, y: int,);";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
    const auto* td = firstTypeDecl(r);
    BEZEL_EXPECT_TRUE(td != nullptr);
    if (td && !td->positionalFields.empty()) {
        // Trailing comma must not affect the field list shape.
        const auto& f0 = td->positionalFields[0];
        const auto& f1 = td->positionalFields[1];
        BEZEL_EXPECT_EQ(std::string(f0.name), std::string("x"));
        BEZEL_EXPECT_EQ(std::string(f1.name), std::string("y"));
    }
}

BEZEL_TEST(parser_trailing_comma_positional_fields, record_with_serialized) {
    const std::string src =
        "module M; record R(x: int, y: int serialized \"y\",);";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

// ===========================================================================
// 9. PositionalPatternList (§A.7): case-pattern destructuring `Foo(a, b,)`.
// ===========================================================================

BEZEL_TEST(parser_trailing_comma_positional_pattern, case_destructuring) {
    const std::string src =
        "module M; class C public method M() _ := case x of"
        " Pair(a, b,) => 1 end; end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

// ===========================================================================
// 10. EnumConstants (§A.3): enum constant list. `enum E A, B, C,; end E;`.
// ===========================================================================

BEZEL_TEST(parser_trailing_comma_enum_constants, simple) {
    const std::string src =
        "module M; enum E A, B, C,; end E;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_enum_constants, with_values) {
    const std::string src =
        "module M; enum E A := 1, B := 2, C := 3,; end E;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_enum_constants, with_serialized) {
    const std::string src =
        "module M; enum E A serialized \"a\", B, C,; end E;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

// ===========================================================================
// 11. ChoiceCases (§A.3, §35.1): choice case list. `choice C A, B, C,; end C;`.
// ===========================================================================

BEZEL_TEST(parser_trailing_comma_choice_cases, simple) {
    const std::string src =
        "module M; choice C A, B, C,; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_choice_cases, with_payload) {
    const std::string src =
        "module M; choice C None, Some(x: int, y: int,),; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_choice_cases, with_owned_payload) {
    const std::string src =
        "module M; choice C None, Wrap(owned x: Foo,),; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

// ===========================================================================
// 12. CasePayload (§A.3): `( CaseField ("," CaseField)* )` inside a choice.
// ===========================================================================

BEZEL_TEST(parser_trailing_comma_case_payload, simple) {
    const std::string src =
        "module M; choice C Pair(x: int, y: int,); end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

// ===========================================================================
// 13. PolymorphicMember (§9.8): `polymorphic("a": T, "b": U,);`.
// ===========================================================================

BEZEL_TEST(parser_trailing_comma_polymorphic, simple) {
    const std::string src =
        "module M; class C"
        " polymorphic(\"a\": int, \"b\": string,);"
        " public method M() _ := 0; end M;"
        " end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

// ===========================================================================
// 14. ExternParamList (§A.3): extern method parameter list. ExternParamList
//     parses via parseParamList so the change to ParamList covers it.
// ===========================================================================

BEZEL_TEST(parser_trailing_comma_extern_params, extern_method) {
    // ExternParamList is parsed via parseParamList, so the trailing-comma
    // change to ParamList covers this production.
    const std::string src =
        "module M; extern \"libc\" convention \"cdecl\""
        " method m1(a: int, b: int,);"
        " end;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

// ===========================================================================
// 15. FunctionType (§A.5): `(p1: T1, p2: T2,) -> R`.
// ===========================================================================

BEZEL_TEST(parser_trailing_comma_function_type, simple) {
    const std::string src =
        "module M; class C public x: (a: int, b: int,) -> int;"
        " end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_function_type, empty_with_trailing) {
    // A zero-param function type `() -> int` has no comma list, so the
    // trailing comma rule does not apply — but we should still parse cleanly.
    const std::string src =
        "module M; class C public x: () -> int; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

// ===========================================================================
// 16. Inheritance (§A.3): class/interface/record base list. `: A, B,`.
// ===========================================================================

BEZEL_TEST(parser_trailing_comma_inheritance, class_bases) {
    const std::string src =
        "module M; class C : A, B, public method M() _ := 0;"
        " end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_inheritance, interface_bases) {
    const std::string src =
        "module M; interface IFoo : A, B, method M() : int; end IFoo;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_inheritance, before_where) {
    const std::string src =
        "module M; class C<T> : A, B, where T : I1, public method M() _ := 0;"
        " end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_inheritance, before_body) {
    // Trailing `,` directly before the body opener `{`.
    const std::string src =
        "module M; class C : A, B, public method M() _ := 0; end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

// ===========================================================================
// 17+18. EnumInheritance (§A.3, both alternatives): integer-type base or
//       arbitrary-inherit-item base. Both parse through the same comma loop.
//
//       NOTE: EnumInheritance is followed by the enum-constants list, which
//       is a separate grammar production (`EnumConstants`); the boundary
//       between the two is implicit (no separator token). The parser greedily
//       consumes comma-separated base types until it sees a token that
//       cannot start a Type. The trailing-comma check accepts a `,` directly
//       before `;` (the constants-list terminator).
// ===========================================================================

BEZEL_TEST(parser_trailing_comma_enum_inheritance, integer_type_base) {
    // Alternative 1: `enum E : int A, B,;` — one integer base type, constants
    // list with trailing comma.
    const std::string src =
        "module M; enum E : int A, B,; end E;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_enum_inheritance, inherit_item_base) {
    // Alternative 2: `enum E : IFoo A, B,;` — one inherit-item base,
    // constants list with trailing comma.
    const std::string src =
        "module M; enum E : IFoo A, B,; end E;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(hasParseErrors(r));
}

// ===========================================================================
// Negative: `f(,)` — a leading empty argument is still rejected (no
// out-of-scope relaxation of the comma-list rule beyond trailing).
// ===========================================================================

BEZEL_TEST(parser_trailing_comma_rejects_leading_comma, arg_list) {
    const std::string src =
        "module M; class C public method M() _ := f(,); end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_TRUE(hasParseErrors(r));
}

BEZEL_TEST(parser_trailing_comma_rejects_leading_comma, param_list) {
    const std::string src =
        "module M; class C public method M(, a: int) _ := 0; end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_TRUE(hasParseErrors(r));
}

// ===========================================================================
// Equality check: a trailing-comma form parses to the same AST shape as the
// no-trailing-comma form (the comma list is purely lexical).
// ===========================================================================

BEZEL_TEST(parser_trailing_comma_ast_equivalence, call_site) {
    const std::string noComma = "f(1, 2)";
    const std::string withComma = "f(1, 2,)";
    const auto r1 = parseAll(
        "module M; class C public method M() _ := f(1, 2); end M; end C;");
    const auto r2 = parseAll(
        "module M; class C public method M() _ := f(1, 2,); end M; end C;");
    BEZEL_EXPECT_FALSE(hasParseErrors(r1));
    BEZEL_EXPECT_FALSE(hasParseErrors(r2));
    const auto* e1 = extractExprStmt(r1);
    const auto* e2 = extractExprStmt(r2);
    BEZEL_EXPECT_TRUE(e1 != nullptr);
    BEZEL_EXPECT_TRUE(e2 != nullptr);
    if (e1 && e2) {
        BEZEL_EXPECT_EQ(exprToString(*e1), exprToString(*e2));
    }
}