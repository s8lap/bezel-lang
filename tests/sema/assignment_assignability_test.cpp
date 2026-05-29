// Spec-derived tests for Sema slice T2 — primitive assignability on a
// plain `:=` assignment statement (§5.1). Mirrors slice T1's typed-
// initializer check, applied to assignment to a typed local or
// parameter.

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
    return std::string("module M; class C public method M(p: int, b: bool) ")
        + body + " end M; end C;";
}

} // namespace

BEZEL_TEST(sema_assign2, int_to_int_local_ok) {
    auto r = parseSemaAll(wrap("var x: int := 0; x := 5;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_assign2, bool_to_int_local_diagnoses) {
    auto r = parseSemaAll(wrap("var x: int := 0; x := b;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_assign2, int_to_param_ok) {
    auto r = parseSemaAll(wrap("p := 7;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_assign2, bool_to_int_param_diagnoses) {
    auto r = parseSemaAll(wrap("p := b;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_assign2, int_widens_to_double_local_ok) {
    auto r = parseSemaAll(wrap("var d: double := 0.0; d := p;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_assign2, double_narrowing_to_int_diagnoses) {
    auto r = parseSemaAll(wrap("var x: int := 0; x := 3.14;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_assign2, string_to_int_diagnoses) {
    auto r = parseSemaAll(wrap("p := \"no\";"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
