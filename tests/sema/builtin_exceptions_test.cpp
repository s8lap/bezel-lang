// Spec-derived tests for Sema slice S25 — language-built-in exception
// types (§31.4) resolve without import.

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

std::string field(const std::string& ty) {
    return std::string("module M; class C public _e: ") + ty + "; end C;";
}

} // namespace

BEZEL_TEST(sema_exc, exception_resolves) {
    auto r = parseSemaAll(field("Exception"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_exc, argument_null_exception_resolves) {
    auto r = parseSemaAll(field("ArgumentNullException"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_exc, invalid_operation_exception_resolves) {
    auto r = parseSemaAll(field("InvalidOperationException"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_exc, throw_new_builtin_exception_resolves) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method M() throw new InvalidOperationException(); end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_exc, unknown_exception_still_diagnoses) {
    auto r = parseSemaAll(field("MadeUpException"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
