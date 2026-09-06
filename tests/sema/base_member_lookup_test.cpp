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

// fix #3985: delegates to the shared gated helper (sema_support.hpp)
// instead of duplicating the parse-error-gate logic locally.
bezel::test::SemaResult parseSemaAll(std::string_view src) {
    return bezel::test::parseAndTypecheckGated(src);
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
        "class Base public method Helper(): int => 1; end Base; "
        "class Derived : Base "
        "  public method M() _ := this.Helper; end M; "
        "end Derived;");
    auto* ma = findMemberAccess(r, "M");
    // fix #3985: was `if (ma) { EXPECT... }` (issue #3985's vacuous-
    // assertion finding, same shape as interface_member_resolves below) —
    // assert non-null unconditionally so a null lookup fails the test
    // instead of passing with zero assertions executed.
    BEZEL_EXPECT_TRUE(ma != nullptr);
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
    // fix #3985: was `if (ma) { EXPECT... }` — see interface_member_resolves.
    BEZEL_EXPECT_TRUE(ma != nullptr);
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
    // fix #3985: was `if (ma) { EXPECT... }` — see interface_member_resolves.
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<FieldSymbol*>(sym) != nullptr);
    }
}

BEZEL_TEST(sema_base, interface_member_resolves) {
    // Implements clause (after the colon) — interface members are
    // looked up via the same base-types path.
    //
    // fix #3985: `public method Do(): int;` inside the interface body was a
    // §11.8 rejection (interface members are implicitly public and admit no
    // repeated visibility modifier) — the fixture never parsed, so this row
    // was reaching sema on parseSemaAll's ungated, error-recovery-truncated
    // AST. VERIFIED against the real `bezelc -c`: with `public` dropped from
    // the interface member, the source parses with zero diagnostics.
    auto r = parseSemaAll(
        "module M; "
        "interface IDoer method Do(): int; end IDoer; "
        "class C : IDoer "
        "  public method Do(): int => 1; "
        "  public method M() _ := this.Do; end M; "
        "end C;");
    auto* ma = findMemberAccess(r, "M");
    // fix #3985: was `if (ma) { EXPECT... }` — a null findMemberAccess
    // (e.g. from the grammar-invalid fixture above) made this test assert
    // NOTHING and still report `[ OK ]`, indistinguishable in gtest output
    // from a genuine pass (issue #3892's `if (x) { EXPECT }` discriminator
    // problem). Assert non-null unconditionally, as the file's own
    // `derived_inherits_base_field` already does.
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<MethodSymbol*>(sym) != nullptr);
    }
}
