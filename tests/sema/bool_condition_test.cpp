// Spec-derived tests for Sema slice S18 — control-flow conditions must
// be `bool` (§21.2 if, §21.3 while, §20 if-expression).
//
// The check only fires when the condition has a resolved primitive type
// that is not bool; un-typed conditions are not flagged (no false
// positives while expression typing is still incomplete).

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
    return std::string("module M; class C public method M(flag: bool, n: int) ")
        + body + " end M; end C;";
}

} // namespace

BEZEL_TEST(sema_cond, if_bool_param_ok) {
    auto r = parseSemaAll(wrap("if flag then n := 1; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_cond, if_int_condition_diagnoses) {
    auto r = parseSemaAll(wrap("if n then n := 1; end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_cond, if_comparison_ok) {
    auto r = parseSemaAll(wrap("if n > 0 then n := 1; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_cond, while_int_condition_diagnoses) {
    auto r = parseSemaAll(wrap("while n do n := n - 1; end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_cond, while_bool_ok) {
    auto r = parseSemaAll(wrap("while flag do n := n - 1; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_cond, elif_int_condition_diagnoses) {
    auto r = parseSemaAll(wrap(
        "if flag then n := 1; elif n then n := 2; end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_cond, if_expression_int_condition_diagnoses) {
    auto r = parseSemaAll(wrap("n := 1 if n else 2;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_cond, if_expression_bool_ok) {
    auto r = parseSemaAll(wrap("n := 1 if flag else 2;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_cond, string_condition_diagnoses) {
    auto r = parseSemaAll(wrap("if \"x\" then n := 1; end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
