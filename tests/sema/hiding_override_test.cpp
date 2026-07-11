// Spec-derived tests for Sema slice S15 — hiding forbidden / override
// must target a virtual (§12.1, §12.2; Appendix C.1).
//
// §12.2: declaring a derived-class member with the same name as a base
// CLASS member is a compile error, unless overriding a virtual/abstract
// base member with `override`. Interface members are excluded — a class
// re-declaring an interface member is implementing it, not hiding.

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

BEZEL_TEST(sema_hide, hiding_nonvirtual_method_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public method Speak() _ := 1; end Speak; end Base; "
        "class Derived : Base "
        "  public method Speak() _ := 2; end Speak; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_hide, override_of_virtual_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual method Speak() _ := 1; end Speak; end Base; "
        "class Derived : Base "
        "  public override method Speak() _ := 2; end Speak; "
        "end Derived;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_hide, override_of_nonvirtual_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public method Speak() _ := 1; end Speak; end Base; "
        "class Derived : Base "
        "  public override method Speak() _ := 2; end Speak; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_hide, distinct_names_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public method Speak() _ := 1; end Speak; end Base; "
        "class Derived : Base "
        "  public method Shout() _ := 2; end Shout; "
        "end Derived;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_hide, hiding_base_field_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public _x: int; end Base; "
        "class Derived : Base "
        "  public _x: int; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_hide, interface_member_reimpl_is_not_hiding) {
    // Re-declaring an interface member is implementation, not hiding —
    // no `override` required, no diagnostic.
    auto r = parseSemaAll(
        "module M; "
        "interface IDoer method Do(); end IDoer; "
        "class C : IDoer "
        "  public method Do() _ := 1; end Do; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_hide, override_through_two_levels_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class A public virtual method M() _ := 1; end M; end A; "
        "class B : A public override method M() _ := 2; end M; end B; "
        "class C : B public override method M() _ := 3; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_hide, sealed_override_cannot_be_overridden_again) {
    auto r = parseSemaAll(
        "module M; "
        "class A public virtual method M() _ := 1; end M; end A; "
        "class B : A public sealed override method M() _ := 2; end M; end B; "
        "class C : B public override method M() _ := 3; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §12.2 (v0.181, #1560): the hiding-escape clause reads "a matching
// signature per §12.1" — which now admits the base-`T`/override-`T?`
// nullability widening (§12.1) — so a widened `override` is recognized as a
// legitimate override (escaping the hiding rule), not a same-name hiding
// declaration. Regression coverage for #1090's overload-selection loop: the
// hiding check picks a base candidate to validate the derived member
// against BEFORE it can decide override-vs-hiding, and that selection must
// use the SAME widening-aware predicate as the override-match check itself
// (Resolver::overrideParamTypeMatches, shared via overrideParamsMatch), or
// this widened member would be spuriously matched against the wrong
// candidate and mis-diagnosed.
BEZEL_TEST(sema_hide, widened_override_through_two_levels_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class A public virtual method M(a: string) _ := 1; end M; end A; "
        "class B : A public override method M(a: string?) _ := 2; end M; end B; "
        "class C : B public override method M(a: string?) _ := 3; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §12.2 (v0.184, issue #1694): "The blocked-name set." A `private` base
// member never blocks a same-named derived member — "two same-named
// `private` fields in base and derived are distinct members and legal."
// Prior to the fix, the hiding check collected every same-name base member
// regardless of visibility, so this was wrongly rejected. This is the
// fail-without/pass-with regression pin for #1694.
BEZEL_TEST(sema_hide_1694, private_base_field_does_not_block_hiding) {
    auto r = parseSemaAll(
        "module M; "
        "class Base private _x: int; end Base; "
        "class Derived : Base "
        "  private _x: int; "
        "end Derived;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Same rule, method form — "not limited to instance members"/field members;
// the blocked-name set filter applies uniformly to every member kind.
BEZEL_TEST(sema_hide_1694, private_base_method_does_not_block_hiding) {
    auto r = parseSemaAll(
        "module M; "
        "class Base private method Speak() _ := 1; end Speak; end Base; "
        "class Derived : Base "
        "  private method Speak() _ := 2; end Speak; "
        "end Derived;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §12.2 (v0.184): when the base member is invisible at the derived
// declaration site, "the derived declaration creates a distinct member: no
// override relationship and no shared overload family exist between the
// two" — so a derived member of the SAME name but a DIFFERENT signature
// from an invisible (private) base method is legal too (it does not need to
// resemble an overload of the base method at all).
BEZEL_TEST(sema_hide_1694, private_base_method_distinct_signature_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Base private method Speak() _ := 1; end Speak; end Base; "
        "class Derived : Base "
        "  public method Speak(a: string) _ := 2; end Speak; "
        "end Derived;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Control: `public` base members still always block (regression guard —
// duplicates hiding_base_field_diagnoses above, restated under the #1694
// naming so the whole blocked-name-set axis is grouped together).
BEZEL_TEST(sema_hide_1694, public_base_member_still_blocks_hiding) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public _x: int; end Base; "
        "class Derived : Base "
        "  public _x: int; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Control: `protected` base members always block — "a subclass reached the
// base by inheriting from it," so a protected member is always visible at
// the derived declaration site (§12.2 v0.184).
BEZEL_TEST(sema_hide_1694, protected_base_member_still_blocks_hiding) {
    auto r = parseSemaAll(
        "module M; "
        "class Base protected _x: int; end Base; "
        "class Derived : Base "
        "  protected _x: int; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Control: `internal` base members block when visible at the derived
// declaration site — same module (§13.4/§12.2 v0.184). Cross-module
// `internal` non-blocking is verified via bezelc directly (issue #1694
// verification notes); the sema-only harness here has no second module.
BEZEL_TEST(sema_hide_1694, internal_base_member_same_module_still_blocks_hiding) {
    auto r = parseSemaAll(
        "module M; "
        "class Base internal _x: int; end Base; "
        "class Derived : Base "
        "  internal _x: int; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
