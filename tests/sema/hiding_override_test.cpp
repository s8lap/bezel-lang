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
