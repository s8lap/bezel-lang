// Spec-derived tests for Sema slice S33 — method body presence
// (§12.5 / §11.2). In a class, `abstract` methods have no body and
// non-abstract methods must have one. Interface methods have no body.

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

BEZEL_TEST(sema_body, concrete_method_with_body_ok) {
    auto r = parseSemaAll(
        "module M; class C public method F(): int => 1; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_body, concrete_method_without_body_diagnoses) {
    auto r = parseSemaAll(
        "module M; class C public method F(): int; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_body, abstract_method_without_body_ok) {
    auto r = parseSemaAll(
        "module M; abstract class C "
        "  public abstract method F(): int; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_body, abstract_method_with_body_diagnoses) {
    auto r = parseSemaAll(
        "module M; abstract class C "
        "  public abstract method F(): int => 1; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_body, interface_method_without_body_ok) {
    auto r = parseSemaAll(
        "module M; interface I method F(): int; end I;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_body, interface_method_with_body_diagnoses) {
    auto r = parseSemaAll(
        "module M; interface I method F(): int => 1; end I;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_body, block_body_concrete_ok) {
    auto r = parseSemaAll(
        "module M; class C public method F() return; end F; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
