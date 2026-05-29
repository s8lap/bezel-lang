// Spec-derived tests for Sema slice S8 — `private` member visibility
// (§22.3). A private member is accessible only from inside its
// declaring type's own bodies.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

} // namespace

BEZEL_TEST(sema_priv, private_access_from_own_class_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _x: int := 1; "
        "  public method M() _ := this._x; end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_priv, private_access_from_other_class_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class A private _x: int := 1; end A; "
        "class B "
        "  public method M(a: A) _ := a._x; end M; "
        "end B;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_priv, public_access_from_other_class_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class A public _x: int := 1; end A; "
        "class B "
        "  public method M(a: A) _ := a._x; end M; "
        "end B;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_priv, private_inherited_diagnoses_from_derived) {
    // Per §22.3 spec, `private` is type-local even on inheritance —
    // a derived class cannot reach into the base's privates.
    auto r = parseSemaAll(
        "module M; "
        "class Base private _x: int := 1; end Base; "
        "class Derived : Base "
        "  public method M() _ := this._x; end M; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_priv, private_method_call_from_outside_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class A private method Helper(): int _ := 1; end Helper; end A; "
        "class B "
        "  public method M(a: A) _ := a.Helper; end M; "
        "end B;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
