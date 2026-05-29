// Spec-derived tests for Sema slice S9 — sealed-class inheritance
// prohibition (§11.4). A class marked `sealed` is not a valid base.

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

BEZEL_TEST(sema_sealed, inherit_from_sealed_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class Base end Base; "
        "class Derived : Base end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_sealed, inherit_from_non_sealed_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Base end Base; "
        "class Derived : Base end Derived;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_sealed, sealed_class_itself_ok) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class C end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_sealed, sealed_class_can_implement_interface) {
    // Interface implementation isn't inheritance; sealed doesn't block it.
    auto r = parseSemaAll(
        "module M; "
        "interface IDoer method Do(): int; end IDoer; "
        "sealed class C : IDoer "
        "  public method Do(): int _ := 1; end Do; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
