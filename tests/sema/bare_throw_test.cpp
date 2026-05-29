// Spec-derived tests for Sema slice S27 — bare `throw;` re-raise is
// valid only inside a catch arm body (§21.8 / §31.5).

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

std::string wrap(const std::string& body) {
    return std::string("module M; class C public method M(flag: bool) ")
        + body + " end M; end C;";
}

} // namespace

BEZEL_TEST(sema_throw, bare_throw_in_catch_ok) {
    auto r = parseSemaAll(wrap(
        "try flag := true; catch var e: Exception => throw; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_throw, bare_throw_in_method_body_diagnoses) {
    auto r = parseSemaAll(wrap("throw;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_throw, throw_with_value_in_method_body_ok) {
    auto r = parseSemaAll(wrap("throw new Exception();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_throw, bare_throw_in_try_body_diagnoses) {
    auto r = parseSemaAll(wrap(
        "try throw; catch var e: Exception => flag := false; end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_throw, bare_throw_in_finally_diagnoses) {
    auto r = parseSemaAll(wrap(
        "try flag := true; "
        "catch var e: Exception => flag := false; "
        "finally throw; end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_throw, throw_with_value_in_catch_ok) {
    auto r = parseSemaAll(wrap(
        "try flag := true; "
        "catch var e: Exception => throw new Exception(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
