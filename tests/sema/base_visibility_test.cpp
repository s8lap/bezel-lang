// Spec-derived tests for Sema slice S38 — a class's base class and
// implemented interfaces must be at least as visible as the class
// (§13.7 / Appendix C.2).

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

BEZEL_TEST(sema_base_vis, public_base_public_derived_ok) {
    auto r = parseSemaAll(
        "module M; public class B end B; "
        "public class D : B end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_base_vis, internal_base_public_derived_diagnoses) {
    auto r = parseSemaAll(
        "module M; internal class B end B; "
        "public class D : B end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_base_vis, default_base_public_derived_diagnoses) {
    // A base with no visibility keyword is internal → less visible than
    // a public derived class.
    auto r = parseSemaAll(
        "module M; class B end B; "
        "public class D : B end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_base_vis, internal_base_internal_derived_ok) {
    auto r = parseSemaAll(
        "module M; internal class B end B; "
        "internal class D : B end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_base_vis, internal_interface_public_class_diagnoses) {
    auto r = parseSemaAll(
        "module M; internal interface I method F(); end I; "
        "public class C : I public method F() _ := 1; end F; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_base_vis, builtin_base_ok) {
    // A built-in interface (IDisposable) resolves to a null declaration
    // (effectively public) and is not flagged.
    auto r = parseSemaAll(
        "module M; public class C : IDisposable end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
