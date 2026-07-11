// Spec-derived sema tests for #1685 — §11.2 (v0.184): the `scoped` parameter
// modifier must match EXACTLY between an interface member (or an overridden
// base member) and its implementation/override, in both directions.
//
// Authority (spec §11.2, v0.184 — "`scoped` in interface signatures"):
// "The same exact-match rule covers the `scoped` modifier, in both
// directions, for the same reason there is no `owned`-direction variance: a
// `scoped`/unscoped mismatch changes the *caller's* contract. An
// implementation adding `scoped` to a parameter the interface declares plain
// would retain a callback — or store a class-typed borrow — that callers
// through the interface admitted under a call-bounded, non-retaining
// contract ([§28.14]), ... a use-after-free shape. An implementation
// *dropping* the interface's `scoped` is the safe direction semantically
// (the callee retains less than announced), but it is rejected all the
// same: `scoped` is part of a function type's identity ([§27.1]) and never
// distinguishes overloads ([§22.6])."
//
// §12.1 states the identical rule for `override`: "Parameter modifiers that
// must match: `ref`, `out`, `owned`, `scoped` (v0.184 ...)."
//
// Before the #1685 fix, `checkAbstractImplementations` (TypeResolution_Checks.cpp)
// verified only a class's IMPLICIT interface conformance by member NAME and
// parameter COUNT — never parameter modifiers — so a `scoped` add-or-drop
// mismatch against an interface member was silently accepted (verified by
// hand against a pre-fix build: `s1_scoped_mismatch.bz`-shaped programs
// compiled clean). The `override` direction (base class, `virtual`/
// `override`) was already covered by `checkOverrideSignatureMatch`'s raw
// `dp->modifier != bp->modifier` comparison and is not the regression this
// file targets; `iface_add_scoped_mismatch_rejected_spec_11_2` and
// `iface_drop_scoped_mismatch_rejected_spec_11_2` below are the tests that
// fail without the fix and pass with it.

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

bool anyDiagContains(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(std::string(needle)) != std::string::npos) return true;
    return false;
}

}  // namespace

// §11.2 (v0.184) — the pinned bug: an interface declares a PLAIN function-
// typed parameter; the implementing method adds `scoped`. Callers through
// the interface admitted a call-bounded (non-retaining) argument, so a
// storing implementation is a use-after-free shape. Must be a compile error
// at the implementing class.
BEZEL_TEST(sema_scoped_iface_match_1685, iface_add_scoped_mismatch_rejected_spec_11_2) {
    auto r = parseSemaAll(
        "module M; "
        "interface IEventOwner method OnEvent(cb: () -> ()); end IEventOwner; "
        "class Impl : IEventOwner "
        "  public method OnEvent(scoped cb: () -> ()) end OnEvent; "
        "end Impl;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "scoped"));
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "§11.2"));
}

// §11.2 (v0.184) — the reverse direction: an interface declares the
// parameter `scoped`; the implementation drops it. Semantically the safer
// direction (retains less), but rejected all the same — `scoped` is part of
// function-type identity and never distinguishes overloads (§22.6).
BEZEL_TEST(sema_scoped_iface_match_1685, iface_drop_scoped_mismatch_rejected_spec_11_2) {
    auto r = parseSemaAll(
        "module M; "
        "interface IEventOwner method OnEvent(scoped cb: () -> ()); end IEventOwner; "
        "class Impl : IEventOwner "
        "  public method OnEvent(cb: () -> ()) end OnEvent; "
        "end Impl;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "scoped"));
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "§11.2"));
}

// CONTROL — a `scoped` interface member matched exactly by the implementation
// compiles clean.
BEZEL_TEST(sema_scoped_iface_match_1685, matching_scoped_signature_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "interface IEventOwner method OnEvent(scoped cb: () -> ()); end IEventOwner; "
        "class Impl : IEventOwner "
        "  public method OnEvent(scoped cb: () -> ()) end OnEvent; "
        "end Impl;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL — a plain (non-`scoped`) interface member matched exactly compiles
// clean (the ordinary, unmarked case must not be false-rejected).
BEZEL_TEST(sema_scoped_iface_match_1685, matching_plain_signature_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "interface IEventOwner method OnEvent(cb: () -> ()); end IEventOwner; "
        "class Impl : IEventOwner "
        "  public method OnEvent(cb: () -> ()) end OnEvent; "
        "end Impl;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §11.2 / §28.14 (v0.180, #1439 scoped construction) — the same exact-match
// rule applies to a `scoped` parameter of CLASS type (a passive borrow, not
// a callback), not just function-typed parameters. Add-mismatch rejected.
BEZEL_TEST(sema_scoped_iface_match_1685, iface_add_scoped_class_typed_param_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Stream end Stream; "
        "interface IHolder method Attach(s: Stream); end IHolder; "
        "class Impl : IHolder "
        "  private _s: Stream? := null; "
        "  public method Attach(scoped s: Stream) _s := s; end Attach; "
        "end Impl;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "scoped"));
}

// §11.2 — the interface obligation may be satisfied by a method INHERITED
// from a base class rather than redeclared on the conforming class itself;
// the `scoped` match must still be enforced against that inherited method,
// not silently skipped because the conforming class has no member of its own
// by that name.
BEZEL_TEST(sema_scoped_iface_match_1685, inherited_impl_scoped_mismatch_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "interface IEventOwner method OnEvent(scoped cb: () -> ()); end IEventOwner; "
        "class Base public method OnEvent(cb: () -> ()) end OnEvent; end Base; "
        "class D : Base, IEventOwner end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "scoped"));
}

// §11.2 — the same inherited-implementation shape, but MATCHING: control to
// confirm inherited (not redeclared) implementations are not false-rejected.
BEZEL_TEST(sema_scoped_iface_match_1685, inherited_impl_scoped_match_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "interface IEventOwner method OnEvent(scoped cb: () -> ()); end IEventOwner; "
        "class Base public method OnEvent(scoped cb: () -> ()) end OnEvent; end Base; "
        "class D : Base, IEventOwner end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §12.1 (v0.184) CONTROL — the `override` direction (base class, not
// interface) was already enforced before #1685 (checkOverrideSignatureMatch's
// exact modifier comparison covers `scoped` as a plain string compare); this
// pins that it stays enforced alongside the new interface-side check.
BEZEL_TEST(sema_scoped_iface_match_1685, override_add_scoped_mismatch_still_rejected_spec_12_1) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual method OnEvent(cb: () -> ()) end OnEvent; end Base; "
        "class D : Base public override method OnEvent(scoped cb: () -> ()) end OnEvent; end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
