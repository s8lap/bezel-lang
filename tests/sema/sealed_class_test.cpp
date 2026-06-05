// Spec-derived tests for Sema slice S9 — sealed-class inheritance
// prohibition (§12.4). A class marked `sealed` is not a valid base.

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

BEZEL_TEST(sema_sealed, inherit_from_sealed_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class Base end Base; "
        "class Derived : Base end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_sealed, inherit_from_non_sealed_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Base end Base; "
        "class Derived : Base end Derived;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_sealed, sealed_class_itself_ok) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class C end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_sealed, sealed_class_can_implement_interface) {
    // Interface implementation isn't inheritance; sealed doesn't block it.
    auto r = parseSemaAll(
        "module M; "
        "interface IDoer method Do(): int; end IDoer; "
        "sealed class C : IDoer "
        "  public method Do(): int => 1; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §11.7 / §19.6: the combination `sealed abstract` creates a closed abstract
// class — a class that may be inherited from *within the declaring module*.
// The §11.7 example form (a `sealed abstract` Shape with same-module `sealed`
// subclasses) must compile. This is the spec's recommended closed-hierarchy
// idiom for exhaustive `case` matching; rejecting it made the idiom
// uncompilable (issue: FINDING [6]).
BEZEL_TEST(sema_sealed, sealed_abstract_same_module_subclass_ok_spec_11_7) {
    auto r = parseSemaAll(
        "module M; "
        "sealed abstract class Shape "
        "  public abstract method Area(): int; "
        "end Shape; "
        "sealed class Circle : Shape "
        "  public R: int; "
        "  public constructor(r: int) R := r; end constructor; "
        "  public override method Area(): int => 3 * R * R; "
        "  destructor end destructor; "
        "end Circle;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §11.7: a non-sealed subclass of a `sealed abstract` base, same module, is
// equally allowed — the closed-abstract base restricts only cross-module
// derivation, not the subclass's own modifiers.
BEZEL_TEST(sema_sealed, sealed_abstract_same_module_plain_subclass_ok_spec_11_7) {
    auto r = parseSemaAll(
        "module M; "
        "sealed abstract class Base "
        "  public abstract method V(): int; "
        "end Base; "
        "class Derived : Base "
        "  public override method V(): int => 1; "
        "end Derived;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §12.4 regression guard: a plain `sealed` (non-abstract) base must STILL be
// rejected as a base, even same-module. The fix for §11.7 special-cases only
// `sealed abstract`; plain sealing is unchanged.
BEZEL_TEST(sema_sealed, plain_sealed_base_still_rejected_spec_12_4) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class Base "
        "  public method V(): int => 1; "
        "end Base; "
        "class Derived : Base end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
