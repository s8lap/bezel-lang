// Spec-derived tests for Sema slice S7 — `readonly` field assignment
// (§11.5). A `readonly` field is assignable only inside the declaring
// class's constructor (or its field initializer). Assignments to
// `this.<readonly>` from a method body are diagnostics.

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

BEZEL_TEST(sema_readonly, assigned_in_ctor_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public readonly _x: int; "
        "  public constructor() this._x := 1; end constructor; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_readonly, assigned_in_method_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public readonly _x: int := 0; "
        "  public method M() this._x := 1; end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_readonly, non_readonly_assigned_in_method_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public _x: int := 0; "
        "  public method M() this._x := 1; end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_readonly, initializer_only_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public readonly _x: int := 42; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_readonly, derived_class_method_cannot_reassign_inherited_readonly) {
    // Even derived classes can't reassign a readonly field from their
    // methods — the constructor exemption is the only loophole.
    auto r = parseSemaAll(
        "module M; "
        "class Base public readonly _x: int := 0; end Base; "
        "class Derived : Base "
        "  public method M() this._x := 1; end M; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
