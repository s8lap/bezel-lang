// Spec-derived tests for Sema slice S12 — inheritance-cycle detection.
// Circular base-type chains are diagnosed by TypeResolution after all
// base references resolve.

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

BEZEL_TEST(sema_cycle, two_class_cycle_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class A : B end A; "
        "class B : A end B;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_cycle, three_class_cycle_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class A : B end A; "
        "class B : C end B; "
        "class C : A end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_cycle, linear_chain_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class A end A; "
        "class B : A end B; "
        "class C : B end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_cycle, diamond_through_interfaces_ok) {
    auto r = parseSemaAll(
        "module M; "
        "interface I1 method M(); end I1; "
        "interface I2 method M(); end I2; "
        "class C : I1, I2 "
        "  public method M() _ := 1; end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
