// Spec-derived tests for Sema slice S34 — static members may not be
// virtual / abstract / override (§12.1).

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

BEZEL_TEST(sema_static_virtual, static_method_ok) {
    auto r = parseSemaAll(
        "module M; class C public static method F(): int => 1; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static_virtual, static_virtual_method_diagnoses) {
    auto r = parseSemaAll(
        "module M; class C public static virtual method F(): int => 1; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static_virtual, virtual_instance_method_ok) {
    auto r = parseSemaAll(
        "module M; class C public virtual method F(): int => 1; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static_virtual, static_abstract_method_diagnoses) {
    auto r = parseSemaAll(
        "module M; abstract class C "
        "  public static abstract method F(): int; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
