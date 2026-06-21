// fix #874 / §12.3 / §12.5: calling an abstract base method via `base.M()` is
// a compile error — the method has no implementation so there is nothing to
// call.  Before the fix such a call was accepted and produced garbage at
// runtime (the vtable slot for the abstract method was unfilled).
//
// Four cases are tested:
//   1. base.<abstract>()          — must be rejected  (§12.3/§12.5, #874)
//   2. base.<virtual-concrete>()  — must be accepted  (§12.3: valid override call)
//   3. this.<abstract-origin>()   — virtual dispatch → must be accepted
//   4. multi-level concrete base  — must be accepted

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

// True iff some diagnostic mentions the §12.3/§12.5 abstract-base-call rule.
bool hasAbstractBaseCallError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("abstract base method") != std::string::npos)
            return true;
    return false;
}

} // namespace

// §12.3/§12.5, #874: `base.Area()` where `Area` is abstract must be rejected.
BEZEL_TEST(sema_base_abstract_call_874, abstract_base_method_call_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "abstract class Shape "
        "  public abstract method Area(): int; "
        "end Shape; "
        "class Rectangle : Shape "
        "  public override method Area(): int "
        "    var ba: int := base.Area(); "
        "    return ba; "
        "  end Area; "
        "end Rectangle;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAbstractBaseCallError(r));
}

// §12.3: calling a CONCRETE (virtual) base method via `base.M()` is valid
// and must not be rejected.
BEZEL_TEST(sema_base_abstract_call_874, concrete_virtual_base_call_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class Base "
        "  public virtual method Compute(): int => 10; "
        "end Base; "
        "class Derived : Base "
        "  public override method Compute(): int "
        "    var b: int := base.Compute(); "
        "    return b; "
        "  end Compute; "
        "end Derived;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A virtual dispatch via `this` to a method whose origin is abstract (but
// which the concrete class overrides) must still be accepted.
BEZEL_TEST(sema_base_abstract_call_874, virtual_this_call_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "abstract class Shape "
        "  public abstract method Area(): int; "
        "end Shape; "
        "class Rectangle : Shape "
        "  public override method Area(): int => 12; "
        "  public method Test(): int "
        "    return this.Area(); "
        "  end Test; "
        "end Rectangle;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Multi-level hierarchy: B extends A (concrete) and C extends B; calling
// base.Compute() from C where B.Compute() is concrete must be accepted.
BEZEL_TEST(sema_base_abstract_call_874, multilevel_concrete_base_call_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class A "
        "  public virtual method Compute(): int => 1; "
        "end A; "
        "class B : A "
        "  public override method Compute(): int => 2; "
        "end B; "
        "class C : B "
        "  public override method Compute(): int "
        "    var b: int := base.Compute(); "
        "    return b; "
        "  end Compute; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
