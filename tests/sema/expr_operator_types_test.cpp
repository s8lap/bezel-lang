// Spec-derived tests for Sema slice 3c — binary/unary operator result
// typing per §17.1's result-type table.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::sema::PrimitiveType;
using bezel::sema::TypeRef;

namespace {

bezel::test::SemaResult parseSema(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

const bezel::ast::Expr* discardRhs(const bezel::test::SemaResult& r,
                                   std::size_t methodIdx = 0) {
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td) return nullptr;
    std::size_t seen = 0;
    for (const auto& m : td->members) {
        const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
        if (!md) continue;
        if (seen++ != methodIdx) continue;
        if (md->stmts.empty()) return nullptr;
        const auto* ds = dynamic_cast<const bezel::ast::DiscardStmt*>(
            md->stmts[0].get());
        return ds ? ds->value.get() : nullptr;
    }
    return nullptr;
}

const TypeRef* typeOf(const bezel::test::SemaResult& r) {
    const auto* e = discardRhs(r);
    return e ? r.ctx->exprType(*e) : nullptr;
}

} // namespace

BEZEL_TEST(sema_ops, equality_yields_bool) {
    auto r = parseSema(
        "module M; class C public method M() _ := 1 = 2; end M; end C;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Bool);
}

BEZEL_TEST(sema_ops, inequality_yields_bool) {
    auto r = parseSema(
        "module M; class C public method M() _ := 1 <> 2; end M; end C;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Bool);
}

BEZEL_TEST(sema_ops, less_than_yields_bool) {
    auto r = parseSema(
        "module M; class C public method M() _ := 1 < 2; end M; end C;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Bool);
}

BEZEL_TEST(sema_ops, logical_and_yields_bool) {
    auto r = parseSema(
        "module M; class C public method M() _ := true and false; end M; end C;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Bool);
}

// #1040 / §6: `and`/`or`/`not` have boolean-only semantics. A NON-bool operand
// was previously accepted (result typed bool) and CRASHED codegen — the
// short-circuit PHI hit an LLVM "operands must be same type" assertion
// (`int(u and 255)`), and `not 5` emitted malformed IR. They must be rejected.
BEZEL_TEST(sema_ops, and_nonbool_operand_rejected_1040) {
    auto r = parseSema(
        "module M; class C public method M(): int "
        "  var u: uint := 5; return int(u and 255); end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
BEZEL_TEST(sema_ops, or_nonbool_operand_rejected_1040) {
    auto r = parseSema(
        "module M; class C public method M(): int "
        "  return int(5 or 3); end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
BEZEL_TEST(sema_ops, not_nonbool_operand_rejected_1040) {
    auto r = parseSema(
        "module M; class C public method M(): int "
        "  var u: uint := 5; return (1 if (not u) else 0); end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
// Positive controls — valid bool logical operators must STILL be accepted.
BEZEL_TEST(sema_ops, and_bool_operands_still_accepted_1040) {
    auto r = parseSema(
        "module M; class C public method M(): int "
        "  var x: int := 5; return (1 if (x > 0 and x < 10) else 0); end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
BEZEL_TEST(sema_ops, not_bool_operand_still_accepted_1040) {
    auto r = parseSema(
        "module M; class C public method M(): int "
        "  return (1 if (not true) else 0); end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ops, arithmetic_keeps_operand_type) {
    auto r = parseSema(
        "module M; class C public method M() _ := 1 + 2; end M; end C;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
}

BEZEL_TEST(sema_ops, multiplication_keeps_operand_type) {
    auto r = parseSema(
        "module M; class C public method M() _ := 3 * 4; end M; end C;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
}

BEZEL_TEST(sema_ops, mod_keeps_operand_type) {
    auto r = parseSema(
        "module M; class C public method M() _ := 7 mod 2; end M; end C;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
}

BEZEL_TEST(sema_ops, unary_minus_keeps_type) {
    auto r = parseSema(
        "module M; class C public method M() _ := -5; end M; end C;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
}

BEZEL_TEST(sema_ops, unary_not_yields_bool) {
    auto r = parseSema(
        "module M; class C public method M() _ := not true; end M; end C;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Bool);
}

BEZEL_TEST(sema_ops, shift_keeps_operand_type) {
    auto r = parseSema(
        "module M; class C public method M() _ := 1 << 3; end M; end C;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
}

BEZEL_TEST(sema_ops, nested_binary_chain) {
    // 1 + 2 + 3 — left-associative; outer + on (1+2)+3 → int.
    auto r = parseSema(
        "module M; class C public method M() _ := 1 + 2 + 3; end M; end C;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
}

BEZEL_TEST(sema_ops, comparison_chain_in_logical) {
    auto r = parseSema(
        "module M; class C public method M() "
        "  _ := 1 < 2 and 3 > 2; "
        "end M; end C;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Bool);
}

// §17.1 L12 null-coalescing: `a ?? b` has the *non-nullable* form of the
// LHS as its result type (spec § null-coalescing: the RHS must be
// compatible with the non-nullable form of the left).
BEZEL_TEST(sema_ops, coalesce_yields_nonnull_lhs) {
    auto r = parseSema(
        "module M; class C public method M(a: int?) "
        "  _ := a ?? 2; "
        "end M; end C;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int && !t->nullable);
}

// Regression: the result stays typed even when the RHS is a postfix-`!`
// expression (whose own type the checker does not record). Deriving the
// `??` type from the LHS — not the RHS — keeps the whole expression typed.
BEZEL_TEST(sema_ops, coalesce_typed_when_rhs_is_bang) {
    auto r = parseSema(
        "module M; class C public method M(a: int?, b: int?) "
        "  _ := a ?? b!; "
        "end M; end C;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int && !t->nullable);
}

// Reference-typed coalescing yields the non-nullable named type.
BEZEL_TEST(sema_ops, coalesce_reference_type_nonnull) {
    auto r = parseSema(
        "module M; "
        "class C public method M(a: T?) _ := a ?? new T(); end M; end C; "
        "class T end T;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named && !t->nullable);
}

// §17.12 / §2553: postfix `!` (null-forgiving) yields the non-nullable
// form of the operand's type — so the result is typed and usable (#206).
BEZEL_TEST(sema_ops, postfix_bang_yields_nonnull) {
    auto r = parseSema(
        "module M; "
        "class C public method M(a: T?) _ := a!; end M; end C; "
        "class T end T;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named && !t->nullable);
}

// §7.5 null-conditional member access `x?.M` — the result type is always
// nullable (the access yields null when the target is null).
BEZEL_TEST(sema_ops, null_conditional_member_is_nullable) {
    auto r = parseSema(
        "module M; "
        "class C public method M(t: T) _ := t?.n; end M; end C; "
        "class T public n: N := new N(); end T; "
        "class N end N;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named && t->nullable);
}

// §20: the `if` expression has the common type of its branches — recording
// it lets member access on the result (`(a if c else b).M`) resolve.
BEZEL_TEST(sema_ops, conditional_expression_takes_branch_type) {
    auto r = parseSema(
        "module M; "
        "class C public method M(a: N, b: N, c: bool) _ := (a if c else b); "
        "end M; end C; class N end N;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named && !t->nullable);
}

// §10: an enum-constant access `E.Green` has the enum type — so a local
// inferred from it carries the enum type (needed e.g. for a `case`
// scrutinee to resolve its arms' enum-constant patterns).
BEZEL_TEST(sema_ops, enum_constant_access_has_enum_type) {
    auto r = parseSema(
        "module M; "
        "class C public method M() _ := E.Green; end M; end C; "
        "enum E Red, Green, Blue; end E;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named &&
                      t->declaration && t->declaration->name == "E");
}

// §17.4: `x as T` has the nullable form of T; `x is T` is bool. Recording
// the `as` type lets member access on its result (`(x as T)!.M`) resolve.
BEZEL_TEST(sema_ops, as_cast_is_nullable_target_type) {
    auto r = parseSema(
        "module M; class C public method M(o: N) _ := (o as N); end M; "
        "end C; class N end N;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named && t->nullable);
}

BEZEL_TEST(sema_ops, is_test_is_bool) {
    auto r = parseSema(
        "module M; class C public method M(o: N) _ := (o is N); end M; "
        "end C; class N end N;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Bool);
}

// §19: a `case` expression's type is its arm bodies' common type, so member
// access on the result resolves.
BEZEL_TEST(sema_ops, case_expression_takes_arm_type) {
    auto r = parseSema(
        "module M; class C public method M(k: int, a: N, b: N) "
        "  _ := case k 1 => a, _ => b, end; "
        "end M; end C; class N end N;");
    auto* t = typeOf(r);
    BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named && !t->nullable);
}
