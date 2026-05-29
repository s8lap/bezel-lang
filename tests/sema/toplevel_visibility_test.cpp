// Spec-derived tests for Sema slice S22 — top-level type visibility
// (§13.2). A top-level type may only be `public` or `internal`
// (internal is also the default); `protected` and `private` are errors.

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

BEZEL_TEST(sema_toplevel_vis, public_ok) {
    auto r = parseSemaAll("module M; public class C end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_toplevel_vis, internal_ok) {
    auto r = parseSemaAll("module M; internal class C end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_toplevel_vis, default_ok) {
    auto r = parseSemaAll("module M; class C end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_toplevel_vis, private_diagnoses) {
    auto r = parseSemaAll("module M; private class C end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_toplevel_vis, protected_diagnoses) {
    auto r = parseSemaAll("module M; protected class C end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_toplevel_vis, nested_private_type_ok) {
    // A nested type may be private (§11.6); the top-level rule applies
    // only to top-level types.
    auto r = parseSemaAll(
        "module M; "
        "public class Outer "
        "  private class Inner end Inner; "
        "end Outer;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
