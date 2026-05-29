// Spec-derived tests for Sema slice S10 — single-inheritance check
// for classes (§11.1). A class may name at most one class-kind base;
// the remaining base entries must be interfaces.

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

BEZEL_TEST(sema_multi_base, single_class_base_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Base end Base; "
        "class Derived : Base end Derived;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_multi_base, two_class_bases_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class A end A; "
        "class B end B; "
        "class C : A, B end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_multi_base, class_plus_interface_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Base end Base; "
        "interface I method M(); end I; "
        "class C : Base, I "
        "  public method M() _ := 1; end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_multi_base, multiple_interfaces_ok) {
    auto r = parseSemaAll(
        "module M; "
        "interface I1 method M1(); end I1; "
        "interface I2 method M2(); end I2; "
        "class C : I1, I2 "
        "  public method M1() _ := 1; end M1; "
        "  public method M2() _ := 2; end M2; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
