// fix #2838 / §12.3 / §12.5 / §23: accessing an abstract base property via
// `base.P` is a compile error — the property has no implementation, so
// §12.3's "calls the base class's implementation" contract has nothing to
// reach. Before the fix the spelling compiled clean and silently produced 0
// (codegen's `base`-bypass branch in lowerVirtualPropertyGet / -Set found no
// accessor body and zero-init'd).
//
// Four cases mirror #874's named-method gate:
//   1. base.<abstract>()        — must be rejected  (§12.3/§12.5/#2838)
//   2. base.<virtual-concrete>()— must be accepted  (§12.3: valid bypass)
//   3. this.<abstract-origin>() — virtual dispatch → must be accepted
//   4. multi-level concrete base— must be accepted

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

// True iff some diagnostic mentions the §12.3/§12.5 abstract-base-property
// rule.
bool hasAbstractBasePropertyError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("abstract base property") != std::string::npos)
            return true;
    return false;
}

} // namespace

// §12.3/§12.5, #2838: `base.P` where `P` is abstract must be rejected.
BEZEL_TEST(sema_base_abstract_property_2838,
           abstract_base_property_via_base_is_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "abstract class B "
        "  public abstract property P: int; "
        "end B; "
        "class D: B "
        "  public override property P: int => 7; "
        "  public method ViaBaseProp(): int "
        "    return base.P; "
        "  end ViaBaseProp; "
        "end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAbstractBasePropertyError(r));
}

// §12.3: `base.P` on a CONCRETE (virtual) base property is valid and must
// not be rejected — the override-bypass is exactly the §12.3 contract.
BEZEL_TEST(sema_base_abstract_property_2838,
           concrete_virtual_base_property_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class B "
        "  public virtual property P: int => 100; "
        "end B; "
        "class D: B "
        "  public override property P: int => 7; "
        "  public method ViaBaseProp(): int "
        "    return base.P; "
        "  end ViaBaseProp; "
        "end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A virtual dispatch via `this` to a property whose origin is abstract (but
// which the concrete class overrides) must still be accepted. Mirrors
// #874's virtual_this_call_accepted — `this.X` does NOT bypass virtual
// dispatch, so it lands on the override.
BEZEL_TEST(sema_base_abstract_property_2838,
           virtual_this_property_call_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "abstract class B "
        "  public abstract property P: int; "
        "end B; "
        "class D: B "
        "  public override property P: int => 12; "
        "  public method Test(): int "
        "    return this.P; "
        "  end Test; "
        "end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Multi-level hierarchy: B extends A (concrete virtual property) and C
// extends B; reading base.P from C where B.P is concrete must be accepted.
BEZEL_TEST(sema_base_abstract_property_2838,
           multilevel_concrete_base_property_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class A "
        "  public virtual property P: int => 1; "
        "end A; "
        "class B: A "
        "  public override property P: int => 2; "
        "end B; "
        "class C: B "
        "  public override property P: int => 3; "
        "  public method Test(): int "
        "    return base.P; "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
