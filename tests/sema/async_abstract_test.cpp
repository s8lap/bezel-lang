// Spec-derived tests for Sema slice S35 — `async abstract` is a
// compile error (§12.6 / Appendix C.6).

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

BEZEL_TEST(sema_async, async_abstract_diagnoses) {
    auto r = parseSemaAll(
        "module M; abstract class C "
        "  public async abstract method F(): Task; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_async, abstract_task_return_without_async_ok) {
    auto r = parseSemaAll(
        "module M; abstract class C "
        "  public abstract method F(): Task; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_async, async_concrete_method_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public async method F(): Task return; end F; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
