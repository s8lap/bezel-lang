// Spec-derived tests for Sema slice T3 — primitive assignability of a
// returned value against the callable's return type (§5.1).

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

std::string m(const std::string& sig, const std::string& body) {
    return std::string("module M; class C public method ") + sig + " " +
        body + " end M; end C;";
}

} // namespace

BEZEL_TEST(sema_ret, return_int_from_int_ok) {
    auto r = parseSemaAll(m("M(): int", "return 5;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ret, return_int_from_double_widens_ok) {
    auto r = parseSemaAll(m("M(): double", "return 5;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ret, return_bool_from_int_diagnoses) {
    auto r = parseSemaAll(m("M(): int", "return true;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ret, return_double_from_int_narrowing_diagnoses) {
    auto r = parseSemaAll(m("M(): int", "return 3.14;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ret, return_string_from_int_diagnoses) {
    auto r = parseSemaAll(m("M(): int", "return \"x\";"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ret, return_param_int_from_int_ok) {
    auto r = parseSemaAll(
        "module M; class C public method M(p: int): int return p; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ret, return_bool_from_bool_ok) {
    auto r = parseSemaAll(m("M(): bool", "return true;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
