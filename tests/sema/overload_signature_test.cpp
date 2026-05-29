// Spec-derived tests for Sema slice S19 — overloads must be
// distinguishable (§22.6). Overload resolution considers parameter
// count, types, and `ref`/`out` modifiers. Ordinary `owned` does NOT
// distinguish; `out` vs `out owned` does.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

} // namespace

BEZEL_TEST(sema_overload, identical_signatures_diagnose) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method F(a: int) _ := 1; end F; "
        "  public method F(a: int) _ := 2; end F; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_overload, different_arity_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method F(a: int) _ := 1; end F; "
        "  public method F(a: int, b: int) _ := 2; end F; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_overload, different_param_type_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method F(a: int) _ := 1; end F; "
        "  public method F(a: bool) _ := 2; end F; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_overload, owned_only_difference_diagnoses) {
    // `M(t: T)` vs `M(owned t: T)` — ordinary owned does not
    // distinguish, so this is an error (§22.6).
    auto r = parseSemaAll(
        "module M; "
        "class T end T; "
        "class C "
        "  public method F(t: T) _ := 1; end F; "
        "  public method F(owned t: T) _ := 2; end F; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_overload, ref_distinguishes) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method F(a: int) _ := 1; end F; "
        "  public method F(ref a: int) _ := 2; end F; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_overload, out_vs_out_owned_distinguishes) {
    auto r = parseSemaAll(
        "module M; "
        "class T end T; "
        "class C "
        "  public method F(out c: T) _ := 1; end F; "
        "  public method F(out owned c: T) _ := 2; end F; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_overload, duplicate_constructors_diagnose) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public constructor(a: int) end constructor; "
        "  public constructor(a: int) end constructor; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_overload, different_named_methods_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method F(a: int) _ := 1; end F; "
        "  public method G(a: int) _ := 2; end G; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
