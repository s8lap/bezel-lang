// Spec-derived tests for Sema slice S20 — an override must match the
// base member's signature exactly (§12.1): visibility, return type,
// parameter types, parameter modifiers (ref/out/owned), and the owned
// return modifier.

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

} // namespace

BEZEL_TEST(sema_override, matching_override_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual method F(a: int): int => a; end Base; "
        "class D : Base public override method F(a: int): int => a; end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_override, return_type_mismatch_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual method F(): int => 1; end Base; "
        "class D : Base public override method F(): bool => true; end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_override, param_type_mismatch_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual method F(a: int) _ := 1; end F; end Base; "
        "class D : Base public override method F(a: bool) _ := 2; end F; end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_override, param_count_mismatch_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual method F(a: int) _ := 1; end F; end Base; "
        "class D : Base public override method F(a: int, b: int) _ := 2; end F; end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_override, param_modifier_mismatch_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual method F(a: int) _ := 1; end F; end Base; "
        "class D : Base public override method F(ref a: int) _ := 2; end F; end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_override, visibility_mismatch_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual method F() _ := 1; end F; end Base; "
        "class D : Base internal override method F() _ := 2; end F; end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_override, owned_return_mismatch_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class T end T; "
        "class Base public virtual method F(): owned T => new T(); end Base; "
        "class D : Base public override method F(): T => new T(); end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_override, matching_named_param_type_override_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class T end T; "
        "class Base public virtual method F(t: T): T => t; end Base; "
        "class D : Base public override method F(t: T): T => t; end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
