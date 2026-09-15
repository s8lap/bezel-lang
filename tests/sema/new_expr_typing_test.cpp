// Spec-derived tests for Sema slice 3f — NewExpr / DefaultExpr typing,
// plus type-resolution of types embedded in expressions (NewExpr::type,
// TypeTestExpr::testType, TypeOfExpr::queriedType, DefaultExpr::type).
//
// Before this slice, `new T()` had no exprType because no pass walked
// the expression's embedded ast::Type to produce a resolved TypeRef.
// Now ExprTypecheck attaches the resolved type to the NewExpr; slice
// 3d's MemberAccessExpr resolver can then look up `(new T()).X`.

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

const bezel::ast::Expr* firstDiscardValue(
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
                    return ds->value.get();
                }
            }
        }
    }
    return nullptr;
}

// The initializer expression of the first field declared in the program.
const bezel::ast::Expr* firstFieldInitializer(
    const bezel::test::SemaResult& r) {
    if (!r.sourceFile) return nullptr;
    for (const auto& topD : r.sourceFile->topDecls) {
        const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(topD.get());
        if (!td) continue;
        for (const auto& m : td->members) {
            const auto* fd =
                dynamic_cast<const bezel::ast::FieldDecl*>(m.get());
            if (fd && fd->initializer) return fd->initializer.get();
        }
    }
    return nullptr;
}

} // namespace

// Issue #163: TypeResolution must resolve Type nodes embedded in a field
// initializer expression (e.g. the `T` in `new T()`), just as it does for
// constructor/method bodies. Before the fix, a class-typed field
// initializer had no resolved type and silently failed to lower in codegen.
BEZEL_TEST(sema_new_expr, field_initializer_new_expr_typed) {
    auto r = parseSemaAll(
        "module M; "
        "class T end T; "
        "class C public _t: T := new T(); end C;");
    auto* e = firstFieldInitializer(r);
    BEZEL_EXPECT_TRUE(dynamic_cast<const bezel::ast::NewExpr*>(e) != nullptr);
    if (e) {
        auto* t = r.ctx->exprType(*e);
        BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named);
    }
}

BEZEL_TEST(sema_new_expr, field_initializer_new_with_args_typed) {
    auto r = parseSemaAll(
        "module M; "
        "class T public constructor(x: int) end constructor; end T; "
        "class C public _t: T := new T(7); end C;");
    auto* e = firstFieldInitializer(r);
    BEZEL_EXPECT_TRUE(e != nullptr);
    if (e) {
        auto* t = r.ctx->exprType(*e);
        BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named);
    }
}

// A `new T()` embedded in an *expression* body (`=> new T()`) must be
// type-resolved just like in a block body — else a factory method's
// result has no type and fails to lower in codegen.
BEZEL_TEST(sema_new_expr, expression_body_new_expr_typed) {
    auto r = parseSemaAll(
        "module M; "
        "class T end T; "
        "class C public static method Make(): T => new T(); end C;");
    // Find the Make method's expression body.
    const bezel::ast::Expr* e = nullptr;
    for (const auto& topD : r.sourceFile->topDecls) {
        auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(topD.get());
        if (!td) continue;
        for (const auto& m : td->members) {
            auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
            if (md && md->exprBody) e = md->exprBody.get();
        }
    }
    BEZEL_EXPECT_TRUE(dynamic_cast<const bezel::ast::NewExpr*>(e) != nullptr);
    if (e) {
        auto* t = r.ctx->exprType(*e);
        BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named);
    }
}

BEZEL_TEST(sema_new_expr, new_named_type) {
    auto r = parseSemaAll(
        "module M; "
        "class T end T; "
        "class C public method M() _ := new T(); end M; end C;");
    auto* e = firstDiscardValue(r);
    BEZEL_EXPECT_TRUE(dynamic_cast<const bezel::ast::NewExpr*>(e) != nullptr);
    if (e) {
        auto* t = r.ctx->exprType(*e);
        BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named);
    }
}

BEZEL_TEST(sema_new_expr, member_access_on_new_expr) {
    // Slice 3d uses NewExpr's type to resolve `(new D()).y`.
    auto r = parseSemaAll(
        "module M; "
        "class D public _y: int; end D; "
        "class C "
        "  public method M() _ := (new D())._y; end M; "
        "end C;");
    auto* e = firstDiscardValue(r);
    auto* ma = dynamic_cast<const bezel::ast::MemberAccessExpr*>(e);
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<FieldSymbol*>(sym) != nullptr);
        auto* t = r.ctx->exprType(*ma);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

BEZEL_TEST(sema_new_expr, default_expr_typed) {
    auto r = parseSemaAll(
        "module M; "
        "class C public method M() _ := default(int); end M; end C;");
    auto* e = firstDiscardValue(r);
    BEZEL_EXPECT_TRUE(e != nullptr);
    if (e) {
        auto* t = r.ctx->exprType(*e);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

BEZEL_TEST(sema_new_expr, new_with_constructor_args) {
    auto r = parseSemaAll(
        "module M; "
        "class T public constructor(x: int) end constructor; end T; "
        "class C public method M() _ := new T(42); end M; end C;");
    auto* e = firstDiscardValue(r);
    BEZEL_EXPECT_TRUE(e != nullptr);
    if (e) {
        auto* t = r.ctx->exprType(*e);
        BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named);
    }
}
