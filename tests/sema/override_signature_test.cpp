// Spec-derived tests for Sema slice S20 — an override must match the
// base member's signature exactly (§12.1): visibility, return type,
// parameter types, parameter modifiers (ref/out/owned), and the owned
// return modifier.
//
// §12.1 (v0.181, #1560) admits one exception to the "exactly" rule: an
// override may WIDEN a parameter from the base's `T` to `T?` (nullable),
// monotonically down the inheritance chain — mirroring §11.2's identical
// allowance for interface implementations. The reverse direction (base
// `T?`, override `T`, i.e. narrowing) stays rejected, as does narrowing
// back after an intermediate override has already widened.

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

// §12.1 (v0.181, #1560): an override may widen a parameter from the base's
// `T` (string) to `T?` (string?) — the same contravariant nullability
// allowance §11.2 grants interface implementations ("an implementation may
// declare a parameter as `T?` where the interface requires `T`").
BEZEL_TEST(sema_override, widening_param_nullability_override_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual method F(a: string) _ := 1; end F; end Base; "
        "class D : Base public override method F(a: string?) _ := 2; end F; end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §12.1 (v0.181, #1560): the reverse direction — base `T?`, override `T` —
// is NOT a match and stays rejected: a narrowing override would let a
// `null` admitted through the base's static type reach a non-nullable
// parameter (mirrors §11.2's identical reverse-direction rejection for
// interface implementations).
BEZEL_TEST(sema_override, narrowing_param_nullability_override_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual method F(a: string?) _ := 1; end F; end Base; "
        "class D : Base public override method F(a: string) _ := 2; end F; end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §12.1 (v0.181, #1560): an override matching the base's ALREADY-nullable
// parameter type exactly (no widening involved) is unaffected — a normal
// exact match still passes.
BEZEL_TEST(sema_override, exact_nullable_param_type_override_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual method F(a: string?) _ := 1; end F; end Base; "
        "class D : Base public override method F(a: string?) _ := 2; end F; end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §12.1 (v0.181, #1560): widening is monotone DOWN THE CHAIN — an override
// matches against the NEAREST overridden declaration. Once Mid widens
// Base's `a: string` to `a: string?`, Grand's override matches against
// Mid's (already-widened) declaration, so Grand may keep `string?` but may
// NOT narrow back to `string` — even though `string` is what the original
// (Base) declaration used. (Regression coverage: the override-match check
// must select Mid, the nearest ancestor, as the base to validate against —
// not fall through to Base merely because Grand's narrower signature
// happens to match Base's original, un-widened one.)
BEZEL_TEST(sema_override, narrowing_after_intermediate_widening_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual method F(a: string) _ := 1; end F; end Base; "
        "class Mid : Base public override method F(a: string?) _ := 2; end F; end Mid; "
        "class Grand : Mid public override method F(a: string) _ := 3; end F; end Grand;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §12.1 (v0.181, #1560): the positive half of the monotone chain — once
// Mid widens to `string?`, Grand overriding with `string?` again (staying
// widened, not narrowing) is accepted.
BEZEL_TEST(sema_override, widened_override_staying_widened_down_chain_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual method F(a: string) _ := 1; end F; end Base; "
        "class Mid : Base public override method F(a: string?) _ := 2; end F; end Mid; "
        "class Grand : Mid public override method F(a: string?) _ := 3; end F; end Grand;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #1592 follow-up (filed alongside #1560): a VALUE-typed parameter (`int`)
// widening is declined for now, with the same §12.1 diagnostic as any other
// mismatch. Unlike a reference type (class/interface/string/Object), whose
// `T` and `T?` forms share one runtime representation, a nullable value
// type lowers to a distinct boxed representation; admitting the widening
// without a vtable adapter thunk (the itable case already has one,
// #1139/#1183) would let a virtual call built from a non-widened ancestor's
// static type corrupt the call. See #1592 for the follow-up.
BEZEL_TEST(sema_override, value_type_param_widening_declined_for_abi_safety) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual method F(a: int) _ := 1; end F; end Base; "
        "class D : Base public override method F(a: int?) _ := 2; end F; end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
