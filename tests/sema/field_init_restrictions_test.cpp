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

// §22.8 / §10 fix #656: enum constants are implicitly static compile-time
// values; a static readonly field initializer on an enum may freely reference
// the enum's declared constants. Spec §10 / §22.13: "enum constants are
// compile-time values and are available before any static readonly initializer
// runs." spec_0025 (§10.4 Methods, properties, static members) verifies this.
BEZEL_TEST(sema_field_init, spec_0025_enum_static_readonly_references_own_constants_ok) {
    // Direct transcription of spec §10 / spec_0025: enum Priority's static
    // readonly field `All` initializes with the enum's own constants.
    // Must NOT diagnose §22.8 on Low/Medium/High. (§10 / §22.13)
    auto r = parseSemaAll(
        "module M; "
        "enum Priority "
        "  Low, Medium, High; "
        "  public static readonly All: int := Low; "
        "end Priority;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_field_init, spec_0025_enum_static_readonly_multiple_constants_ok) {
    // Ensure multiple enum constants in the same initializer are all accepted.
    // (§10.4 / §22.13)
    auto r = parseSemaAll(
        "module M; "
        "enum Color "
        "  Red, Green, Blue; "
        "  public static readonly First: int := Red; "
        "  public static readonly Second: int := Green; "
        "end Color;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_field_init, enum_instance_field_still_diagnoses) {
    // Only EnumConstantSymbol is exempted; an actual instance field on an
    // enum (which is itself a §10.5 violation, but we test the §22.8 path
    // separately) still reports an error when referenced in a static initializer.
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public _x: int := 0; "
        "  public static _y: int := _x; "  // _x is an instance member
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
