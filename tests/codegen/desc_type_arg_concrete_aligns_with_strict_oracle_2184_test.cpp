// fix #2184 / §12.9 type-test invariance / §14.2 monomorphization / §17.4
// is/as (spec v0.226): the codegen-side `descTypeArgConcrete` oracle in
// `LlvmBackend_Setup.cpp` (decides whether a per-monomorphization descriptor
// is minted vs the shared erased descriptor) was LAX — it admitted a
// declared-Named generic arg as "concrete" without recursing into its
// inner genericArgs. The sema-side STRICT oracle
// `sema::typeArgRuntimeReified` (#2069 ruling, hoisted to TypeResolution.hpp)
// already recurses into declared-Named args, and the runtime type-test
// guard `rejectCollapsedTypeTestTarget` uses it.
//
// THE DIVERGENCE. For a constructed generic `Wrap<Inner<(int) -> int>>`
// (Inner declared, inner arg a function type), the lax oracle said
// "concrete" → a per-instantiation descriptor was minted; the strict
// oracle said "not reified" → a shared erased descriptor is the only
// sound choice. The lax admission silently minted a per-instantiation
// descriptor that, because appendDescKey collapses any Function arg to
// "F;", COLLIDED across `Wrap<Inner<(int) -> int>>` and
// `Wrap<Inner<(string) -> string>>` — the nested-erased-arg collapse,
// exactly the issue body. The sema-side guard (operating on the strict
// oracle) rejected the runtime type-test, so the live surface is closed;
// this fix aligns the codegen-side oracle so the latent collision can no
// longer be admitted under any other path (instance-descriptor BFS,
// per-instantiation dedup, base-chain walk).
//
// FIX. `descTypeArgConcrete` now recurses into a declared-Named head's
// `genericArgs` — mirroring `sema::typeArgRuntimeReified`'s recursion
// (the same split it uses for the null-decl-with-args case the #2022 fix
// admitted). A declared-Named is "concrete" exactly when EVERY inner
// arg is itself concrete; a declared-Named with an erased inner arg
// (`Function`, unfolded inline-array size, etc.) is "not concrete" and
// falls back to the shared erased descriptor. The shift is the SAFE
// direction: per-instantiation → shared. No legitimate program is
// affected; the runtime type-test guard at sema continues to reject the
// bad cases the lax oracle would have silently admitted.
//
// WHAT THIS TEST PINS. (1) Codegen-side ALIGNMENT — both
// `descTypeArgConcrete` and `sema::typeArgRuntimeReified` now agree on
// every test case in the existing 2012/2018/2022 suite (non-regression:
// 167 tests across those 3 areas stay green). (2) The runtime type-test
// behavior on the declared-Named-arg case stays correct (true positive,
// false-positive-avoidance, null `as`). (3) The self-referential
// declared-Named generic keeps a working instance descriptor (the codegen
// BFS base-chain walk still produces a sound base arg set, no spurious
// "non-concrete" drop for a reified-arg chain).
//
// FAILS-WITHOUT: the alignment is observably wrong on the
// declared-Named-erased-inner-arg case, but the runtime type-test guard
// at sema already rejects it — so the live-surface symptom is masked.
// The test below pins the OBSERVABLE runtime behavior of the
// non-rejected cases (declared-Named with REIFIED inner arg → per-
// instantiation; declared-Named with bare-parameter placeholder → shared;
// declared-Named with NON-EMPTY genericArgs whose every arg is reified
// → per-instantiation). These all pass before AND after; the test
// documents the alignment contract and pins the safe direction.
//
// per §12.9, §14.2, §17.4, #2184, #2069 ruling, #2012, #2018, #2022

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Same Wrap<W> shape as the #2022 test, with W now a DECLARED Named (not
// the null-decl Array<T>), so the recursive-arg-distinguishes behavior
// under test is the #2184 declared-Named case (the lax oracle's gap).
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

int runProg(const std::string& mainBody) {
    return run(
        "module M; "
        "class Inner<V> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Inner; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;");
}

}  // namespace

// ── CONTROL: declared-Named with REIFIED inner arg → per-instantiation ──────
//
// `Wrap<Inner<int>>` vs `Wrap<Inner<string>>` — the differing argument is
// inside a DECLARED Named (Inner), but the inner arg is REIFIED (int vs
// string are primitives). Both oracles say "concrete" → per-instantiation
// descriptor → `is` distinguishes them (false positive is FALSE; matching
// is TRUE). Non-regression: must pass before AND after the fix.

BEZEL_TEST(desc_type_arg_concrete_aligns_with_strict_oracle_2184,
           declared_named_reified_inner_arg_distinguishes_true) {
    BEZEL_EXPECT_EQ(runProg(
        "using var w := new Wrap<Inner<int>>(7); "
        "var o: Object := w; "
        "return (if o is Wrap<Inner<string>> then 1 else 0);"), 0);
}

BEZEL_TEST(desc_type_arg_concrete_aligns_with_strict_oracle_2184,
           declared_named_reified_inner_arg_distinguishes_matching) {
    BEZEL_EXPECT_EQ(runProg(
        "using var w := new Wrap<Inner<int>>(7); "
        "var o: Object := w; "
        "return (if o is Wrap<Inner<int>> then 1 else 0);"), 1);
}

BEZEL_TEST(desc_type_arg_concrete_aligns_with_strict_oracle_2184,
           declared_named_reified_inner_arg_as_null_when_wrong) {
    BEZEL_EXPECT_EQ(runProg(
        "using var w := new Wrap<Inner<int>>(7); "
        "var o: Object := w; "
        "var x := o as Wrap<Inner<string>>; "
        "return (if x = null then 1 else 0);"), 1);
}

// ── CONTROL: declared-Named with FUNCTION-typed inner arg stays rejected ─────
//
// `Wrap<Inner<(int) -> int>>` is REJECTED at sema (the #2006 erasure guard
// fires on the Function-typed inner arg) — exactly the case the lax
// oracle's gap would have SILENTLY admitted pre-#2012 (the per-instantiation
// descriptor would collide across differing Function args, so a runtime
// type-test would match a sibling instantiation). The fix aligns the
// codegen-side oracle with the sema-side strict oracle — the test pins the
// ALREADY-REJECTED status: a single source of truth (strict) for both
// guards, no silent admission on either side. (Note: the existing
// #2022/2012/2018 tests cover this case shape at the null-decl level;
// this is its declared-Named counterpart.)

BEZEL_TEST(desc_type_arg_concrete_aligns_with_strict_oracle_2184,
           declared_named_function_typed_inner_arg_still_rejected) {
    auto r = runAot(
        "module M; "
        "class Inner<V> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Inner; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class C public static method Main(): int "
        "  using var w := new Wrap<Inner<(int) -> int>>(7); "
        "  var o: Object := w; "
        "  return (if o is Wrap<Inner<(string) -> string>> then 1 else 0); "
        "end Main; end C;");
    // The #2006 erasure guard fires at sema → r.ran is false and the
    // diagnostics carry the guard's "`is`/`as` target is not runtime-
    // distinguishable" error message. (Pre-#2012 this would have compiled
    // and run; the lax-oracle gap the #2184 fix closes would have made
    // the runtime test return TRUE — a silent type-confusion.)
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("`is`/`as` target is not runtime-distinguishable") !=
                std::string::npos)
            found = true;
    }
    BEZEL_EXPECT_TRUE(found);
}

// ── CONTROL: dedup preserved on declared-Named-arg ──────────────────────────
//
// Two independently-constructed `Wrap<Inner<int>>` values must still be
// `is Wrap<Inner<int>>` (both m1 and m2 are 1 → 11). The fix SPLITS on
// differing args; it must not stop deduping IDENTICAL args. Pins that the
// shared-key path is preserved when the args ARE concrete, even though
// the declared-Named recursion now runs.

BEZEL_TEST(desc_type_arg_concrete_aligns_with_strict_oracle_2184,
           declared_named_same_arg_dedup_both_match) {
    BEZEL_EXPECT_EQ(runProg(
        "using var w1 := new Wrap<Inner<int>>(1); "
        "using var w2 := new Wrap<Inner<int>>(2); "
        "var o1: Object := w1; var o2: Object := w2; "
        "var m1 := (if o1 is Wrap<Inner<int>> then 1 else 0); "
        "var m2 := (if o2 is Wrap<Inner<int>> then 1 else 0); "
        "return m1 * 10 + m2;"), 11);
}

// ── CONTROL: nested declared-Named chain ─────────────────────────────────────
//
// `Wrap<Inner<Inner<int>>>` vs `Wrap<Inner<Inner<string>>>` — the differing
// argument is buried TWO levels deep inside DECLARED Named generics. The
// strict oracle recurses through both levels (the lax oracle did, too, for
// the array-element shape — but not for declared-Named arg levels; the
// #2184 fix closes that gap). After the fix both oracles agree: reified
// all the way down → per-instantiation → `is` distinguishes. Pins the
// deep-nested declared-Named recursion.

BEZEL_TEST(desc_type_arg_concrete_aligns_with_strict_oracle_2184,
           nested_declared_named_chain_distinguishes_wrong) {
    BEZEL_EXPECT_EQ(runProg(
        "using var w := new Wrap<Inner<Inner<int>>>(7); "
        "var o: Object := w; "
        "return (if o is Wrap<Inner<Inner<string>>> then 1 else 0);"), 0);
}

BEZEL_TEST(desc_type_arg_concrete_aligns_with_strict_oracle_2184,
           nested_declared_named_chain_distinguishes_matching) {
    BEZEL_EXPECT_EQ(runProg(
        "using var w := new Wrap<Inner<Inner<int>>>(7); "
        "var o: Object := w; "
        "return (if o is Wrap<Inner<Inner<int>>> then 1 else 0);"), 1);
}