// Spec-derived tests for bug #478 — a field/property initializer that
// references a primary-constructor parameter must be accepted, not rejected.
//
// Authority: §22.8 ("Field initializers may reference … primary-constructor
// parameters (§22.10)") and §22.10 (examples: `public readonly property
// Radius: double := radius;`). The checker must distinguish primary-ctor
// parameters (permitted) from genuine instance members (forbidden).
//
// Negative regression: referencing a genuine instance member from a field
// initializer must still be rejected (§22.8 "may not reference … instance
// members"). The fix is surgical — only FieldSymbol with posParam != null
// AND parent kind == Class is permitted.

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

// ── §22.8 / §22.10 positive: primary-ctor param in auto-property initializer ──

// §22.10 spec example shape: `public readonly property X: int := x;` where
// `x` is a primary-constructor parameter. Must compile without error (#478).
BEZEL_TEST(sema_primary_ctor_param_init_478, property_init_from_primary_ctor_param_ok) {
    // per §22.8 / §22.10
    auto r = parseSemaAll(
        "module M; "
        "class Point(x: int, y: int) "
        "  public readonly property X: int := x; "
        "  public readonly property Y: int := y; "
        "end Point;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.8: field (not property) initialized from a primary-ctor param is also OK.
BEZEL_TEST(sema_primary_ctor_param_init_478, field_init_from_primary_ctor_param_ok) {
    // per §22.8 / §22.10
    auto r = parseSemaAll(
        "module M; "
        "class Box(value: int) "
        "  public _stored: int := value; "
        "end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.10 spec example: BankAccount with `name` and `initialBalance`.
BEZEL_TEST(sema_primary_ctor_param_init_478, bank_account_spec_example_22_10_ok) {
    // per §22.10 spec example
    auto r = parseSemaAll(
        "module M; "
        "class BankAccount(name: string, initialBalance: int) "
        "  public readonly property Name: string := name; "
        "  public _balance: int := initialBalance; "
        "end BankAccount;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── §22.8 negative regressions — genuine instance members still forbidden ──

// §22.8: referencing another instance FIELD from a field initializer must
// still produce an error after the fix. The fix is surgical: only
// primary-ctor params (posParam != null, parent kind == Class) are allowed.
BEZEL_TEST(sema_primary_ctor_param_init_478, instance_field_ref_in_init_still_diagnoses) {
    // per §22.8 — regression guard for #478 fix
    auto r = parseSemaAll(
        "module M; class C "
        "  public _a: int := _b; "
        "  public _b: int := 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.8: `this` in an initializer must still be rejected.
BEZEL_TEST(sema_primary_ctor_param_init_478, this_in_init_still_diagnoses) {
    // per §22.8 — regression guard for #478 fix
    auto r = parseSemaAll(
        "module M; class C "
        "  public _x: int := 0; "
        "  public _y: int := this._x; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.8: referencing an instance property (not a primary-ctor param) from
// a property initializer must still be rejected.
BEZEL_TEST(sema_primary_ctor_param_init_478, instance_property_in_init_still_diagnoses) {
    // per §22.8 — regression guard for #478 fix
    auto r = parseSemaAll(
        "module M; class C "
        "  public property A: int := 0; "
        "  public property B: int := A; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
