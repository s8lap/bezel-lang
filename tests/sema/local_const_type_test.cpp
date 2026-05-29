// Spec-derived tests for Sema slice S31 — a statement-level `const`
// must have a primitive value type or string (§21.1 / §22.12).

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
    return std::string("module M; class T end T; "
        "class C public method M() ") + body + " end M; end C;";
}

} // namespace

BEZEL_TEST(sema_lconst, int_local_const_ok) {
    auto r = parseSemaAll(wrap("const K: int := 5;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_lconst, string_local_const_ok) {
    auto r = parseSemaAll(wrap("const S: string := \"x\";"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_lconst, named_type_local_const_diagnoses) {
    auto r = parseSemaAll(wrap("const X: T := null;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_lconst, inferred_local_const_no_check) {
    auto r = parseSemaAll(wrap("const K := 5;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
