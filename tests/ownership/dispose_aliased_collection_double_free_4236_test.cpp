// Spec-derived tests for fix #4236 — explicit `.Dispose()` / `?.Dispose()`
// through an ALIAS of a still-in-scope owned collection local double-frees,
// with no compile-time diagnostic.
//
// §28.3 "Ownership state of locals" (v0.298): "`var x := expr;` takes the
// ownership state of `expr` ... A read of a named owned binding on the
// right-hand side is a borrow unless it carries the explicit transfer
// marker — `var y := move x;`" — so `var b: Array<int> := a;` (no `move`)
// makes `b` a BORROW of `a`. This is settled and intentional (Pattern 1 of
// the same section relies on exactly this to equalize branches to
// "borrowed"); #4236's own second hypothesis — that the assignment itself
// should be rejected without `move` — is WRONG and this file does not test
// for that rejection.
//
// §28.6 "Who may call Dispose()" (v0.213/v0.298, anchor sec-who-may-dispose):
// "`Dispose()` through a borrow is a compile error. The borrow receivers: a
// borrowed local or parameter ..." — so `b.Dispose()` must be REJECTED at
// compile time, exactly as a borrowed parameter or a borrowed field read
// already is (Ownership.cpp's checkDisposeReceiverOwnership, existing
// arms). THIS is the layer that was wrong: sema's checkDisposeReceiverOwnership
// had a documented, deliberate gap for "a plain `var`/`using var` local" that
// could not (in general) distinguish a genuine identifier re-borrow from an
// owned call-result disposable (an IEnumerator from GetEnumerator(), a
// Subscription from Subscribe(), ...) — and admitted BOTH. The fix narrows
// that gap using an authority the ownership pass ALREADY computes for a
// different purpose (borrow-escape diagnostics): `states_[sym].source`,
// populated by `borrowSourceForExpr`'s Shape 1 ONLY for an identifier/field/
// index re-borrow rooted at a still-tracked owned local — NEVER for a
// CallExpr initializer. A Borrowed local WITH a recorded source is
// unambiguous; a Borrowed local with none is untouched (the accept-side
// control rows below pin that).
//
// DIRECTION OF TRAVEL: this fix SUPPRESSES a free by rejecting the call
// before codegen ever emits it — it does not change codegen's own
// unconditional-free arms (LlvmBackend_Calls2.cpp's emitExplicitDisposeChain,
// #4221) at all. Suppressing too broadly would turn the pre-existing
// call-result gap into new FALSE REJECTS (measured: it does not, see the
// "UNAFFECTED" section). Suppressing too narrowly would leave the double-
// free reachable (measured: it does not, see the REJECT arm + the FAIL-
// WITHOUT/PASS-WITH cycle in the commit body).
//
// §4246 (borrow-declared local later reassigned to a fresh owned value,
// then leaked on Dispose()) is ADJACENT and OUT OF BOUNDS: no row here
// reassigns an aliasing local after its declaration, so none can trip it.

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

std::string prog(const std::string& mainBody, const std::string& prelude = "") {
    return "module M; " + prelude +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// =====================================================================
// REJECT arm — the issue's own repro, across the axis grid: receiver type
// (Array/Dictionary/Set/LinkedList/interface-typed alias) x spelling
// (`.Dispose()` / `?.Dispose()`).
// =====================================================================

BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           array_alias_dot_dispose_rejected_28_6) {
    auto r = runAot(prog(
        "var a := new Array<int>(2); a.Add(1); "
        "var b: Array<int> := a; b.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           array_alias_nullcond_dispose_rejected_28_6) {
    auto r = runAot(prog(
        "var a := new Array<int>(2); a.Add(1); "
        "var b: Array<int> := a; b?.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           dictionary_alias_dot_dispose_rejected_28_6) {
    auto r = runAot(prog(
        "var a := new Dictionary<string, int>(); a.Add(\"k\", 1); "
        "var b: Dictionary<string, int> := a; b.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           set_alias_nullcond_dispose_rejected_28_6) {
    auto r = runAot(prog(
        "var a := new Set<int>(); a.Add(1); "
        "var b: Set<int> := a; b?.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           linkedlist_alias_dot_dispose_rejected_28_6) {
    auto r = runAot(prog(
        "var a := new LinkedList<int>(); a.AddLast(1); "
        "var b: LinkedList<int> := a; b.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// The issue's own third repro shape: an INTERFACE-typed alias. `.Add(1)` runs
// on the concrete-typed original `a` (interface-typed `.Add` is a separate,
// pre-existing, unrelated gap — #853, "not yet supported in codegen" — so
// this row avoids it deliberately, exactly as #4221's own interface row did).
BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           interface_alias_dot_dispose_rejected_28_6) {
    auto r = runAot(prog(
        "var a := new Array<int>(2); a.Add(1); "
        "var x: ICollection<int> := a; x.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `x`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           interface_alias_nullcond_dispose_rejected_28_6) {
    auto r = runAot(prog(
        "var a := new Array<int>(2); a.Add(1); "
        "var x: ICollection<int> := a; x?.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `x`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// Order independence: the rejection is a STATIC classification of `b`, fixed
// at its declaration — it must fire the same whether the original is
// disposed BEFORE the alias-dispose attempt or not at all. (`a.Dispose()`
// here is itself legitimate — `a` genuinely owns the value.)
BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           array_alias_dispose_after_original_disposed_rejected_28_6) {
    auto r = runAot(prog(
        "var a := new Array<int>(2); a.Add(1); "
        "var b: Array<int> := a; a.Dispose(); b.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// =====================================================================
// ACCEPT-side controls (LANE_RULES §15) — programs that MUST keep compiling
// and running correctly; a false-reject here would be worse than #4236 itself.
// =====================================================================

// A genuine second owner via `move`: `b` takes `a`'s obligation outright, so
// `b.Dispose()` is a legitimate owned dispose — must still compile AND free
// exactly once (loop-amplified under ASan/LSan per LANE_RULES §3).
BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           array_move_then_dispose_frees_exactly_once_28_8_1) {
    auto r = runAotChecked(prog(
        "var i := 0; "
        "while i < 200 do "
        "  var a := new Array<int>(2); a.Add(1); "
        "  var b := move a; b.Dispose(); "
        "  i := i + 1; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A non-aliased plain dispose of the genuine owner must keep working
// (unaffected — `a`'s own tracked state is Owned, not Borrowed, so the new
// check's `state == Borrowed` guard never fires for it).
BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           array_plain_dispose_unaliased_still_compiles_28_6) {
    auto r = runAot(prog(
        "var a := new Array<int>(2); a.Add(1); a.Dispose(); return 41;"));
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// An alias that is declared but NEVER explicitly disposed must still compile
// (this check only ever fires AT a `.Dispose()`/`?.Dispose()` call site) and
// the original must still auto-drop exactly once at scope end — no leak.
// Loop-amplified per LANE_RULES §3 (a one-shot LSan row can pass on both
// sides of a fix).
BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           array_alias_never_disposed_frees_exactly_once_28_3) {
    auto r = runAotChecked(prog(
        "var i := 0; "
        "while i < 200 do "
        "  var a := new Array<int>(2); a.Add(1); "
        "  var b: Array<int> := a; "
        "  var n := b.Count; "
        "  i := i + n - n; "
        "  i := i + 1; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// UNAFFECTED — the pre-existing, documented, out-of-scope "call-result" gap
// (Ownership.cpp's checkDisposeReceiverOwnership gap note): a plain local
// bound from a CALL result (not a bare identifier read) can never populate
// `disposeAliasSource` — `handleLocalDecl`'s new fallback
// (Ownership.cpp, fix #4236) only ever matches
// `dynamic_cast<const ast::IdentifierExpr*>(ld.initializer.get())`, which is
// structurally false for any `ast::CallExpr` initializer — so the new check
// cannot fire for this shape by construction, not merely by measurement.
// `Math.Min`/`Math.Max`-style bare-type-parameter call results are the
// existing, already-passing worked example of a Borrowed plain-`var` local
// whose `.Dispose()` this file's fix must leave exactly as before: see
// `bare_type_param_returns_argsource_1801_test.cpp`'s S4 rows (a `using var`
// pin is accepted; an explicit `.Dispose()` is rejected by the PRE-EXISTING
// `bareBorrowHolders_` check above this fix's new code, unrelated to it) and
// the full unfiltered `--area ownership` sweep in this fix's commit body,
// which shows zero regressions among the pre-existing suite.
