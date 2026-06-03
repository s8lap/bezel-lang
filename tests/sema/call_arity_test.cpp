// Spec-derived tests for Sema slice T5 — call argument-count check
// (§22.5). Only fires for a sole-overload method called with all-
// positional arguments; optional parameters set the minimum.

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

BEZEL_TEST(sema_arity, exact_count_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int, b: int) end F; "
        "  public method M() this.F(1, 2); end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_arity, too_few_args_diagnoses) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int, b: int) end F; "
        "  public method M() this.F(1); end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_arity, too_many_args_diagnoses) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int, b: int) end F; "
        "  public method M() this.F(1, 2, 3); end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_arity, optional_param_omitted_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int, b: int := 0) end F; "
        "  public method M() this.F(1); end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_arity, optional_param_supplied_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int, b: int := 0) end F; "
        "  public method M() this.F(1, 5); end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_arity, overloaded_method_not_checked) {
    // Two overloads of F → arity check is skipped (overload resolution
    // is a later slice), so the call to F(1) is not flagged even
    // though it doesn't match F(int,int).
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int) end F; "
        "  public method F(a: int, b: int) end F; "
        "  public method M() this.F(1); end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_arity, named_arg_call_missing_required_diagnosed) {
    // §22.5: named-argument binding is now performed (the prior
    // "named-arg call not checked" bail was removed). `F(a := 1)` binds
    // `a` by name but leaves the required parameter `b` unsupplied, which
    // §22.5 requires every call to satisfy — so this is now diagnosed.
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int, b: int) end F; "
        "  public method M() this.F(a := 1); end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_arity, zero_arg_call_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F() end F; "
        "  public method M() this.F(); end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
