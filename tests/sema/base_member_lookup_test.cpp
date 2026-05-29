// Spec-derived tests for Sema slice S5 — base-type member lookup.
//
// Per §12 (inheritance and dispatch) with §11.1/§11.2 (base class and
// implemented interfaces): a member access on a Derived instance can
// resolve to a member declared on a base type (class or interface).
// Slice S5 adds breadth-first lookup over the base-type chain.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::sema::FieldSymbol;
using bezel::sema::MethodSymbol;
using bezel::sema::PrimitiveType;
using bezel::sema::PropertySymbol;
using bezel::sema::TypeRef;
using bezel::sema::TypeSymbol;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

const bezel::ast::MemberAccessExpr* findMemberAccess(
    const bezel::test::SemaResult& r, std::string_view methodName) {
    if (!r.sourceFile) return nullptr;
    for (const auto& topD : r.sourceFile->topDecls) {
        const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(topD.get());
        if (!td) continue;
        for (const auto& m : td->members) {
            const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(
                m.get());
            if (!md || md->name != methodName) continue;
            if (md->bodyKind == bezel::ast::MethodBodyKind::Expression &&
                md->exprBody) {
                return dynamic_cast<const bezel::ast::MemberAccessExpr*>(
                    md->exprBody.get());
            }
            if (md->bodyKind == bezel::ast::MethodBodyKind::Block) {
                for (const auto& s : md->stmts) {
                    if (!s) continue;
                    if (auto* ds =
                            dynamic_cast<const bezel::ast::DiscardStmt*>(
                                s.get())) {
                        if (auto* ma =
                                dynamic_cast<const bezel::ast::MemberAccessExpr*>(
                                    ds->value.get())) return ma;
                    }
                }
            }
        }
    }
    return nullptr;
}

} // namespace

BEZEL_TEST(sema_base, derived_inherits_base_field) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public _x: int; end Base; "
        "class Derived : Base "
        "  public method M() _ := this._x; end M; "
        "end Derived;");
    auto* ma = findMemberAccess(r, "M");
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<FieldSymbol*>(sym) != nullptr);
        auto* t = r.ctx->exprType(*ma);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

BEZEL_TEST(sema_base, derived_inherits_base_method) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public method Helper(): int _ := 1; end Helper; end Base; "
        "class Derived : Base "
        "  public method M() _ := this.Helper; end M; "
        "end Derived;");
    auto* ma = findMemberAccess(r, "M");
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<MethodSymbol*>(sym) != nullptr);
    }
}

BEZEL_TEST(sema_base, derived_member_shadows_base) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public _x: int; end Base; "
        "class Derived : Base "
        "  public _x: int; "
        "  public method M() _ := this._x; end M; "
        "end Derived;");
    auto* ma = findMemberAccess(r, "M");
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        // Derived's own _x wins over Base's.
        auto* fs = dynamic_cast<FieldSymbol*>(sym);
        BEZEL_EXPECT_TRUE(fs != nullptr);
        if (fs) {
            // parent should be Derived, not Base.
            BEZEL_EXPECT_TRUE(fs->parent != nullptr);
            if (fs->parent) BEZEL_EXPECT_TRUE(fs->parent->name == "Derived");
        }
    }
}

BEZEL_TEST(sema_base, transitive_inheritance_through_two_levels) {
    auto r = parseSemaAll(
        "module M; "
        "class A public _y: int; end A; "
        "class B : A end B; "
        "class C : B "
        "  public method M() _ := this._y; end M; "
        "end C;");
    auto* ma = findMemberAccess(r, "M");
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<FieldSymbol*>(sym) != nullptr);
    }
}

BEZEL_TEST(sema_base, interface_member_resolves) {
    // Implements clause (after the colon) — interface members are
    // looked up via the same base-types path.
    auto r = parseSemaAll(
        "module M; "
        "interface IDoer public method Do(): int; end IDoer; "
        "class C : IDoer "
        "  public method Do(): int _ := 1; end Do; "
        "  public method M() _ := this.Do; end M; "
        "end C;");
    auto* ma = findMemberAccess(r, "M");
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<MethodSymbol*>(sym) != nullptr);
    }
}
