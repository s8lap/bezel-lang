// Spec-derived sema tests for fix #747 (R42 FALSE_ACCEPT) — assignment to a
// `readonly` auto-property.
//
// Authority: §23.1 — "A `readonly` auto-property may be assigned only in a field
// initializer or in a constructor." (Analogous to a readonly field, §22.8.) Bug:
// a readonly auto-property was assignable everywhere — a write in a normal
// method took effect at runtime. The fix rejects an assignment OUTSIDE a
// constructor; constructor assignment and a non-readonly auto-property are
// unaffected.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasReadonlyPropError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("cannot assign to readonly property") !=
            std::string::npos)
            return true;
    return false;
}

}  // namespace

// §23.1: a readonly auto-property write in a normal method is rejected.
BEZEL_TEST(readonly_auto_property_747, assign_outside_ctor_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public readonly property Version: int := 1; "
        "  public static method Main(): int "
        "    using var c := new C(); c.Version := 99; return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasReadonlyPropError(r));
}

// §23.1: assignment inside the constructor is allowed.
BEZEL_TEST(readonly_auto_property_747, assign_in_ctor_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public readonly property Version: int; "
        "  public constructor() this.Version := 5; end constructor; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasReadonlyPropError(r));
}

// §23.1: a non-readonly auto-property is freely assignable — no error.
BEZEL_TEST(readonly_auto_property_747, non_readonly_auto_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public property Version: int; "
        "  public static method Main(): int "
        "    using var c := new C(); c.Version := 8; return c.Version; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasReadonlyPropError(r));
}
