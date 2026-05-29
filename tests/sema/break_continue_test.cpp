// Spec-derived tests for Sema slice S26 — `break`/`continue` are legal
// only inside a loop (§21.5).

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

BEZEL_TEST(sema_loop, break_in_while_ok) {
    auto r = parseSemaAll(wrap("while flag do break; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_loop, continue_in_for_ok) {
    auto r = parseSemaAll(wrap("for var x in n do continue; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_loop, break_outside_loop_diagnoses) {
    auto r = parseSemaAll(wrap("break;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_loop, continue_outside_loop_diagnoses) {
    auto r = parseSemaAll(wrap("continue;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_loop, break_in_if_in_while_ok) {
    auto r = parseSemaAll(wrap(
        "while flag do if flag then break; end; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_loop, break_after_loop_diagnoses) {
    // The break is lexically after the loop body, not inside it.
    auto r = parseSemaAll(wrap(
        "while flag do n := n - 1; end; break;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_loop, break_in_nested_loop_ok) {
    auto r = parseSemaAll(wrap(
        "while flag do while flag do break; end; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
