// Spec-derived tests for Sema slice S6 — diagnose `this` / `base` in
// static methods and static constructors (§12.3). Instance members
// don't exist in a static context; using `this` / `base` from one is
// an error.

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

} // namespace

BEZEL_TEST(sema_static, this_in_static_method_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method M() _ := this; end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static, base_in_static_method_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class B end B; "
        "class C : B "
        "  public static method M() _ := base; end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static, this_in_instance_method_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M() _ := this; end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static, this_in_static_constructor_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  static constructor() _ := this; end constructor; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static, this_in_regular_constructor_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public constructor() _ := this; end constructor; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static, this_in_method_field_access_on_static_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public _x: int; "
        "  public static method M() _ := this._x; end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
