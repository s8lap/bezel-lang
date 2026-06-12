// Spec-derived tests for fix #680 — false 'hides an inherited member'
// on derived class primary-constructor parameters whose names match base
// class primary-constructor parameter names.
//
// Authority: §11.1 (primary constructors) / §12.2 (hiding is not allowed).
//
// §11.1 / §22.10: "For each primary-constructor parameter, the compiler
// does NOT generate an auto-property (unlike records). The parameters are
// initialization-time values, optionally captured for body use; the class
// author decides which fields or properties to declare publicly."
//
// §12.2: "Declaring a member in a derived class with the same name as a
// non-virtual base member is a compile error." The key word is MEMBER.
// Primary-constructor parameters are NOT members of the class's public API;
// they are ctor-time values. Applying the hiding check to them is wrong.
//
// Repro (spec §11.1 example): BankAccount(name, initialBalance) ←
//   SavingsAccount(name, initialBalance, rate): BankAccount(name, initialBalance)
// Both `name` and `initialBalance` appear in both classes as primary-ctor
// params, but neither is a declared member that can participate in hiding.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

} // namespace

// ── §11.1 / §22.10 / #680: the spec's own primary-ctor inheritance example ──

// §11.1 spec example (verbatim shape): base and derived class both have
// primary-ctor parameters named `name` and `initialBalance`. The derived
// class's `SavingsAccount(name, initialBalance, rate)` must NOT produce a
// 'hides an inherited member' error on `name` or `initialBalance`.
// Pre-fix: false positive on both params. Post-fix: compiles clean.
BEZEL_TEST(sema_680_primary_ctor_hiding, spec_example_11_1_savings_account_no_hiding) {
    // per §11.1 / §22.10 — exact spec example shape
    auto r = parseSemaAll(
        "module M; "
        "class BankAccount(name: string, initialBalance: int) "
        "  public readonly property Name: string := name; "
        "  private _balance: int := initialBalance; "
        "end BankAccount; "
        "class SavingsAccount(name: string, initialBalance: int, rate: int) "
        "  : BankAccount(name, initialBalance) "
        "  private _rate: int := rate; "
        "end SavingsAccount;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §11.1 / #680: a single shared param name in base and derived must not diagnose.
BEZEL_TEST(sema_680_primary_ctor_hiding, single_shared_param_name_no_hiding) {
    // per §11.1
    auto r = parseSemaAll(
        "module M; "
        "class A(x: int) "
        "  private _x: int := x; "
        "end A; "
        "class B(x: int, y: int) : A(x) "
        "  private _y: int := y; "
        "end B;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §11.1 / #680: all params shared in both base and derived — no hiding error.
BEZEL_TEST(sema_680_primary_ctor_hiding, all_params_shared_no_hiding) {
    // per §11.1
    auto r = parseSemaAll(
        "module M; "
        "class Base(a: int, b: int) "
        "  private _a: int := a; "
        "  private _b: int := b; "
        "end Base; "
        "class Derived(a: int, b: int) : Base(a, b) "
        "  private _extra: int := a; "
        "end Derived;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── §12.2 NEGATIVE regressions: real member hiding still diagnosed ──────────

// §12.2 / #680 regression: a genuine field declared in derived with the same
// name as a base field must STILL be rejected (hiding a real member).
BEZEL_TEST(sema_680_primary_ctor_hiding, real_field_hiding_still_diagnosed) {
    // per §12.2 — regression guard
    auto r = parseSemaAll(
        "module M; "
        "class Base public _x: int; end Base; "
        "class Derived : Base "
        "  public _x: int; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §12.2 / #680 regression: a method hiding a base method must STILL diagnose.
BEZEL_TEST(sema_680_primary_ctor_hiding, method_hiding_base_method_still_diagnosed) {
    // per §12.2 — regression guard
    auto r = parseSemaAll(
        "module M; "
        "class Base public method Speak() _ := 1; end Speak; end Base; "
        "class Derived : Base "
        "  public method Speak() _ := 2; end Speak; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §12.2 / §11.1 / #680: a record's positional field STILL participates in
// hiding — record fields ARE public members, unlike class primary-ctor params.
// (Record-record hiding is disallowed; class-derived-from-record with same
// field name is also disallowed.)
// Here we test that a BODY-DECLARED field in a derived class whose name
// matches a base class's explicit body-declared field still diagnoses.
BEZEL_TEST(sema_680_primary_ctor_hiding, body_field_not_affected_by_exemption) {
    // per §12.2 / §11.1 — regression guard: body field (no posParam) hides
    auto r = parseSemaAll(
        "module M; "
        "class P(x: int) "
        "  public _x: int := x; "  // body field — not a primary-ctor param
        "end P; "
        "class Q : P(0) "
        "  public _x: int; "       // hides base body field P::_x — must diagnose
        "end Q;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
