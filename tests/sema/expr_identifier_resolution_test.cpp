// Spec-derived tests for Sema slice 3b — identifier resolution inside
// method bodies. Each IdentifierExpr inside a method/ctor body gets the
// Symbol it refers to attached via SemaContext::exprSymbol. Locals are
// created on demand at LocalDecl sites; parameters are pre-populated
// from the symbol table.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::sema::LocalSymbol;
using bezel::sema::ParameterSymbol;
using bezel::sema::Symbol;

namespace {

bezel::test::SemaResult parseSema(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// Get the i-th stmt of the first method body as a typed pointer.
template <typename T>
const T* nthMethodStmt(const bezel::test::SemaResult& r, std::size_t i) {
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td || td->members.empty()) return nullptr;
    const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(
        td->members[0].get());
    if (!md || i >= md->stmts.size()) return nullptr;
    return dynamic_cast<const T*>(md->stmts[i].get());
}

} // namespace

BEZEL_TEST(sema_ident, local_var_resolves) {
    auto r = parseSema(
        "module M; class C public method M() "
        "  var x: int := 1; "
        "  _ := x; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* ds = nthMethodStmt<bezel::ast::DiscardStmt>(r, 1);
    BEZEL_EXPECT_TRUE(ds != nullptr);
    if (ds && ds->value) {
        Symbol* sym = r.ctx->exprSymbol(*ds->value);
        BEZEL_EXPECT_TRUE(dynamic_cast<LocalSymbol*>(sym) != nullptr);
    }
}

BEZEL_TEST(sema_ident, parameter_resolves) {
    auto r = parseSema(
        "module M; class C public method M(p: int) "
        "  _ := p; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* ds = nthMethodStmt<bezel::ast::DiscardStmt>(r, 0);
    if (ds && ds->value) {
        Symbol* sym = r.ctx->exprSymbol(*ds->value);
        BEZEL_EXPECT_TRUE(dynamic_cast<ParameterSymbol*>(sym) != nullptr);
    }
}

BEZEL_TEST(sema_ident, local_shadowing_takes_precedence) {
    auto r = parseSema(
        "module M; class C public method M(x: int) "
        "  var x: int := 99; "
        "  _ := x; "
        "end M; end C;");
    // The local x shadows the parameter; the discard's RHS should
    // resolve to the local.
    const auto* ds = nthMethodStmt<bezel::ast::DiscardStmt>(r, 1);
    if (ds && ds->value) {
        Symbol* sym = r.ctx->exprSymbol(*ds->value);
        BEZEL_EXPECT_TRUE(dynamic_cast<LocalSymbol*>(sym) != nullptr);
    }
}

// §12: an unqualified reference to an *inherited* member inside a
// derived method resolves as an implicit-`this` reference — lookupName
// must search base types, not just the enclosing type's own members.
BEZEL_TEST(sema_ident, inherited_field_resolves_unqualified) {
    using bezel::sema::FieldSymbol;
    auto r = parseSema(
        "module M; "
        "class Derived : Base public method M() _ := _x; end M; end Derived; "
        "class Base public _x: int := 1; end Base;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* ds = nthMethodStmt<bezel::ast::DiscardStmt>(r, 0);
    BEZEL_EXPECT_TRUE(ds != nullptr);
    if (ds && ds->value) {
        Symbol* sym = r.ctx->exprSymbol(*ds->value);
        BEZEL_EXPECT_TRUE(dynamic_cast<FieldSymbol*>(sym) != nullptr);
    }
}

BEZEL_TEST(sema_ident, inherited_method_resolves_unqualified) {
    using bezel::sema::MethodSymbol;
    auto r = parseSema(
        "module M; "
        "class Derived : Base public method M() _ := Helper(); end M; "
        "end Derived; "
        "class Base public method Helper(): int => 1; end Base;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* ds = nthMethodStmt<bezel::ast::DiscardStmt>(r, 0);
    if (ds && ds->value) {
        const auto* call =
            dynamic_cast<const bezel::ast::CallExpr*>(ds->value.get());
        BEZEL_EXPECT_TRUE(call != nullptr);
        if (call) {
            Symbol* sym = r.ctx->exprSymbol(*call->callee);
            BEZEL_EXPECT_TRUE(dynamic_cast<MethodSymbol*>(sym) != nullptr);
        }
    }
}

BEZEL_TEST(sema_ident, unresolved_identifier_in_body_diagnosed) {
    auto r = parseSema(
        "module M; class C public method M() "
        "  _ := undeclared_name; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ident, this_and_base_not_diagnosed) {
    // `this` / `base` are special identifiers — slice 3b doesn't
    // resolve them but mustn't diagnose them either.
    auto r = parseSema(
        "module M; class C public method M() "
        "  _ := this; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ident, local_in_initializer_is_outer_not_self) {
    // `var x := someUndeclared;` — the RHS evaluates BEFORE x is in
    // scope, so the RHS doesn't see x. If `someUndeclared` is
    // legitimately undeclared, it's a diagnosed error. Just verify
    // the parser/sema cycle handles this rather than testing the
    // specific spec-corner (x doesn't self-reference).
    auto r = parseSema(
        "module M; class C public method M() "
        "  var x: int := 1; "
        "  var x: int := x; "
        "end M; end C;");
    // The second `x := x` — RHS sees outer x first (which is the prior
    // declaration in same scope; slice 3b just overwrites the name
    // mapping at the LocalDecl, so this resolves to the first x).
    // We just assert it parses + walks without crash.
    (void)r;
}

BEZEL_TEST(sema_ident, member_access_target_resolves) {
    // `x.member`: the target `x` is an IdentifierExpr that must resolve to the
    // local, and a member access on it must not emit a spurious diagnostic.
    // This test dates from slice 3b, when member NAMES were never resolved —
    // it originally used a bogus `x.Foo`. Since #2126 a nonexistent member on a
    // primitive value IS diagnosed (§8.1/§26.5), so the bogus form would now
    // (correctly) error; use a REAL member (`ToString`, §8.5) to keep this test
    // focused on TARGET resolution rather than the member-lookup behavior the
    // dedicated #2126 tests cover.
    auto r = parseSema(
        "module M; class C public method M() "
        "  var x: int := 1; "
        "  _ := x.ToString(); "
        "end M; end C;");
    // x in `x.ToString()` resolves to the local; no diagnostic.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ident, type_name_resolves_in_method_body) {
    // `C` in the method body resolves to the enclosing class symbol
    // (visible via module-level scope). Uses `default(C?)` — `default(C)` for a
    // non-nullable class is a §17.11 compile error (#951); the nullable form
    // still exercises type-name resolution of `C`.
    auto r = parseSema(
        "module M; class C public method M() "
        "  _ := default(C?); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ident, scope_isolation_between_methods) {
    auto r = parseSema(
        "module M; class C "
        "  public method A() var x: int := 1; end A; "
        "  public method B() _ := x; end B; "
        "end C;");
    // x is local to A; should not be visible in B.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
