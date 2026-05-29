// Spec-derived tests for Sema slice S29 — field initializers may not
// observe instance state: no `this`, `base`, or instance members
// (§22.8).

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

BEZEL_TEST(sema_field_init, literal_initializer_ok) {
    auto r = parseSemaAll(
        "module M; class C public _x: int := 42; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_field_init, this_in_initializer_diagnoses) {
    auto r = parseSemaAll(
        "module M; class C public _x: int := this._x; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_field_init, instance_member_ref_diagnoses) {
    // Bare reference to another instance field (implicit `this`).
    auto r = parseSemaAll(
        "module M; class C "
        "  public _a: int := _b; "
        "  public _b: int := 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_field_init, static_member_ref_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static _seed: int := 10; "
        "  public _a: int := _seed; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_field_init, const_ref_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public const K: int := 5; "
        "  public _a: int := K; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_field_init, instance_method_in_method_body_ok) {
    // The restriction is initializer-only; method bodies may use `this`.
    auto r = parseSemaAll(
        "module M; class C "
        "  public _x: int := 0; "
        "  public method M() _ := this._x; end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_field_init, auto_property_initializer_this_diagnoses) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property P: int := this._x; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
