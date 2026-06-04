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

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
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
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<MethodSymbol*>(sym) != nullptr);
    }
}

BEZEL_TEST(sema_member, this_property_resolves_to_property_symbol) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public property Name: int { get; set; } "
        "  public method M() _ := this.Name; end M; "
        "end C;");
    auto* ma = firstMemberAccess(r);
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<PropertySymbol*>(sym) != nullptr);
        auto* t = r.ctx->exprType(*ma);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

BEZEL_TEST(sema_member, unresolved_member_on_this_silently_unset) {
    // Slice 3d does not diagnose unresolved members (the lookup is
    // intentionally conservative — base-type/inherited and extension-
    // member dispatch arrive in later slices). The MemberAccessExpr
    // simply lacks an exprSymbol; later passes can still proceed.
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
}

BEZEL_TEST(sema_member, qualified_field_access_resolves) {
    auto r = parseSemaAll(
        "module M; "
        "class D "
        "  public _y: int; "
        "end D; "
        "class C "
        "  private _d: D; "
        "  public method M() _ := this._d._y; end M; "
        "end C;");
    auto* ma = firstMemberAccess(r);
    // ma is the outer `._y`; check its symbol + type.
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
// these (only Array<T>/OwnedArray<T> declare it), so this guards only Count.
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
