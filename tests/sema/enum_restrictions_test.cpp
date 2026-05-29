// Spec-derived tests for Sema slice S32 — enum member restrictions
// (§10.5): no instance fields, constructors, nested types, indexers;
// no virtual/abstract/override methods. Static members allowed (§10.4).

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

BEZEL_TEST(sema_enum, plain_enum_ok) {
    auto r = parseSemaAll(
        "module M; enum Color Red, Green, Blue; end Color;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_enum, enum_static_method_ok) {
    auto r = parseSemaAll(
        "module M; enum E A, B; "
        "  public static method Count(): int => 2; "
        "end E;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_enum, instance_field_diagnoses) {
    auto r = parseSemaAll(
        "module M; enum E A, B; public _x: int := 0; end E;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_enum, static_field_ok) {
    auto r = parseSemaAll(
        "module M; enum E A, B; public static _x: int := 0; end E;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_enum, constructor_diagnoses) {
    auto r = parseSemaAll(
        "module M; enum E A, B; public constructor() end constructor; end E;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_enum, virtual_method_diagnoses) {
    auto r = parseSemaAll(
        "module M; enum E A, B; "
        "  public virtual method M() _ := 1; end M; "
        "end E;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_enum, instance_method_ok) {
    auto r = parseSemaAll(
        "module M; enum E A, B; "
        "  public method Describe(): int => 1; "
        "end E;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
