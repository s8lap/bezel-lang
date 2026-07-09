// Spec-derived tests for #1614 — a property/indexer `override` must match the
// base member's signature exactly (§12.1): visibility, the property (return)
// type, the accessor set/get presence, and (for indexers) the index-parameter
// signature (count, modifiers, types). Before #1614 a property/indexer
// override went through NO signature-matching check at all —
// checkOverrideSignatureMatch dynamic_cast both sides to MethodSymbol and
// returned immediately when either was a PropertySymbol — so any mismatch was
// silently accepted.
//
// §12.1 (v0.181, #1560) admits the same parameter-nullability widening a
// method override enjoys: an override's INDEX parameter may widen the base's
// `T` to `T?` (an index parameter is an input, like a method parameter). The
// property TYPE is a return type (the getter's result / the setter's `value`),
// so it must match exactly — no covariance in this version.
//
// §12.5: the abstract shorthand `abstract property Name: T;` declares a single
// abstract getter (get-only); the common `override property Name: T => e;`
// idiom overriding it must stay accepted (it is NOT an accessor-presence
// mismatch).

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

// ── Property type (the §12.1 "return type") ───────────────────────────────

// §12.1 / #1614: a matching property override (same type) compiles.
BEZEL_TEST(sema_prop_override_1614, matching_property_override_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual property Name: string => \"base\"; end Base; "
        "class D : Base public override property Name: string => \"d\"; end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §12.1 / #1614: a property override with a MISMATCHED property type is
// rejected (previously silently accepted — the bug this test pins).
BEZEL_TEST(sema_prop_override_1614, property_type_mismatch_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual property Name: string => \"base\"; end Base; "
        "class D : Base public override property Name: int => 42; end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §12.1 / #1614: a property override with the base's type under a different
// (nullable) form is NOT a match — the property type is a return type, so the
// §12.1 T→T? parameter widening does NOT apply (no covariance).
BEZEL_TEST(sema_prop_override_1614, property_type_nullable_mismatch_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual property Name: string => \"base\"; end Base; "
        "class D : Base public override property Name: string? => null; end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ── Visibility ────────────────────────────────────────────────────────────

// §12.1 / #1614: a property override with mismatched visibility is rejected.
BEZEL_TEST(sema_prop_override_1614, visibility_mismatch_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual property Name: string => \"b\"; end Base; "
        "class D : Base internal override property Name: string => \"d\"; end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ── Accessor set/get presence ─────────────────────────────────────────────

// §12.1 / #1614: an override that DROPS the base's setter (base get+set,
// override get-only) is rejected — it changes the vtable accessor-slot shape.
BEZEL_TEST(sema_prop_override_1614, dropped_setter_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base "
        "  private _x: int := 0; "
        "  public virtual property X: int get => _x; set(v) => _x := v; end; "
        "end Base; "
        "class D : Base public override property X: int => 7; end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §12.1 / #1614: an override that ADDS a setter the get-only base lacks is
// likewise rejected (the accessor sets must match).
BEZEL_TEST(sema_prop_override_1614, added_setter_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual property X: int => 0; end Base; "
        "class D : Base "
        "  private _x: int := 0; "
        "  public override property X: int get => _x; set(v) => _x := v; end; "
        "end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §12.1 / #1614: a matching get+set block-body override (base get+set,
// override get+set) compiles.
BEZEL_TEST(sema_prop_override_1614, matching_block_getset_override_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Base "
        "  private _x: int := 0; "
        "  public virtual property X: int get => _x; set(v) => _x := v; end; "
        "end Base; "
        "class D : Base "
        "  private _y: int := 0; "
        "  public override property X: int get => _y; set(v) => _y := v; end; "
        "end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── §12.5 abstract shorthand (must NOT be over-rejected) ──────────────────

// §12.5 / #1614: the abstract shorthand `abstract property Name: T;` declares
// a single get-only abstract getter; overriding it with an expression-bodied
// get-only property is the canonical idiom and must compile.
BEZEL_TEST(sema_prop_override_1614, abstract_shorthand_override_ok) {
    auto r = parseSemaAll(
        "module M; "
        "abstract class Base public abstract property Name: string; end Base; "
        "class D : Base public override property Name: string => \"d\"; end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── Indexers (§23): the index-parameter signature ─────────────────────────

// §23 / §12.1 / #1614: a matching indexer override compiles.
BEZEL_TEST(sema_prop_override_1614, matching_indexer_override_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual property [i: int]: int => 0; end Base; "
        "class D : Base public override property [i: int]: int => i; end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §23 / §12.1 / #1614: an indexer override with a mismatched index-parameter
// TYPE is rejected (previously silently accepted — the bug this test pins).
BEZEL_TEST(sema_prop_override_1614, indexer_param_type_mismatch_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual property [i: int]: int => 0; end Base; "
        "class D : Base public override property [i: bool]: int => 0; end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §23 / §12.1 / #1614: an indexer override with a mismatched index-parameter
// COUNT is rejected.
BEZEL_TEST(sema_prop_override_1614, indexer_param_count_mismatch_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual property [i: int]: int => 0; end Base; "
        "class D : Base public override property [i: int, j: int]: int => 0; "
        "end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §23 / §12.1 (v0.181, #1560) / #1614: an indexer's index parameter may
// WIDEN the base's `T` to `T?` — it is a parameter (an input), so the same
// contravariant nullability allowance §12.1 grants method parameters applies.
BEZEL_TEST(sema_prop_override_1614, indexer_param_widening_override_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual property [s: string]: int => 0; end Base; "
        "class D : Base public override property [s: string?]: int => 0; "
        "end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §23 / §12.1 (v0.181, #1560) / #1614: the reverse direction — base `T?`,
// override `T` — is a narrowing and is rejected (a null admitted through the
// base's static type would reach a non-nullable index parameter).
BEZEL_TEST(sema_prop_override_1614, indexer_param_narrowing_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual property [s: string?]: int => 0; end Base; "
        "class D : Base public override property [s: string]: int => 0; end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §23 / §12.5 / #1614: an abstract indexer shorthand overridden by a matching
// indexer compiles (the §12.5 abstract-getter idiom for indexers).
BEZEL_TEST(sema_prop_override_1614, abstract_indexer_override_ok) {
    auto r = parseSemaAll(
        "module M; "
        "abstract class Base public abstract property [i: int]: int; end Base; "
        "class D : Base public override property [i: int]: int => i; end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
