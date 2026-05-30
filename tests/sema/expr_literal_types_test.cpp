// Spec-derived tests for Sema slice 3 (literal-type subset). Each
// literal form should receive its canonical primitive TypeRef via
// SemaContext::exprType.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::sema::PrimitiveType;
using bezel::sema::TypeRef;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// Pull the first method-body expression (its first statement's RHS or
// the method's expression body) and return its annotated TypeRef.
const TypeRef* firstMethodFirstExprType(const bezel::test::SemaResult& r) {
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td) return nullptr;
    for (const auto& m : td->members) {
        const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
        if (!md) continue;
        if (md->bodyKind == bezel::ast::MethodBodyKind::Expression &&
            md->exprBody) {
            return r.ctx->exprType(*md->exprBody);
        }
        if (md->bodyKind == bezel::ast::MethodBodyKind::Block &&
            !md->stmts.empty()) {
            const auto* ds = dynamic_cast<const bezel::ast::DiscardStmt*>(
                md->stmts[0].get());
            if (ds && ds->value) return r.ctx->exprType(*ds->value);
            const auto* rs = dynamic_cast<const bezel::ast::ReturnStmt*>(
                md->stmts[0].get());
            if (rs && rs->value) return r.ctx->exprType(*rs->value);
        }
    }
    return nullptr;
}

} // namespace

BEZEL_TEST(sema_expr_lit, int_literal) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := 42; end M; end C;");
    auto* t = firstMethodFirstExprType(r);
    BEZEL_EXPECT_TRUE(t != nullptr);
    if (t) BEZEL_EXPECT_TRUE(t->primitive == PrimitiveType::Int);
}

BEZEL_TEST(sema_expr_lit, float_literal) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := 3.14; end M; end C;");
    auto* t = firstMethodFirstExprType(r);
    if (t) BEZEL_EXPECT_TRUE(t->primitive == PrimitiveType::Double);
}

BEZEL_TEST(sema_expr_lit, char_literal) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := 'x'; end M; end C;");
    auto* t = firstMethodFirstExprType(r);
    if (t) BEZEL_EXPECT_TRUE(t->primitive == PrimitiveType::Char);
}

BEZEL_TEST(sema_expr_lit, string_literal) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := \"hi\"; end M; end C;");
    auto* t = firstMethodFirstExprType(r);
    if (t) BEZEL_EXPECT_TRUE(t->primitive == PrimitiveType::String);
}

BEZEL_TEST(sema_expr_lit, verbatim_string_literal) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := @\"hi\"; end M; end C;");
    auto* t = firstMethodFirstExprType(r);
    if (t) BEZEL_EXPECT_TRUE(t->primitive == PrimitiveType::String);
}

BEZEL_TEST(sema_expr_lit, interp_string_literal) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := $\"hi {name}\"; end M; end C;");
    auto* t = firstMethodFirstExprType(r);
    if (t) BEZEL_EXPECT_TRUE(t->primitive == PrimitiveType::String);
}

BEZEL_TEST(sema_expr_lit, bool_literal) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := true; end M; end C;");
    auto* t = firstMethodFirstExprType(r);
    if (t) BEZEL_EXPECT_TRUE(t->primitive == PrimitiveType::Bool);
}

BEZEL_TEST(sema_expr_lit, null_literal_is_nullable_object) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := null; end M; end C;");
    auto* t = firstMethodFirstExprType(r);
    if (t) {
        BEZEL_EXPECT_TRUE(t->primitive == PrimitiveType::Object);
        BEZEL_EXPECT_TRUE(t->nullable);
    }
}

BEZEL_TEST(sema_expr_lit, unit_value) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := (); end M; end C;");
    auto* t = firstMethodFirstExprType(r);
    if (t) BEZEL_EXPECT_TRUE(t->kind == TypeRef::Kind::Unit);
}

BEZEL_TEST(sema_expr_lit, primitive_typeref_cache) {
    // Two int literals should share the same TypeRef* (the primitive cache).
    auto r = parseSemaAll(
        "module M; class C "
        "  public method M() _ := 1; end M; "
        "  public method N() _ := 2; end N; "
        "end C;");
    const TypeRef* a = nullptr;
    const TypeRef* b = nullptr;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    for (const auto& m : td->members) {
        const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
        if (!md || md->stmts.empty()) continue;
        const auto* ds = dynamic_cast<const bezel::ast::DiscardStmt*>(
            md->stmts[0].get());
        if (!ds || !ds->value) continue;
        if (!a) a = r.ctx->exprType(*ds->value);
        else if (!b) b = r.ctx->exprType(*ds->value);
    }
    BEZEL_EXPECT_TRUE(a != nullptr && a == b);
}

BEZEL_TEST(sema_expr_lit, recursion_into_binary_subexprs) {
    // Slice 3 doesn't compute a BinaryExpr's own type, but it should still
    // attach types to the leaf literal subexpressions.
    auto r = parseSemaAll(
        "module M; class C public method M() _ := 1 + 2; end M; end C;");
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(
        td->members[0].get());
    const auto* ds = dynamic_cast<const bezel::ast::DiscardStmt*>(
        md->stmts[0].get());
    const auto* be = dynamic_cast<const bezel::ast::BinaryExpr*>(ds->value.get());
    BEZEL_EXPECT_TRUE(be != nullptr);
    if (be && be->left) {
        auto* lt = r.ctx->exprType(*be->left);
        BEZEL_EXPECT_TRUE(lt && lt->primitive == PrimitiveType::Int);
    }
    if (be && be->right) {
        auto* rt = r.ctx->exprType(*be->right);
        BEZEL_EXPECT_TRUE(rt && rt->primitive == PrimitiveType::Int);
    }
    // Slice 3c typed the BinaryExpr itself — `int + int` is int.
    auto* whole = r.ctx->exprType(*ds->value);
    BEZEL_EXPECT_TRUE(whole && whole->primitive == PrimitiveType::Int);
}

// §5: an unsuffixed integer literal must fit in `int`; a larger value
// requires a suffix (it is not silently widened or wrapped).
BEZEL_TEST(sema_literal, unsuffixed_int_in_range_ok) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := 2147483647; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_literal, unsuffixed_int_too_large_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := 2147483648; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_literal, large_literal_with_long_suffix_ok) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := 5000000000L; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_literal, large_hex_literal_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := 0xFFFFFFFF; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
