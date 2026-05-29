// Spec-derived tests for Sema slice S11 — `new` on non-instantiable
// types (abstract class, static class, interface) is an error
// (§11.4 / §11).

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

BEZEL_TEST(sema_inst, new_abstract_class_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "abstract class A end A; "
        "class C public method M() _ := new A(); end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_inst, new_non_abstract_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class A end A; "
        "class C public method M() _ := new A(); end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_inst, new_static_class_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "static class S end S; "
        "class C public method M() _ := new S(); end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_inst, new_interface_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "interface I method M(); end I; "
        "class C public method M() _ := new I(); end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_inst, new_derived_of_abstract_ok) {
    // Deriving from an abstract class is fine; only `new Abstract()`
    // is forbidden.
    auto r = parseSemaAll(
        "module M; "
        "abstract class A end A; "
        "class B : A end B; "
        "class C public method M() _ := new B(); end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
