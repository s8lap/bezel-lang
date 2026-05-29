// Spec-derived tests for Sema slice S21 — return-statement form rules
// (§21.7). A callable with a return type must `return <value>;`; a
// callable with no return type uses bare `return;`; constructors and
// destructors may not return a value. (Every-path coverage is a
// separate, flow-based check, not exercised here.)

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

BEZEL_TEST(sema_return, value_method_returns_value_ok) {
    auto r = parseSemaAll(
        "module M; class C public method F(): int return 1; end F; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_return, value_method_bare_return_diagnoses) {
    auto r = parseSemaAll(
        "module M; class C public method F(): int return; end F; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_return, void_method_bare_return_ok) {
    auto r = parseSemaAll(
        "module M; class C public method F() return; end F; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_return, void_method_value_return_diagnoses) {
    auto r = parseSemaAll(
        "module M; class C public method F() return 1; end F; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_return, constructor_value_return_diagnoses) {
    auto r = parseSemaAll(
        "module M; class C public constructor() return 1; end constructor; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_return, constructor_bare_return_ok) {
    auto r = parseSemaAll(
        "module M; class C public constructor() return; end constructor; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_return, destructor_value_return_diagnoses) {
    auto r = parseSemaAll(
        "module M; class C destructor return 1; end destructor; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_return, value_return_inside_if_in_void_method_diagnoses) {
    auto r = parseSemaAll(
        "module M; class C public method F(b: bool) "
        "  if b then return 1; end; "
        "end F; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
