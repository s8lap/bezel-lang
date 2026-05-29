// Spec-derived tests for Sema slice 3e — parameter typing on
// identifier use. An IdentifierExpr resolving to a ParameterSymbol
// should carry the parameter's declared type via exprType. Combined
// with slice 3d, this lets `p.X` resolve member `X` on the parameter's
// type.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::sema::FieldSymbol;
using bezel::sema::ParameterSymbol;
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

// Find the first method body's first IdentifierExpr discard
// (`_ := <expr>;` form). Returns null if not present.
const bezel::ast::IdentifierExpr* firstDiscardIdentExpr(
    const bezel::test::SemaResult& r) {
    if (!r.sourceFile) return nullptr;
    for (const auto& topD : r.sourceFile->topDecls) {
        const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(topD.get());
        if (!td) continue;
        for (const auto& m : td->members) {
            const auto* md =
                dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
            if (!md) continue;
            if (md->bodyKind == bezel::ast::MethodBodyKind::Expression &&
                md->exprBody) {
                if (auto* id =
                        dynamic_cast<const bezel::ast::IdentifierExpr*>(
                            md->exprBody.get())) return id;
            }
            if (md->bodyKind == bezel::ast::MethodBodyKind::Block) {
                for (const auto& s : md->stmts) {
                    if (!s) continue;
                    if (auto* ds =
                            dynamic_cast<const bezel::ast::DiscardStmt*>(
                                s.get())) {
                        if (auto* id =
                                dynamic_cast<const bezel::ast::IdentifierExpr*>(
                                    ds->value.get())) return id;
                    }
                }
            }
        }
    }
    return nullptr;
}

const bezel::ast::MemberAccessExpr* firstDiscardMemberAccess(
    const bezel::test::SemaResult& r) {
    if (!r.sourceFile) return nullptr;
    for (const auto& topD : r.sourceFile->topDecls) {
        const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(topD.get());
        if (!td) continue;
        for (const auto& m : td->members) {
            const auto* md =
                dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
            if (!md || md->bodyKind != bezel::ast::MethodBodyKind::Block)
                continue;
            for (const auto& s : md->stmts) {
                if (!s) continue;
                if (auto* ds =
                        dynamic_cast<const bezel::ast::DiscardStmt*>(s.get())) {
                    if (auto* ma =
                            dynamic_cast<const bezel::ast::MemberAccessExpr*>(
                                ds->value.get())) return ma;
                }
            }
        }
    }
    return nullptr;
}

} // namespace

BEZEL_TEST(sema_param, primitive_param_identifier_typed) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(p: int) _ := p; end M; "
        "end C;");
    auto* id = firstDiscardIdentExpr(r);
    BEZEL_EXPECT_TRUE(id != nullptr);
    if (id) {
        auto* sym = r.ctx->exprSymbol(*id);
        BEZEL_EXPECT_TRUE(dynamic_cast<ParameterSymbol*>(sym) != nullptr);
        auto* t = r.ctx->exprType(*id);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

BEZEL_TEST(sema_param, named_param_identifier_typed) {
    auto r = parseSemaAll(
        "module M; "
        "class D end D; "
        "class C "
        "  public method M(d: D) _ := d; end M; "
        "end C;");
    auto* id = firstDiscardIdentExpr(r);
    if (id) {
        auto* t = r.ctx->exprType(*id);
        BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named);
    }
}

BEZEL_TEST(sema_param, param_field_member_access_resolves) {
    auto r = parseSemaAll(
        "module M; "
        "class D public _y: int; end D; "
        "class C "
        "  public method M(d: D) _ := d._y; end M; "
        "end C;");
    auto* ma = firstDiscardMemberAccess(r);
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<FieldSymbol*>(sym) != nullptr);
        auto* t = r.ctx->exprType(*ma);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

BEZEL_TEST(sema_param, owned_param_modifier_carried) {
    // Slice 3e: the modifier doesn't change the type-carrying behavior;
    // an `owned T` param still has type T on identifier use.
    auto r = parseSemaAll(
        "module M; "
        "class D end D; "
        "class C "
        "  public method M(owned d: D) _ := d; end M; "
        "end C;");
    auto* id = firstDiscardIdentExpr(r);
    if (id) {
        auto* sym = r.ctx->exprSymbol(*id);
        auto* par = dynamic_cast<ParameterSymbol*>(sym);
        BEZEL_EXPECT_TRUE(par != nullptr);
        if (par) BEZEL_EXPECT_TRUE(par->modifier == "owned");
        auto* t = r.ctx->exprType(*id);
        BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named);
    }
}

BEZEL_TEST(sema_param, constructor_param_identifier_typed) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public constructor(p: int) _ := p; end constructor; "
        "end C;");
    // First ctor's first stmt.
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    const auto* cd = dynamic_cast<const bezel::ast::ConstructorDecl*>(
        td->members[0].get());
    const auto* ds = dynamic_cast<const bezel::ast::DiscardStmt*>(
        cd->body[0].get());
    const auto* id =
        dynamic_cast<const bezel::ast::IdentifierExpr*>(ds->value.get());
    BEZEL_EXPECT_TRUE(id != nullptr);
    if (id) {
        auto* t = r.ctx->exprType(*id);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}
