// Spec-derived tests for Sema slice S37 — a static constructor takes
// no parameters (§22.7 / Appendix C.5).

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

BEZEL_TEST(sema_static_ctor, no_params_ok) {
    auto r = parseSemaAll(
        "module M; class C static constructor() end constructor; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static_ctor, with_params_diagnoses) {
    auto r = parseSemaAll(
        "module M; class C static constructor(a: int) end constructor; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static_ctor, instance_ctor_with_params_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public constructor(a: int) end constructor; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
