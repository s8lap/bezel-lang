// Spec-derived tests for Sema slice 3d (basic) — MemberAccessExpr
// resolution for `this.<member>` and qualified-member field access.
//
// After this pass, every MemberAccessExpr where the target type is
// resolvable should have:
//   * ctx.exprSymbol(ma) → the resolved member Symbol*
//   * ctx.exprType(ma)   → the field/property's resolved type
//   (Methods are recorded as exprSymbol but no exprType — function
//   types need the overload-resolution slice that follows.)
//
// Unresolvable member names emit a diagnostic ("no member `X` on …").
// Member access on an unknown target type silently no-ops (the target
// already emitted its own error).

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

namespace {

// fix #3985: delegates to the shared gated helper (sema_support.hpp)
// instead of duplicating the parse-error-gate logic locally.
bezel::test::SemaResult parseSemaAll(std::string_view src) {
    return bezel::test::parseAndTypecheckGated(src);
}

// Locate the first MemberAccessExpr inside the first method's body.
const bezel::ast::MemberAccessExpr* firstMemberAccess(
    const bezel::test::SemaResult& r) {
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td) return nullptr;
    for (const auto& m : td->members) {
        const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
        if (!md) continue;
        // Walk the first statement looking for MemberAccessExpr.
        if (md->bodyKind == bezel::ast::MethodBodyKind::Expression &&
            md->exprBody) {
            if (auto* ma =
                    dynamic_cast<const bezel::ast::MemberAccessExpr*>(
                        md->exprBody.get())) return ma;
        }
        if (md->bodyKind == bezel::ast::MethodBodyKind::Block) {
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

BEZEL_TEST(sema_member, this_field_resolves_to_field_symbol) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _x: int; "
        "  public method M() _ := this._x; end M; "
        "end C;");
    auto* ma = firstMemberAccess(r);
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<FieldSymbol*>(sym) != nullptr);
        auto* t = r.ctx->exprType(*ma);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

// Static member access `Type.member` across classes: the target is a
// type name (resolving to a TypeSymbol), so the member is looked up on
// that type. Before this, the type-name target produced no targetType
// and the access silently went unresolved.
BEZEL_TEST(sema_member, cross_class_static_field_resolves) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M() _ := Other._total; end M; "
        "end C; "
        "class Other public static _total: int := 5; end Other;");
    auto* ma = firstMemberAccess(r);
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<FieldSymbol*>(sym) != nullptr);
        auto* t = r.ctx->exprType(*ma);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

BEZEL_TEST(sema_member, cross_class_static_method_resolves) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M() _ := Other.Val; end M; "
        "end C; "
        "class Other public static method Val(): int => 7; end Other;");
    auto* ma = firstMemberAccess(r);
    // fix #3985: was `if (ma) { EXPECT... }` — see
    // this_property_resolves_to_property_symbol below for the same finding.
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<MethodSymbol*>(sym) != nullptr);
    }
}

// §12.3: `base.M` resolves M against the base class, so an override in
// the current type is bypassed and the base's member is found.
BEZEL_TEST(sema_member, base_member_resolves_to_base_not_override) {
    auto r = parseSemaAll(
        "module M; "
        "class Derived : Base "
        "  public override method V(): int => 2; "
        "  public method CallBase() _ := base.V; end CallBase; "
        "end Derived; "
        "class Base public virtual method V(): int => 1; end Base;");
    // Find the `base.V` access in Derived.CallBase.
    auto* ma = firstMemberAccess(r);
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        auto* ms = dynamic_cast<MethodSymbol*>(sym);
        BEZEL_EXPECT_TRUE(ms != nullptr);
        // The resolved method's parent is Base, not Derived.
        if (ms) BEZEL_EXPECT_TRUE(ms->parent && ms->parent->name == "Base");
    }
}

BEZEL_TEST(sema_member, this_method_resolves_to_method_symbol) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private method Helper() _ := 1; end Helper; "
        "  public method M() _ := this.Helper; end M; "
        "end C;");
    auto* ma = firstMemberAccess(r);
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<MethodSymbol*>(sym) != nullptr);
    }
}

BEZEL_TEST(sema_member, this_property_resolves_to_property_symbol) {
    // fix #3985: `{ get; set; }` is C#'s property-accessor syntax, not
    // Bezel's — Appendix A's `PropertyBody` has no brace form at all (the
    // auto-property alternative is `AutoAccessorVis? (":=" Expression)?
    // ... ";"`, terminated directly by `;`, or the explicit-accessor form is
    // `Accessor* "end" Identifier? ";"`). This never parsed, so the row was
    // reaching sema on parseSemaAll's ungated, error-recovery-truncated
    // AST. VERIFIED against the real `bezelc -c`: a plain auto-property
    // declaration parses with zero diagnostics and is enough — this test
    // only asserts that `this.Name` resolves to a PropertySymbol.
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public property Name: int; "
        "  public method M() _ := this.Name; end M; "
        "end C;");
    auto* ma = firstMemberAccess(r);
    // fix #3985: was `if (ma) { EXPECT... }` — issue #3985's vacuous-
    // assertion finding: a null lookup (e.g. from the grammar-invalid
    // fixture the `{ get; set; }` bug above produced) made this test
    // assert nothing and still report `[ OK ]`. Assert non-null
    // unconditionally, as most of this file's sibling tests already do.
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<PropertySymbol*>(sym) != nullptr);
        auto* t = r.ctx->exprType(*ma);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

BEZEL_TEST(sema_member, unresolved_member_on_this_unset_and_diagnosed) {
    // issue #414: an unknown member on a fully-resolved user type (here the
    // current class `C`) is now a compile error — it leaves no resolved
    // exprSymbol AND emits a diagnostic, so codegen never lowers a null
    // callee. (The detailed message/scope is asserted in
    // unknown_member_test.cpp; this guards the resolution-side state.)
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M() _ := this.bogus; end M; "
        "end C;");
    auto* ma = firstMemberAccess(r);
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        BEZEL_EXPECT_TRUE(r.ctx->exprSymbol(*ma) == nullptr);
    }
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_member, qualified_field_access_resolves) {
    // fix #3985: `firstMemberAccess` (above) only walks `topDecls[0]`'s
    // members — it is a "first method of the FIRST top-level declaration"
    // helper, not a whole-file search, exactly as every other passing test
    // in this file relies on (the exercised class is always declared
    // first). This fixture declared the unrelated dependency `D` first and
    // `C` — the class whose method `M` is actually under test — second, so
    // `firstMemberAccess` always walked `D`'s members (a field, no
    // methods) and returned null. The `if (ma) { EXPECT... }` anti-pattern
    // masked this: the test has been passing vacuously (asserting nothing)
    // since it was written, independent of and in addition to issue
    // #3985's parseSemaAll-gating finding. VERIFIED against the real
    // gtest binary: with the unconditional `ma != nullptr` assertion
    // added but the declaration order left as-is, this test FAILS
    // (`assertion failed: ma != nullptr`); reordering so `C` is declared
    // first — matching every sibling test in this file — fixes it.
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _d: D; "
        "  public method M() _ := this._d._y; end M; "
        "end C; "
        "class D "
        "  public _y: int; "
        "end D;");
    auto* ma = firstMemberAccess(r);
    // ma is the outer `._y`; check its symbol + type.
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<FieldSymbol*>(sym) != nullptr);
        auto* t = r.ctx->exprType(*ma);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

BEZEL_TEST(sema_member, member_on_local_resolves) {
    auto r = parseSemaAll(
        "module M; "
        "class D public _y: int; end D; "
        "class C "
        "  public method M() "
        "    var d: D := new D(); "
        "    _ := d._y; "
        "  end M; "
        "end C;");
    // Find the second statement's MemberAccessExpr.
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[1].get());
    const auto* md =
        dynamic_cast<const bezel::ast::MethodDecl*>(td->members[0].get());
    const auto* ds =
        dynamic_cast<const bezel::ast::DiscardStmt*>(md->stmts[1].get());
    const auto* ma =
        dynamic_cast<const bezel::ast::MemberAccessExpr*>(ds->value.get());
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<FieldSymbol*>(sym) != nullptr);
    }
}

// ---------------------------------------------------------------------------
// issue #421/#420 (stdlib §4.3/§4.4): a PROPERTY read of `Count`/`Capacity` on
// a built-in heap collection is typed `int`. These collections carry no user
// TypeSymbol, so before the fix the property read had no attached Primitive
// type and codegen had to infer `int` from the lowered LLVM value (#420's
// workaround). The access is an expression-bodied method's body, so
// firstMemberAccess returns the `a.Count` / `a.Capacity` MemberAccessExpr.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_member, array_count_property_typed_int_4_3) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method GetCount(a: Array<int>): int => a.Count; "
        "end C;");
    auto* ma = firstMemberAccess(r);
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* t = r.ctx->exprType(*ma);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

BEZEL_TEST(sema_member, array_capacity_property_typed_int_4_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method GetCap(a: Array<int>): int => a.Capacity; "
        "end C;");
    auto* ma = firstMemberAccess(r);
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* t = r.ctx->exprType(*ma);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

// stdlib §4.10/§4.11: Count is typed int on the other built-in collections too
// (Stack/Queue/Set/LinkedList and Dictionary). Capacity is NOT a member of
// these (only Array<T>/OwningArray<T> declare it), so this guards only Count.
BEZEL_TEST(sema_member, stack_count_property_typed_int_4_10) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method GetCount(s: Stack<int>): int => s.Count; "
        "end C;");
    auto* ma = firstMemberAccess(r);
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* t = r.ctx->exprType(*ma);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

BEZEL_TEST(sema_member, dictionary_count_property_typed_int_4_8) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method GetCount(d: Dictionary<int, int>): int => d.Count; "
        "end C;");
    auto* ma = firstMemberAccess(r);
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* t = r.ctx->exprType(*ma);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}
