// Tests for issue #487: typeof(T) and obj.GetType() must be typed correctly
// in sema and must NOT reach codegen to produce silent garbage or a crash.
//
// Spec basis:
//   §17.10  typeof(TypeName) → Type   (the `Type` sealed class, stdlib §2.13)
//   §8.2    v.GetType() → Type        (runtime type of any value)
//   §8.1    GetType is a universal Object member; every type has it
//
// Before this fix, typeof(T) had NO sema handler so its TypeRef was never
// set — the expression was silently untyped.  GetType() was whitelisted as a
// universal Object member (to suppress a false "no member" diagnostic) but
// its call return type was also never set.  In both cases codegen received
// an untyped expression and produced silent garbage / nullptr dereference.
//
// After this fix:
//   1. typeof(T)     → Named TypeRef for the `Type` builtin class (§17.10)
//   2. obj.GetType() → Named TypeRef for the `Type` builtin class (§8.2)
//   3. Codegen emits a clean "not yet implemented" error for both (the
//      runtime Type-object table is not yet built; deferred to a later
//      milestone alongside the rest of System.Reflection (stdlib §11)).
//      The error message contains "typeof" or "GetType" and "not yet
//      implemented" so tests can match it.
//   4. A normal member call on a class (control case) still compiles and
//      runs correctly, ruling out over-rejection.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

// Run the full sema pipeline (type-resolution + typecheck) on the given source.
bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// Walk the body of the first method named "M" in the first class and return
// the TypeRef of the first `var` local declaration's initializer expression.
// Returns nullptr when the expression has no type set.
const bezel::sema::TypeRef* firstLocalInitType(
        const bezel::test::SemaResult& r) {
    if (!r.sourceFile || !r.ctx) return nullptr;
    for (const auto& topD : r.sourceFile->topDecls) {
        const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(topD.get());
        if (!td) continue;
        for (const auto& mem : td->members) {
            const auto* md =
                dynamic_cast<const bezel::ast::MethodDecl*>(mem.get());
            if (!md || md->name != "M") continue;
            for (const auto& s : md->stmts) {
                if (!s) continue;
                const auto* ld =
                    dynamic_cast<const bezel::ast::LocalDecl*>(s.get());
                if (!ld || !ld->initializer) continue;
                return r.ctx->exprType(*ld->initializer);
            }
        }
    }
    return nullptr;
}

// True iff the TypeRef is the `Type` builtin (a Named TypeRef whose
// declaration has the name "Type", or a Named TypeRef with no declaration
// but whose importedName or astNode identifies "Type").
bool isTypeTypeRef(const bezel::sema::TypeRef* tr) {
    if (!tr) return false;
    if (tr->kind != bezel::sema::TypeRef::Kind::Named) return false;
    if (tr->declaration && tr->declaration->name == "Type") return true;
    // astNode-backed path: the Named TypeRef was built from an ast::Type whose
    // QualifiedName is "Type" (the resolveType path for `var t: Type`).
    if (tr->astNode && tr->astNode->name &&
        tr->astNode->name->parts.size() == 1 &&
        tr->astNode->name->parts[0] == "Type")
        return true;
    return false;
}

} // namespace

// ===========================================================================
// SEMA TYPING — these tests verify that sema assigns a Named `Type` TypeRef
// to the initializer of `var t := typeof(...)`  and  `var t := x.GetType()`.
// Before the fix, the TypeRef was null (untyped).
// ===========================================================================

// §17.10: typeof(int) → Type.  Primitive type argument.
BEZEL_TEST(sema_typeof_gettype_487, typeof_primitive_typed_as_Type_spec_17_10) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M() "
        "    var t := typeof(int); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* tr = firstLocalInitType(r);
    // The initializer's TypeRef must be the `Type` builtin.
    BEZEL_EXPECT_TRUE(isTypeTypeRef(tr));
}

// §17.10: typeof(string) → Type.  Another primitive.
BEZEL_TEST(sema_typeof_gettype_487, typeof_string_typed_as_Type_spec_17_10) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M() "
        "    var t := typeof(string); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* tr = firstLocalInitType(r);
    BEZEL_EXPECT_TRUE(isTypeTypeRef(tr));
}

// §17.10: typeof(BankAccount) → Type.  User-declared class.
BEZEL_TEST(sema_typeof_gettype_487, typeof_user_class_typed_as_Type_spec_17_10) {
    auto r = parseSemaAll(
        "module M; "
        "class BankAccount public _balance: int; end BankAccount; "
        "class C "
        "  public method M() "
        "    var t := typeof(BankAccount); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* tr = firstLocalInitType(r);
    BEZEL_EXPECT_TRUE(isTypeTypeRef(tr));
}

// §8.2: obj.GetType() on a class-typed receiver → Type.
BEZEL_TEST(sema_typeof_gettype_487, gettype_on_class_typed_as_Type_spec_8_2) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget public _id: int; end Widget; "
        "class C "
        "  public method M(w: Widget) "
        "    var t := w.GetType(); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* tr = firstLocalInitType(r);
    BEZEL_EXPECT_TRUE(isTypeTypeRef(tr));
}

// §8.2: this.GetType() inside a class method → Type.
BEZEL_TEST(sema_typeof_gettype_487, gettype_on_this_typed_as_Type_spec_8_2) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M() "
        "    var t := this.GetType(); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* tr = firstLocalInitType(r);
    BEZEL_EXPECT_TRUE(isTypeTypeRef(tr));
}

// §8.2: GetType() on a record-typed receiver → Type.
BEZEL_TEST(sema_typeof_gettype_487, gettype_on_record_typed_as_Type_spec_8_2) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(x: int, y: int); "
        "class C "
        "  public method M(p: Point) "
        "    var t := p.GetType(); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* tr = firstLocalInitType(r);
    BEZEL_EXPECT_TRUE(isTypeTypeRef(tr));
}

// ===========================================================================
// NO SILENT GARBAGE — typeof(T) and GetType() must never be silently untyped.
// Before the fix, `r.ctx->exprType(*init)` returned null, meaning codegen
// would see an untyped expr and emit garbage (or nullptr-deref / crash).
// These tests confirm the TypeRef is non-null after the fix.
// ===========================================================================

// typeof(bool) TypeRef must be non-null (no silent garbage/crash path).
BEZEL_TEST(sema_typeof_gettype_487, typeof_not_silently_untyped_spec_17_10) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M() "
        "    var t := typeof(bool); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* tr = firstLocalInitType(r);
    // Key invariant: the TypeRef is NOT null — before the fix it was null,
    // causing codegen to emit a garbage value or crash.
    BEZEL_EXPECT_TRUE(tr != nullptr);
}

// GetType() TypeRef must be non-null (no silent garbage/crash path).
BEZEL_TEST(sema_typeof_gettype_487, gettype_not_silently_untyped_spec_8_2) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal public _name: string; end Animal; "
        "class C "
        "  public method M(a: Animal) "
        "    var t := a.GetType(); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* tr = firstLocalInitType(r);
    BEZEL_EXPECT_TRUE(tr != nullptr);
}

// ===========================================================================
// CONTROL — normal member calls on user types still work.  These tests
// verify that the fix does not break non-typeof/GetType member access.
// ===========================================================================

// A normal method call on a class still resolves cleanly.
BEZEL_TEST(sema_typeof_gettype_487, normal_member_call_on_class_still_works) {
    auto r = parseSemaAll(
        "module M; "
        "class Calc "
        "  public method Add(a: int, b: int): int => a + b; "
        "end Calc; "
        "class C "
        "  public method M(c: Calc): int "
        "    return c.Add(1, 2); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The universal `ToString` on a record still resolves cleanly (no regression
// from the GetType typing addition to the universal-member dispatch).
BEZEL_TEST(sema_typeof_gettype_487, toString_on_record_still_works) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(x: int, y: int); "
        "class C "
        "  public method M(p: Point): string "
        "    return p.ToString(); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
