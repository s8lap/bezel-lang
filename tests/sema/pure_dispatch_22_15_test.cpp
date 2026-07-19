// Spec-derived tests for the `pure` member modifier — Slice 3: dispatch
// matching (override / interface-implementation rules), pure-constructor
// field-initializer checking, and auto-getter pure classification.
//
// Spec §22.15 (v0.202, proposal 0042) "Checking and dispatch" +
// "Pure constructors" + "Members classified by rule" + Appendix C.14
// bullet 4 ("Overrides and implementations of pure members restate `pure`;
// purity never narrows down an override chain") and bullet 5 ("Field
// initializers of a pure-constructed class ... satisfy the pure-body
// rules").
//
// Dispatch rules per §22.15:
//   1. Purity is never dropped — an `override` of a pure member must be
//      declared `pure`, and a class member implementing a pure interface
//      member must be declared `pure`.
//   2. Purity may be strengthened (permissive — no diagnostic).
//   3. Monotone down the chain (rule 1 applied transitively).
//
// Field initializers of a pure-constructed class are checked under the
// pure-body rules (the birth-store rule extends to initializers).

#include "sema/ExprTypecheck.hpp"
#include "sema/PureChecker.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
        bezel::sema::checkPureBodies(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// =========================================================================
// §22.15 dispatch rule 1: purity is never dropped across an override.
// =========================================================================

// §22.15 dispatch rule 1: an override of a pure base method must itself be
// pure.
BEZEL_TEST(pure_dispatch_22_15, override_of_pure_must_be_pure_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class Base "
        "  public pure virtual method Cost(): int => 0; "
        "end Base; "
        "class D : Base "
        "  public override method Cost(): int => 1; "
        "end D;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "must be declared `pure`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "purity is never dropped"));
}

// §22.15 dispatch rule 1 (control): an override that DOES restate pure is
// accepted.
BEZEL_TEST(pure_dispatch_22_15, override_restating_pure_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class Base "
        "  public pure virtual method Cost(): int => 0; "
        "end Base; "
        "class D : Base "
        "  public pure override method Cost(): int => 1; "
        "end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15 dispatch rule 2 (strengthening): an override MAY declare pure
// where the base is not — accepted.
BEZEL_TEST(pure_dispatch_22_15, override_strengthening_to_pure_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class Base "
        "  public virtual method Cost(): int => 0; "
        "end Base; "
        "class D : Base "
        "  public pure override method Cost(): int => 1; "
        "end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =========================================================================
// §22.15 dispatch rule 1: purity is never dropped across an interface
// implementation.
// =========================================================================

// §22.15 dispatch rule 1: a class member implementing a pure interface
// method must be declared pure.
BEZEL_TEST(pure_dispatch_22_15, impl_of_pure_iface_must_be_pure_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "interface I "
        "  pure method Cost(): int; "
        "end I; "
        "class C : I "
        "  public method Cost(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "must be declared `pure`"));
}

// §22.15 dispatch rule 1 (control): implementation restating pure accepted.
BEZEL_TEST(pure_dispatch_22_15, impl_of_pure_iface_restating_pure_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "interface I "
        "  pure method Cost(): int; "
        "end I; "
        "class C : I "
        "  public pure method Cost(): int => 0; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =========================================================================
// §22.15 "Pure constructors" + Appendix C.14 bullet 5: field initializers
// of a pure-constructed class satisfy the pure-body rules.
// =========================================================================

// §22.15: a class with a pure constructor and a pure-satisfying field
// initializer — accepted.
BEZEL_TEST(pure_dispatch_22_15, pure_ctor_pure_field_init_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _v: int := 42; "
        "  public pure constructor() end constructor; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15: a class with a pure constructor and an IMPURE field initializer
// (one that constructs a non-trivial-dispose class) — the initializer is
// rejected.
BEZEL_TEST(pure_dispatch_22_15, pure_ctor_impure_field_init_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  destructor end destructor; "
        "end C; "
        "class D "
        "  private _c: C := new C(); "
        "  public pure constructor() end constructor; "
        "end D;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "class construction"));
}

// =========================================================================
// §22.15 "Members classified by rule": auto-getter is implicitly pure
// (Slice 3 — auto-getter calls from a pure body are admitted).
// =========================================================================

// §22.15: a call to an auto-property GETTER from a pure body — admitted
// (the auto-getter is pure-classified as a backing-field read).
BEZEL_TEST(pure_dispatch_22_15, pure_calls_auto_getter_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public property V: int := 0; "
        "  public pure method Get(): int => V; "
        "end C;");
    // Auto-property getter is implicitly pure-classified — the read is
    // admitted. (Set is never pure.)
    BEZEL_EXPECT_FALSE(hasMsg(r, "is not pure-declared"));
}
