// Spec-derived tests for Sema slice T6 — argument-type assignability
// for a sole-overload, all-positional call (§5.1 / §12). Reuses the
// conservative assignability check; only clear mismatches are flagged.

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

BEZEL_TEST(sema_argty, matching_int_arg_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int) end F; "
        "  public method M() this.F(5); end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_argty, bool_arg_to_int_param_diagnoses) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int) end F; "
        "  public method M(b: bool) this.F(b); end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_argty, int_widens_to_double_param_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: double) end F; "
        "  public method M() this.F(5); end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_argty, string_arg_to_int_param_diagnoses) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int) end F; "
        "  public method M() this.F(\"hi\"); end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_argty, derived_arg_to_base_param_ok) {
    auto r = parseSemaAll(
        "module M; class Base end Base; class Derived : Base end Derived; "
        "class C "
        "  public method F(b: Base) end F; "
        "  public method M() this.F(new Derived()); end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_argty, unrelated_named_arg_diagnoses) {
    auto r = parseSemaAll(
        "module M; class A end A; class B end B; "
        "class C "
        "  public method F(a: A) end F; "
        "  public method M() this.F(new B()); end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_argty, second_arg_mismatch_diagnoses) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int, b: int) end F; "
        "  public method M(flag: bool) this.F(1, flag); end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
