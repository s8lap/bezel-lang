// Spec-derived tests for Sema slice S24 — always-available built-in
// type names (§2.3) resolve without import. A reference to e.g.
// `IDisposable`, `IEnumerable<T>`, `Task`, or `Array<T>` must not be
// reported as an unresolved type; generic arity is checked against the
// spec's declared form.

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

std::string field(const std::string& ty) {
    return std::string("module M; class C public _x: ") + ty +
        "; end C;";
}

} // namespace

BEZEL_TEST(sema_builtin, idisposable_resolves) {
    auto r = parseSemaAll(field("IDisposable"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_builtin, ienumerable_t_resolves) {
    auto r = parseSemaAll(field("IEnumerable<int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_builtin, array_t_resolves) {
    auto r = parseSemaAll(field("Array<int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_builtin, task_bare_resolves) {
    auto r = parseSemaAll(field("Task"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_builtin, task_t_resolves) {
    auto r = parseSemaAll(field("Task<int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_builtin, cancellation_token_resolves) {
    auto r = parseSemaAll(field("CancellationToken"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_builtin, ienumerable_wrong_arity_diagnoses) {
    auto r = parseSemaAll(field("IEnumerable<int, int>"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_builtin, idisposable_with_arg_diagnoses) {
    auto r = parseSemaAll(field("IDisposable<int>"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_builtin, genuinely_unknown_type_still_diagnoses) {
    auto r = parseSemaAll(field("NotARealType"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_builtin, class_implements_idisposable_ok) {
    // A class implementing the built-in IDisposable should resolve the
    // base type cleanly (unlike a record — §9.7).
    auto r = parseSemaAll(
        "module M; class C : IDisposable end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
