// Spec-derived tests for Sema slice S16 — a static class may contain
// only static members (§11.7).

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

BEZEL_TEST(sema_static_class, static_method_in_static_class_ok) {
    auto r = parseSemaAll(
        "module M; "
        "static class Utils "
        "  public static method Max(a: int, b: int): int => a; "
        "end Utils;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static_class, instance_method_in_static_class_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "static class Utils "
        "  public method Inst() _ := 1; end Inst; "
        "end Utils;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static_class, instance_field_in_static_class_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "static class Utils "
        "  public _x: int := 0; "
        "end Utils;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static_class, static_field_in_static_class_ok) {
    auto r = parseSemaAll(
        "module M; "
        "static class Utils "
        "  public static _count: int := 0; "
        "end Utils;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static_class, destructor_in_static_class_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "static class Utils "
        "  public static method M() _ := 1; end M; "
        "  destructor end destructor; "
        "end Utils;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static_class, instance_member_in_normal_class_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Normal "
        "  public _x: int := 0; "
        "  public method Inst() _ := 1; end Inst; "
        "end Normal;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static_class, static_const_in_static_class_ok) {
    auto r = parseSemaAll(
        "module M; "
        "static class Constants "
        "  public const Pi: double := 3.14; "
        "end Constants;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
