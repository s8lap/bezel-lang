// Spec-derived tests for Sema slice 3h — CallExpr return-type typing.
// When the callee resolves to a MethodSymbol, the CallExpr inherits
// the method's resolved return type. Methods declared without a `:`
// return type take the unit type per §A.4.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::sema::FieldSymbol;
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

// Walk method bodies to find the first DiscardStmt whose value is a
// CallExpr. We need this specific filter because some tests declare
// helper methods that themselves contain DiscardStmt initializers; a
// purely positional "first" wouldn't pick the call we want to type.
const bezel::ast::Expr* firstCallDiscardValue(
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
                    if (dynamic_cast<const bezel::ast::CallExpr*>(
                            ds->value.get())) {
                        return ds->value.get();
                    }
                }
            }
        }
    }
    return nullptr;
}

} // namespace

BEZEL_TEST(sema_call, method_call_returns_int) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method Helper(): int => 1; "
        "  public method M() _ := this.Helper(); end M; "
        "end C;");
    auto* e = firstCallDiscardValue(r);
    BEZEL_EXPECT_TRUE(dynamic_cast<const bezel::ast::CallExpr*>(e) != nullptr);
    if (e) {
        auto* t = r.ctx->exprType(*e);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

BEZEL_TEST(sema_call, method_call_returns_named) {
    auto r = parseSemaAll(
        "module M; "
        "class T end T; "
        "class C "
        // `owned` is REQUIRED here, not decoration (#4759). Per §28.3
        // announcement table row 4 (v0.307), an instance method whose declared
        // return type is a plain concrete class type may return only a
        // receiver-sourced borrow; `new T()` is a fresh owned value, so the
        // unmarked spelling is rejected by the real compiler. This row runs on
        // `parseSemaAll` (sema only — the ownership pass never runs), so the
        // violation would surface only on a port to `runAot` (#3439). The
        // marker does not change what this row measures: the call expression's
        // type is still TypeRef::Kind::Named.
        "  public method Make(): owned T => new T(); "
        "  public method M() _ := this.Make(); end M; "
        "end C;");
    auto* e = firstCallDiscardValue(r);
    BEZEL_EXPECT_TRUE(e != nullptr);
    if (e) {
        auto* t = r.ctx->exprType(*e);
        BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named);
    }
}

BEZEL_TEST(sema_call, method_without_return_type_is_unit) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method Side() _ := 1; end Side; "
        "  public method M() _ := this.Side(); end M; "
        "end C;");
    auto* e = firstCallDiscardValue(r);
    BEZEL_EXPECT_TRUE(e != nullptr);
    if (e) {
        auto* t = r.ctx->exprType(*e);
        BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Unit);
    }
}

BEZEL_TEST(sema_call, call_on_param_resolves) {
    auto r = parseSemaAll(
        "module M; "
        "class T public method Read(): int => 1; end T; "
        "class C "
        "  public method M(t: T) _ := t.Read(); end M; "
        "end C;");
    auto* e = firstCallDiscardValue(r);
    BEZEL_EXPECT_TRUE(e != nullptr);
    if (e) {
        auto* t = r.ctx->exprType(*e);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

BEZEL_TEST(sema_call, call_on_inferred_local_resolves) {
    auto r = parseSemaAll(
        "module M; "
        "class T public method Read(): int => 1; end T; "
        "class C "
        "  public method M() "
        "    var t := new T(); "
        "    _ := t.Read(); "
        "  end M; "
        "end C;");
    // Second statement's discard.
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[1].get());
    const auto* md =
        dynamic_cast<const bezel::ast::MethodDecl*>(td->members[0].get());
    const auto* ds =
        dynamic_cast<const bezel::ast::DiscardStmt*>(md->stmts[1].get());
    auto* e = ds->value.get();
    BEZEL_EXPECT_TRUE(e != nullptr);
    if (e) {
        auto* t = r.ctx->exprType(*e);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}
