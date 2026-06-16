// Spec-derived sema tests for fix #747 (R42 FALSE_ACCEPT) — assignment to a
// `readonly` field through a non-`this` receiver.
//
// Authority: §22.8 — "A field declared `readonly` may be assigned only in a
// field initializer or in a constructor body; assignment anywhere else is a
// compile error." Bug: the check only matched the `this.f` target shape, so an
// instance-qualified (`c.f := x`) or type-qualified (`Type.f := x`) readonly
// write in a normal method slipped through and was dropped at runtime. The fix
// resolves the target's field symbol directly, covering every receiver shape;
// assignment inside a constructor and assignment to a non-readonly field are
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

bool hasReadonlyError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("cannot assign to readonly field") !=
            std::string::npos)
            return true;
    return false;
}

}  // namespace

// §22.8: instance-qualified readonly write in a normal method is rejected.
BEZEL_TEST(readonly_field_assignment_747, instance_receiver_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public readonly _v: int := 10; "
        "  public static method Main(): int "
        "    using var c := new C(); c._v := 50; return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasReadonlyError(r));
}

// §22.8: type-qualified static readonly write in a normal method is rejected.
BEZEL_TEST(readonly_field_assignment_747, static_qualified_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static readonly Max: int := 10; "
        "  public static method Main(): int C.Max := 50; return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasReadonlyError(r));
}

// §22.8: assignment to a readonly field inside the constructor is allowed.
BEZEL_TEST(readonly_field_assignment_747, in_constructor_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public readonly _v: int; "
        "  public constructor() this._v := 7; end constructor; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasReadonlyError(r));
}

// §22.8: a non-readonly field is freely assignable — no error.
BEZEL_TEST(readonly_field_assignment_747, mutable_field_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public _v: int := 0; "
        "  public static method Main(): int "
        "    using var c := new C(); c._v := 9; return c._v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasReadonlyError(r));
}
