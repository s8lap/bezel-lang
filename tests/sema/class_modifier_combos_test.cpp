// Spec-derived tests for Sema slice S17 — class modifier mutual-
// exclusions (§11.7). `abstract`+`static` and `sealed`+`static` are
// compile errors; `sealed abstract` is permitted.

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

BEZEL_TEST(sema_class_mods, abstract_static_diagnoses) {
    auto r = parseSemaAll(
        "module M; abstract static class C end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_class_mods, sealed_static_diagnoses) {
    auto r = parseSemaAll(
        "module M; sealed static class C end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_class_mods, sealed_abstract_ok) {
    auto r = parseSemaAll(
        "module M; sealed abstract class Shape end Shape;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_class_mods, plain_abstract_ok) {
    auto r = parseSemaAll(
        "module M; abstract class Shape end Shape;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_class_mods, plain_static_ok) {
    auto r = parseSemaAll(
        "module M; static class Utils end Utils;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_class_mods, plain_sealed_ok) {
    auto r = parseSemaAll(
        "module M; sealed class C end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
