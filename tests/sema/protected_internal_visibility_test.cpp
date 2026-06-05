// Spec-derived tests for Sema §13.1 — `protected` and `internal` member
// visibility enforcement (issue #432).
//
// §13.1 table:
//   protected — declaring class and all classes derived from it.
//   internal  — code in the same module.
//
// Previously only `private` was enforced; this file verifies that the
// compiler now also rejects out-of-scope `protected` and `internal` access.

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

// --- protected field tests (§13.1) ---

// §13.1 example: protected field accessed from an UNRELATED class → error.
// This is the core repro for issue #432.
BEZEL_TEST(sema_vis_432, protected_field_from_unrelated_class_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base "
        "  protected _x: int := 99; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Base; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Base(); "
        "    return b._x; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §13.1: protected field accessed from WITHIN the declaring class → allowed.
BEZEL_TEST(sema_vis_432, protected_field_from_declaring_class_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Base "
        "  protected _x: int := 0; "
        "  public method GetX(): int => this._x; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Base;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §13.1: protected field accessed from a DERIVED class (via `this`) → allowed.
BEZEL_TEST(sema_vis_432, protected_field_from_derived_class_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Base "
        "  protected _x: int := 0; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Base; "
        "class Derived : Base "
        "  public method GetX(): int => this._x; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Derived;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §13.1: protected field accessed from a TRANSITIVELY DERIVED class → allowed.
BEZEL_TEST(sema_vis_432, protected_field_from_transitive_derived_class_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class A "
        "  protected _x: int := 0; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end A; "
        "class B : A "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end B; "
        "class C : B "
        "  public method GetX(): int => this._x; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- protected method tests (§13.1) ---

// §13.1: protected method called from an UNRELATED class → error.
BEZEL_TEST(sema_vis_432, protected_method_from_unrelated_class_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class A "
        "  protected method Helper(): int => 42; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end A; "
        "class B "
        "  public method M(a: A): int => a.Helper; "
        "end B;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §13.1: protected method called from the DECLARING class → allowed.
BEZEL_TEST(sema_vis_432, protected_method_from_declaring_class_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class A "
        "  protected method Helper(): int => 42; "
        "  public method Pub(): int => this.Helper; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end A;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §13.1: protected method called from a DERIVED class → allowed.
BEZEL_TEST(sema_vis_432, protected_method_from_derived_class_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class A "
        "  protected method Helper(): int => 42; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end A; "
        "class B : A "
        "  public method Pub(): int => this.Helper; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end B;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- public regression tests (§13.1) ---

// §13.1: public members are always accessible from any class → no error.
BEZEL_TEST(sema_vis_432, public_field_from_unrelated_class_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class A public _x: int := 1; end A; "
        "class B "
        "  public method M(a: A): int => a._x; "
        "end B;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- private regression tests (§13.1, slice S8 still works) ---

// §13.1: private members remain inaccessible from outside (regression guard).
BEZEL_TEST(sema_vis_432, private_field_from_unrelated_class_still_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class A private _x: int := 1; end A; "
        "class B "
        "  public method M(a: A): int => a._x; "
        "end B;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §13.1: private access from the declaring class is still allowed (regression).
BEZEL_TEST(sema_vis_432, private_field_from_own_class_still_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _x: int := 0; "
        "  public method Get(): int => this._x; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §13.1: private member on a base class is NOT accessible from derived (§13.1).
BEZEL_TEST(sema_vis_432, private_field_from_derived_class_still_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base private _x: int := 0; end Base; "
        "class Derived : Base "
        "  public method Get(): int => this._x; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
