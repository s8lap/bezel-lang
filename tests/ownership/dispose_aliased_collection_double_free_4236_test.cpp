// Spec-derived tests for fix #4236 — explicit `.Dispose()` / `?.Dispose()`
// through an ALIAS of a still-in-scope owned collection local double-frees,
// with no compile-time diagnostic. EXTENDED by fix #4268 (§28.10, v0.298,
// below the send-back sections) to the identical shape spelled `using var`
// instead of plain `var`/`const` — see that section's own header for why the
// original fix's `!ld.isUsing` exclusion was a misreading of §28.10, not a
// deliberate narrowing.
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
// Subscription from Subscribe(), ...) — and admitted BOTH.
//
// THE MECHANISM, ACTUALLY SHIPPED (an earlier revision of this comment
// described a first attempt — reusing `states_[sym].source`, populated by
// `borrowSourceForExpr` — that was abandoned: `borrowSourceForExpr`'s Shape 1
// gate, `returnedExprMayBorrow -> isBorrowableType`, answers FALSE for every
// compiler-builtin collection (Array/Dictionary/Set/LinkedList/...) — a Named
// TypeRef with a null `declaration` is the SAME SHAPE `isBorrowableType` uses
// for an unresolved generic parameter — so `.source` is never populated for a
// collection-typed alias local at all, and that first attempt built and ran
// green on its own tests while producing ZERO rejections on any of #4236's
// collection repros. Do not repeat that attempt.): a new, DEDICATED field,
// `LocalOwnership::disposeAliasSource` (OwnershipState.hpp), consumed ONLY by
// `checkDisposeReceiverOwnership`. Populated at a plain LocalDecl
// (`handleLocalDecl`), at a `const` LocalConstDecl (`visitStmt`'s
// LocalConstDecl arm — a send-back fix; the field is a DIFFERENT AST node
// from LocalDecl and was originally missed there, LANE_RULES §6's sibling
// trap), re-derived at every plain reassignment (`handleAssignment`), and
// OR-merged across a branch join that converges to the same state
// (`joinInto`, Ownership_Checks.cpp — also a send-back fix, see the
// branch-order-luck rows below) — always via the SAME narrow test: a bare
// IDENTIFIER read (never a member/index/other projection) whose resolved
// symbol is, right now, a still-Owned tracked local (`isOwnedTrackedLocal`).
// A local WITH a recorded `disposeAliasSource` is an unambiguous alias; a
// local with none is untouched (the accept-side control rows below pin
// that).
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
// SEND-BACK gap (a) — a `const` alias is a DIFFERENT AST node
// (`ast::LocalConstDecl`, not `ast::LocalDecl`) and was never visited by the
// original fix at all: it reproduced the identical `free(): invalid pointer`
// / SIGABRT 134 crash the whole fix exists to close. §21.1/#3687 established
// a `const` is tracked "exactly as a plain unmarked `var` alias" — this row
// is that statement's Dispose() consequence.
// =====================================================================

BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           const_array_alias_dot_dispose_rejected_28_6) {
    auto r = runAot(prog(
        "var a := new Array<int>(2); a.Add(1); "
        "const b: Array<int> := a; b.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// =====================================================================
// SEND-BACK gap (b) — `joinInto`'s same-state early `continue` kept only the
// FIRST-LISTED branch's `disposeAliasSource`, so whether this program was
// rejected was pure branch-order luck. Both arm orders must reject
// IDENTICALLY: swapping `then`/`else` must not change the verdict.
// =====================================================================

// `then` sets the alias (front branch, previously WON the join) — was
// already correctly rejected pre-send-back; pinned here as the control that
// proves the merge fix did not disturb the order that already worked.
BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           branch_join_alias_then_arm_rejected_28_3) {
    auto r = runAot(prog(
        "var cond := false; "
        "var a := new Array<int>(2); a.Add(1); "
        "var b: Array<int>; "
        "if cond then b := a; else b := Helper.Peek(a); end; "
        "b.Dispose(); return 0;",
        "class Helper public static method Peek(x: Array<int>): Array<int> "
        "return x; end Peek; end Helper; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// `else` sets the alias (second-listed branch, previously LOST the join and
// its `disposeAliasSource` was discarded) — this is the shape that was
// silently ACCEPTED and double-freed pre-send-back.
BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           branch_join_alias_else_arm_rejected_28_3) {
    auto r = runAot(prog(
        "var cond := false; "
        "var a := new Array<int>(2); a.Add(1); "
        "var b: Array<int>; "
        "if cond then b := Helper.Peek(a); else b := a; end; "
        "b.Dispose(); return 0;",
        "class Helper public static method Peek(x: Array<int>): Array<int> "
        "return x; end Peek; end Helper; "));
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

// =====================================================================
// fix #4268 (§28.10, v0.298) — the identical alias shape, spelled `using var`
// instead of plain `var`/`const`. `Ownership.cpp`'s `handleLocalDecl` gated
// `disposeAliasSource`'s population on `!ld.isUsing`, reasoned (per its own,
// now-corrected, comment) as "a `using var` is itself Owned, not a
// re-borrow". §28.10's OWN dedicated subsection, "using var initialized from
// a borrow", says the opposite BY NAME for this exact RHS shape — "a
// method call without an `owned` return ... an *unmarked* read of a named
// owned binding[: ...] the `using var` does **not** assume a disposal
// obligation ... the value is not owned by this scope" — and §28.10's
// *Adopting a named owned binding* bullet restates it from the adopt side:
// "An **unmarked** `using var y := x;` where `x` is a named owned binding
// does *not* adopt: it is a borrow". That is the SAME K3.13 "marker economy"
// rule (§28.3, "An unmarked read of a class-typed binding never consumes")
// this file's #4236 rows already rely on for plain `var`/`const` — `using
// var` carries no exemption from it. So `using var b := a;` (no `move`) is a
// borrow exactly like `var b := a;`, and §28.6 "Who may call Dispose()"
// ("Dispose() through a borrow is a compile error") applies identically:
// `b.Dispose()` must be rejected. The fix widens `disposeAliasSource`'s
// existing, already-validated population to also run for `using var` —
// feeding the SAME authority #4236 established rather than adding a new
// predicate (LANE_RULES §6/§25) — rather than adding a NEW check that
// consults the vacuous-pin lint's own (separate, differently-scoped)
// classifier.
// =====================================================================

BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           using_var_alias_dot_dispose_rejected_28_10) {
    auto r = runAot(prog(
        "var a := new Array<int>(2); a.Add(1); "
        "using var b: Array<int> := a; b.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           using_var_alias_nullcond_dispose_rejected_28_10) {
    auto r = runAot(prog(
        "var a := new Array<int>(2); a.Add(1); "
        "using var b: Array<int> := a; b?.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// Order independence, mirroring the plain-`var` row above: the rejection is
// a static classification of `b` fixed at its declaration, so it must fire
// whether or not the original was ALREADY disposed first (`a.Dispose()` here
// is itself legitimate — `a` genuinely owns the value).
BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           using_var_alias_dispose_after_original_disposed_rejected_28_10) {
    auto r = runAot(prog(
        "var a := new Array<int>(2); a.Add(1); "
        "using var b: Array<int> := a; a.Dispose(); b.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// The axis this file's earlier rows do not cover: an alias of an OWNED
// PARAMETER (not a local). `isOwnedTrackedLocal` reads `states_[sym].state`,
// which is seeded Owned for an `owned` parameter exactly as for a fresh
// local, so the same bare-identifier check fires. `a.Dispose()` after the
// rejected `b.Dispose()` line discharges the parameter's own separate,
// unrelated "owned parameter must be discharged" obligation (§28.3) so this
// row's ONE diagnostic is unambiguously the alias rejection under test.
BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           using_var_alias_of_owned_parameter_dispose_rejected_28_10) {
    auto r = runAot(
        "module M; class C "
        "public static method Take(owned a: Array<int>): int "
        "using var b: Array<int> := a; b.Dispose(); a.Dispose(); return 0; "
        "end Take; "
        "public static method Main(): int "
        "var a := new Array<int>(2); a.Add(1); return Take(move a); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// =====================================================================
// fix #4268 ACCEPT-side controls (LANE_RULES §15) — the two `using var`
// shapes §28.10 requires to KEEP working must not be false-rejected by the
// widened check. Neither initializer is an `IdentifierExpr` (a `NewExpr` and
// a `CallExpr` respectively), so `disposeAliasSource`'s dynamic_cast fails
// for both by CONSTRUCTION, not merely by measurement — the widening moves
// along exactly one axis (bare-identifier-of-a-still-owned-local) and no
// other. Loop-amplified 200x under `runAotChecked` (generated-code ASan/LSan
// per-test, LANE_RULES §3/§30): a false-reject would show as a compile
// failure (r.ran false); a wrongly-reintroduced double-free or a leak from
// this widening would show as an ASan/LSan report on the SAME line.
// =====================================================================

BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           using_var_fresh_producer_still_compiles_and_runs_28_10) {
    auto r = runAotChecked(prog(
        "var i := 0; "
        "while i < 200 do "
        "  using var b := new Array<int>(2); b.Add(1); "
        "  var n := b.Count; "
        "  i := i + n - n; "
        "  i := i + 1; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// The same fresh-producer shape, but ALSO explicitly `.Dispose()`d before
// scope end — `using var` is one of §28.6's own listed owning receivers
// ("an owned local ... a `using var` ...") for a GENUINE producer, and that
// must stay legal; the disposed-flag machinery (§28.2, idempotent) means
// scope end then no-ops rather than double-freeing.
BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           using_var_fresh_producer_explicit_dispose_still_compiles_28_10) {
    auto r = runAotChecked(prog(
        "var i := 0; "
        "while i < 200 do "
        "  using var b := new Array<int>(2); b.Add(1); "
        "  b.Dispose(); "
        "  i := i + 1; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           using_var_call_result_dispose_still_compiles_28_10) {
    auto r = runAotChecked(prog(
        "var i := 0; "
        "while i < 200 do "
        "  using var b := Helper.Make(); b.Dispose(); "
        "  i := i + 1; "
        "end; "
        "return 0;",
        "class Helper public static method Make(): owned Array<int> "
        "var x := new Array<int>(2); x.Add(1); return x; end Make; "
        "end Helper; "));
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// The plain-`var` sibling row's control, spelled `using var`, WAS accepted
// through spec v0.345 ("an alias that is declared but NEVER explicitly
// disposed must still compile"). Spec v0.350 §28.10 *Adopting a named owned
// binding* makes the unmarked `using var b: Array<int> := a;` over the owned
// local `a` a COMPILE ERROR (it says neither adopt nor view), so the row now
// pins the rejection and names the view remedy; the view spelling `var`
// is the sibling row above.
BEZEL_TEST(ownership_dispose_aliased_collection_4236,
           using_var_alias_never_disposed_frees_exactly_once_28_10) {
    auto r = runAot(prog(
        "var i := 0; "
        "while i < 200 do "
        "  var a := new Array<int>(2); a.Add(1); "
        "  using var b: Array<int> := a; "
        "  var n := b.Count; "
        "  i := i + n - n; "
        "  i := i + 1; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "move a"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.10"));
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
