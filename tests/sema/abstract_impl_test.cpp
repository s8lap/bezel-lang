// Spec-derived tests for Sema slice S13 — a concrete class must
// implement every inherited abstract / interface member (§11.4).

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

BEZEL_TEST(sema_abs_impl, unimplemented_abstract_method_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "abstract class Base public abstract method Draw(); end Base; "
        "class C : Base end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_abs_impl, implemented_abstract_method_ok) {
    auto r = parseSemaAll(
        "module M; "
        "abstract class Base public abstract method Draw(); end Base; "
        "class C : Base "
        "  public method Draw() _ := 1; end Draw; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_abs_impl, unimplemented_interface_method_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "interface IDoer method Do(); end IDoer; "
        "class C : IDoer end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_abs_impl, implemented_interface_method_ok) {
    auto r = parseSemaAll(
        "module M; "
        "interface IDoer method Do(); end IDoer; "
        "class C : IDoer "
        "  public method Do() _ := 1; end Do; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_abs_impl, abstract_class_neednt_implement) {
    // An abstract class is allowed to leave abstract members unimpl'd.
    auto r = parseSemaAll(
        "module M; "
        "interface IDoer method Do(); end IDoer; "
        "abstract class Partial : IDoer end Partial;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_abs_impl, implementation_in_intermediate_class_ok) {
    auto r = parseSemaAll(
        "module M; "
        "abstract class A public abstract method Draw(); end A; "
        "class B : A public method Draw() _ := 1; end Draw; end B; "
        "class C : B end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
