// Spec-derived tests for fix #4334 — explicit `.Dispose()` through a one-hop
// identifier alias of a non-`owned` (BORROW) PARAMETER double-frees at
// runtime. The pre-existing `checkDisposeReceiverOwnership` already rejects
// `p.Dispose()` directly on the parameter itself (the
// `disposeAliasSource`-unrelated ParameterSymbol arm at Ownership.cpp:8193),
// but its receiver-identity check (`sym = ctx_.exprSymbol(*id)` is the symbol
// the receiver identifier itself resolves to) meant a one-hop identifier
// alias — `var b := p; b.Dispose();` — fell through to the permissive
// "plain-local-held borrow is the unenforced gap" fallback and compiled
// clean, leaving the same `bz_array_free` double-free signature as the whole
// owned-alias family #4236/#4268/#4283/#4286/#4304/#4314/#4321 closes.
//
// Distinct from that whole family: `disposeAliasSource` is populated ONLY
// for owned-source aliases by design (its header comment in
// OwnershipState.hpp enumerates the reasons), so it is structurally unable
// to fire for a non-`owned` parameter source — `isOwnedTrackedLocal` returns
// false for a parameter that is itself a borrow. This fix is the
// borrowed-axis twin, scoped to a single one-hop identifier alias of a
// non-`owned` parameter. Multi-hop alias chains of a borrow source
// (`var b := p; var c := b; c.Dispose()`) are deliberately left as a
// recorded gap, exactly like the owned-source chain case #4263 — the
// blast-radius minimisation discipline is the same.
//
// Spec §28.6 (anchor sec-who-may-dispose, v0.298): "Dispose() through a
// borrow is a compile error. The borrow receivers: a borrowed local or
// parameter ..." — so `b.Dispose()` on `b` (a borrow of a borrow-parameter
// `p`) must be REJECTED at compile time, exactly as `p.Dispose()` already
// is.
//
// THIS FIX SUPPRESSES a free by rejecting the call before codegen ever
// emits it. It does NOT change codegen's own unconditional-free arms
// (LlvmBackend_Calls2.cpp's emitExplicitDisposeChain). The accept-side
// control rows below pin that no false-reject is introduced — an alias of
// a borrow that is never explicitly `.Dispose()`-d must stay accepted, and
// an alias of an `owned` parameter is correctly caught by the existing
// `disposeAliasSource` mechanism (#4236/#4304), NOT by this fix's new
// arm.
//
// One alias shape family × three receiver spellings (`.Dispose()`,
// `?.Dispose()`) × three binding spellings (`var`, `const`, `using var`)
// = the axis grid the test rows below cover; the reject diagnostic text
// identifies the receiver (`b`), the source kind (parameter), and the
// spec authority (§28.6 "Who may call Dispose()").

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// Helper program: a method that takes a borrow parameter and an outer Main
// that constructs a fresh Array, calls the method, and disposes the original
// from Main. Issue #4334's own Repro 2 — slightly more general: works for
// every axis in the grid below.
std::string prog(const std::string& body, const std::string& prelude = "") {
    return "module M; " + prelude +
           "class C "
           "public static method Run(p: Array<int>): int " + body +
           "  end Run; "
           "public static method Main(): int "
           "  var a := new Array<int>(2); a.Add(1); "
           "  var r := Run(a); "
           "  a.Dispose(); "
           "  return r; "
           "end Main; "
           "end C;";
}

}  // namespace

// =====================================================================
// REJECT arm — the issue's own Repro 2, across the axis grid:
//   - receiver spelling (.Dispose() / ?.Dispose())
//   - binding spelling (var / const / using var)
// Each row's first dispose (`b.Dispose()` inside Run) is what this fix
// must catch; the outer Main's `a.Dispose()` is unrelated and runs (if
// reached) only on the ok exits the test program never takes.
// =====================================================================

// Repro 2 verbatim — plain `var` alias of a borrow parameter, dot dispose.
BEZEL_TEST(ownership_dispose_aliased_borrow_parameter_4334,
           array_var_alias_dot_dispose_rejected_28_6) {
    auto r = runAot(prog("var b := p; b.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "alias) of parameter `p`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

// Null-conditional spelling — same source shape, `?.Dispose()` form.
BEZEL_TEST(ownership_dispose_aliased_borrow_parameter_4334,
           array_var_alias_nullcond_dispose_rejected_28_6) {
    auto r = runAot(prog("var b := p; b?.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "alias) of parameter `p`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

// `const` alias is a DIFFERENT AST node (ast::LocalConstDecl, not
// ast::LocalDecl) — the same sibling-walker trap LANE_RULES §6 records
// (seven pre-existing instances; a `const` alias must be tracked
// identically to a plain `var` alias for §28.6 to hold, §21.1/#3687).
BEZEL_TEST(ownership_dispose_aliased_borrow_parameter_4334,
           array_const_alias_dot_dispose_rejected_28_6) {
    auto r = runAot(prog("const b: Array<int> := p; b.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "alias) of parameter `p`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

// `using var` alias — the §28.10 borrow-pin shape (fix #4268 established a
// `using var` over an unmarked read of a named owned binding is a borrow,
// not an adoption). The same shape here is a borrow-pin over a borrow
// parameter; the receiver `.Dispose()` is the §28.6 reject under test.
BEZEL_TEST(ownership_dispose_aliased_borrow_parameter_4334,
           array_using_var_alias_dot_dispose_rejected_28_6) {
    auto r = runAot(prog("using var b: Array<int> := p; b.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "alias) of parameter `p`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

// Order independence: the rejection is a STATIC classification of `b`,
// fixed at its declaration — it must fire whether or not `p` was already
// (separately) disposed first. (A `p.Dispose()` here is itself rejected by
// the existing ParameterSymbol arm, the file's first diagnostic; we test
// the alias arm by leaving `p` itself untouched.)
BEZEL_TEST(ownership_dispose_aliased_borrow_parameter_4334,
           array_var_alias_after_separate_use_rejected_28_6) {
    auto r = runAot(prog(
        "var b := p; "
        "var n := p.Count; "    // legitimate borrow use of `p`
        "b.Dispose(); "
        "return n - n;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "alias) of parameter `p`"));
}

// =====================================================================
// ACCEPT-side controls (LANE_RULES §15) — programs that MUST keep
// compiling and running correctly; a false-reject here would be worse
// than the bug itself.
// =====================================================================

// Alias of a borrow parameter, but the alias is NEVER explicitly
// `.Dispose()`-d — the rejection fires only at the call site, not at the
// binding. Loop-amplified 200x under `runAotChecked` (generated-code
// ASan/LSan, LANE_RULES §3): a wrongly-reintroduced double-free would
// surface here.
BEZEL_TEST(ownership_dispose_aliased_borrow_parameter_4334,
           array_var_alias_never_disposed_frees_exactly_once_28_6) {
    auto r = runAotChecked(
        "module M; class C "
        "public static method Run(p: Array<int>): int "
        "  var b: Array<int> := p; "
        "  var n := b.Count; "
        "  return n - n; "
        "end Run; "
        "public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    var a := new Array<int>(2); a.Add(1); "
        "    Run(a); "
        "    a.Dispose(); "
        "    i := i + 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Alias of an OWNED parameter — must still be rejected, AND still be
// rejected by the EXISTING `disposeAliasSource` arm (the owned-source
// family, #4236/#4304), NOT by this fix's new borrow arm. The two arms
// are disjoint because an `owned` parameter is an `isOwnedTrackedLocal`
// (the gate the existing arm fires on) and also passes the
// `modifier == "owned"` gate that excludes it from this fix's new arm.
// This row's diagnostic asserts the owned-source wording —
// "borrow (an alias) of `p`, which still owns it" — to pin that the
// existing arm, not the new arm, caught this shape. (If the new arm ever
// widened to also catch it, this assertion would fail and the diagnostic
// wording would change — that is the only way the new arm would be
// load-bearing here.)
BEZEL_TEST(ownership_dispose_aliased_borrow_parameter_4334,
           array_var_alias_of_owned_parameter_still_owned_axis_28_6) {
    auto r = runAot(
        "module M; class C "
        "public static method Take(owned p: Array<int>): int "
        "  var b: Array<int> := p; "
        "  b.Dispose(); "
        "  p.Dispose(); "
        "  return 0; "
        "end Take; "
        "public static method Main(): int "
        "  var a := new Array<int>(2); a.Add(1); "
        "  return Take(move a); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `p`"));
    // The owned-source wording ("which still owns it") — the EXISTING
    // arm's signature, not this fix's new arm's ("is itself a borrow").
    BEZEL_EXPECT_TRUE(diagContains(r, "which still owns it"));
}

// Direct `.Dispose()` on the borrow parameter itself (Repro 1's control) —
// must keep being rejected by the existing ParameterSymbol arm, exactly
// the wording the lit test `dispose_borrow_receiver_1653.bzl` pins. This
// row is the cross-reference: the new arm catches ONLY the alias shape,
// not the direct form.
BEZEL_TEST(ownership_dispose_aliased_borrow_parameter_4334,
           array_direct_param_dispose_still_rejected_by_old_arm_28_6) {
    auto r = runAot(prog("p.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on parameter `p`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-`owned` parameter is a borrow"));
}

// UNAFFECTED — the pre-existing, documented gap this fix does NOT close:
// a multi-hop alias chain of a borrow source. `b` aliases `p`; `c` aliases
// `b`; `c.Dispose()` would still double-free through `p`, but the chain
// is left unenforced (the existing `disposeAliasSource` family also does
// not chain — that is the #4263 record, and this fix follows the same
// blast-radius discipline on the borrow axis).
//
// `c.Dispose()` here is currently silent — pre-fix, `c.Dispose()` would
// crash at runtime (the same `bz_array_free` double-free). This row pins
// the CURRENT unenforced-chain behaviour so a future chain-extension fix
// (matching the owned-axis #4263 chain fix) has a working row to verify
// against. Marked `DISABLED` until that follow-up lands — leaving it as a
// failing row today would hide the gap behind a known-rejected test.
//   Disabled here because we cannot ship a test that fails without the
//   fix AND fails with it (no follow-up yet exists to chain through);
//   the runAotChecked under ASan IS the eventual accept-side control.
// Re-enable when #4263's chain sibling lands.
// DISABLED: ownership_dispose_aliased_borrow_parameter_4334.chain_two_hops_unenforced_today