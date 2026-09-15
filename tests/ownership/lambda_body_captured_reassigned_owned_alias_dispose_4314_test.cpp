// Spec-derived tests for fix #4314 — the REASSIGNMENT-becomes-owned sibling
// of fix #4286. #4286 closed the lambda-body capture gap for
// `disposeAliasSource` when the captured symbol is an owned local whose OWN
// DECLARATION INITIALIZER is a fresh owned production (`var a := new
// Array<int>(2); ...`). Its new predicate, `isOwnedTrackedLocalInEnclosingFrame`
// (Ownership_Checks.cpp), is purely a function of `ld->initializer` and is
// blind to a captured local that becomes owned via a later plain
// REASSIGNMENT instead — the spec's own "Pattern 2" idiom (§K1.7/K3.11
// worked example: "the same declaration `var x: Stream;` leaves `x` borrowed
// in Pattern 1 and owned in Pattern 2 — the ASSIGNMENTS decide, not the
// declaration"):
//
//   var a: Array<int>;
//   a := new Array<int>(2);
//   var f := lambda() => do var b := a; b.Dispose(); return 0; end;
//
// compiled clean and double-freed in `bz_array_free`, byte-for-byte the
// #4236/#4268/#4283/#4286 crash signature (measured live SIGSEGV under
// ASan). `ld->initializer` is null here, so #4286's own retry never engages.
//
// WHY NOT `ld->becomesOwnedViaReassign`: that flag is stamped by
// `handleAssignment` as part of the MAIN per-statement dataflow walk over
// the method body, which runs strictly AFTER `applyUsingVarBorrowPinInLambdas`
// (the lambda-body pre-pass this fix's call sites run in — see
// `MethodOwnership::run()`, Ownership.cpp). At the point this fix's checks
// run, that flag is exactly as unset as `ld->autoDropAtScopeEnd` was
// measured to be for #4286's own rejected `enclosingFrameOwnedLocalBorrowSource`
// reuse — the identical dataflow-timing trap, one level over. Moreover
// (Node.hpp's own header comment on the flag) it is DELIBERATELY never set
// for a no-initializer local in the first place — exactly this issue's own
// repro shape — because that case "CARRIES the §28.3 disposal obligation...
// stays a hard error absent explicit discharge", so the flag would be
// permanently false for it regardless of ordering.
//
// THE FIX: a NEW, self-contained static AST pre-pass,
// `MethodOwnership::collectReassignedOwnedLocals`, walks this method's own
// top-level `body` ONCE — BEFORE `applyUsingVarBorrowPinInLambdas` runs (see
// the call site in `run()`) — recording every LOCAL symbol with a bare
// `sym := E;` reassignment where `producerState(E) == Owned`, into the new
// `reassignedOwnedLocals_` set. Bounded: it recurses into every nested
// CONTROL-FLOW statement list (if/elif/else, while, for, do…end,
// try/catch/finally, case arms — the same coverage `collectBodyLambdas`
// already establishes as this file's completeness bar) but does NOT descend
// into any `LambdaExpr`'s own body (`AssignmentStmt` is never embedded
// inside an expression in this grammar, so simply never walking into a
// lambda body excludes lambda-interior reassignments by construction — no
// separate lambda-detection arm needed). Because it is a pure AST scan using
// `producerState` (the SAME classifier `isOwnedTrackedLocalInEnclosingFrame`
// itself uses), it has NO dataflow-order dependency and is fully populated
// however early it runs.
//
// The new predicate `isReassignedOwnedLocalInEnclosingFrame` is a plain
// membership test against that set, wired into the SAME two lambda-body
// DECLARATION call sites #4286 widened (`handleLocalDecl`'s `fromLambdaBody`
// retry, and the lambda-body `LocalConstDecl` arm) as an ADDITIONAL `||`
// alternative alongside #4286's own `isOwnedTrackedLocalInEnclosingFrame` —
// neither predicate's own logic changes.
//
// SCOPE (LANE_RULES §6, matching #4286's/#4283's own fence): only the two
// DECLARATION sites are widened, exactly as #4286 did. `handleAssignment`
// and the lambda-body reassignment arm (~line 9518 pre-fix) are untouched —
// `fix #4272`'s banked rows (`y := h.item; y.Dispose();` and its lambda-body
// twin) deliberately pin the CURRENT accept-and-runtime-protected
// reassignment-outside-a-lambda behavior; this fix does not reach those
// sites at all, so that fence holds by construction, re-verified below by
// re-running `explicit_dispose_borrow_became_owned_leak_4246` (12/12
// unchanged; corrected at integration -- the file has 12 rows, measured
// with `grep -c '^BEZEL_TEST'`, not 11).
//
// DEFERRED (measured still-open, filed as a follow-up issue): when the
// DECLARATION and REASSIGNMENT of the captured local both happen INSIDE a
// DIFFERENT (outer) lambda's own body — not this method's top-level body —
// and only a lambda nested inside THAT outer lambda aliases+disposes it,
// this fix's scan (bounded to the method's own top-level statement tree)
// does not find the reassignment. Measured: still a live `bz_array_free`
// SIGSEGV under ASan. Left open rather than folded in here — see the filed
// issue for the repro and the reason (the reassignment there lives inside
// the very lambda-body-reassignment machinery #4272 fences off, a
// materially harder change).

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

}  // namespace

// =====================================================================
// REJECT arm — a captured local that becomes OWNED VIA REASSIGNMENT (no
// declaration initializer), all three declaration spellings of the alias.
// "Declared-and-reassigned in the SAME scope" axis: `a`'s declaration,
// reassignment, and the lambda that captures it are all direct siblings in
// `Main`'s own top-level body.
// =====================================================================

BEZEL_TEST(lambda_body_captured_reassigned_owned_alias_dispose_4314,
           plain_var_alias_of_reassigned_owned_local_dispose_rejected_28_3) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a: Array<int>; a := new Array<int>(2); a.Add(1); "
        "  var f := lambda() => do "
        "    var b := a; b.Dispose(); return 0; "
        "  end; "
        "  var r := f(); a.Dispose(); return r; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

BEZEL_TEST(lambda_body_captured_reassigned_owned_alias_dispose_4314,
           using_var_alias_of_reassigned_owned_local_dispose_rejected_28_10) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a: Array<int>; a := new Array<int>(2); a.Add(1); "
        "  var f := lambda() => do "
        "    using var b := a; b.Dispose(); return 0; "
        "  end; "
        "  var r := f(); a.Dispose(); return r; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

BEZEL_TEST(lambda_body_captured_reassigned_owned_alias_dispose_4314,
           const_alias_of_reassigned_owned_local_dispose_rejected_28_3) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a: Array<int>; a := new Array<int>(2); a.Add(1); "
        "  var f := lambda() => do "
        "    const b := a; b.Dispose(); return 0; "
        "  end; "
        "  var r := f(); a.Dispose(); return r; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// =====================================================================
// REJECT arm — a lambda nested TWO levels deep. `a`'s declaration AND its
// reassignment both live at the OUTERMOST method scope; the alias is
// declared inside the INNER lambda, two frames in.
// `isReassignedOwnedLocalInEnclosingFrame` resolves straight to `a`'s own
// symbol via `reassignedOwnedLocals_` regardless of how many lambda frames
// sit between the reassignment and the alias — no per-depth special-casing
// needed (mirrors #4286's own two-levels-deep row).
// =====================================================================

BEZEL_TEST(lambda_body_captured_reassigned_owned_alias_dispose_4314,
           nested_two_levels_deep_alias_of_reassigned_owned_local_rejected_28_3) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a: Array<int>; a := new Array<int>(2); a.Add(1); "
        "  var outer := lambda() => do "
        "    var inner := lambda() => do "
        "      var b := a; b.Dispose(); return 0; "
        "    end; "
        "    return inner(); "
        "  end; "
        "  var r := outer(); a.Dispose(); return r; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// =====================================================================
// REJECT arm — the reassignment itself sits inside a CONDITIONAL (both
// branches reassign to a fresh owned value). `collectReassignedOwnedLocals`
// recurses into `IfStmt` branches exactly as `collectBodyLambdas` does, so
// the conditional nesting does not hide the reassignment from the scan —
// deliberately conservative (the scan does not attempt path-sensitivity: a
// reassignment reachable on ANY path is enough to require rejecting a later
// unmarked alias-dispose, matching this file's family's existing
// over-rejection-over-double-free bias).
// =====================================================================

BEZEL_TEST(lambda_body_captured_reassigned_owned_alias_dispose_4314,
           reassigned_inside_conditional_alias_dispose_rejected_28_3) {
    auto r = runAot(
        "module M; class C public static method Main(cond: bool): int "
        "  var a: Array<int>; "
        "  if cond then a := new Array<int>(2); "
        "  else a := new Array<int>(3); "
        "  end; "
        "  a.Add(1); "
        "  var f := lambda() => do "
        "    var b := a; b.Dispose(); return 0; "
        "  end; "
        "  var r := f(); a.Dispose(); return r; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// =====================================================================
// REJECT arm — "declared-and-reassigned in an OUTER scope" axis: `a`'s
// declaration and reassignment stay at the METHOD's own top-level (outer)
// scope, but the LAMBDA that captures it is nested one block in (inside an
// `if`). `collectReassignedOwnedLocals` walks the WHOLE method body
// regardless of where any given lambda sits, so this is unaffected by the
// lambda's own nesting depth — only the declaring/reassigning scope matters.
// =====================================================================

BEZEL_TEST(lambda_body_captured_reassigned_owned_alias_dispose_4314,
           lambda_nested_in_conditional_captures_outer_scope_reassigned_local_rejected_28_3) {
    auto r = runAot(
        "module M; class C public static method Main(cond: bool): int "
        "  var a: Array<int>; a := new Array<int>(2); a.Add(1); "
        "  if cond then "
        "    var f := lambda() => do "
        "      var b := a; b.Dispose(); return 0; "
        "    end; "
        "    var r := f(); a.Dispose(); return r; "
        "  end; "
        "  a.Dispose(); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// =====================================================================
// ACCEPT-side controls (LANE_RULES §15) — programs that MUST keep compiling
// and running leak/UAF-free; a false-reject would be worse than #4314
// itself. Loop-amplified 200x (LANE_RULES §3/§30): a one-shot leak/UAF row
// can pass on both sides.
// =====================================================================

// A REASSIGNMENT-owned capture that is only READ (no alias declared, no
// `.Dispose()` anywhere in the lambda) must stay accepted.
BEZEL_TEST(lambda_body_captured_reassigned_owned_alias_dispose_4314,
           captured_reassigned_owned_local_read_only_frees_exactly_once_28_9) {
    auto r = runAotChecked(
        "module M; class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    var a: Array<int>; a := new Array<int>(2); a.Add(1); "
        "    var f := lambda() => do return a.Count; end; "
        "    var n := f(); "
        "    a.Dispose(); "
        "    i := i + n - n; i := i + 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A REASSIGNMENT-owned capture aliased inside the lambda (`var b := a;`) but
// the alias is NEVER disposed — merely populating `disposeAliasSource` must
// be inert at runtime, and the captured local's own disposal at the
// enclosing scope must still free exactly once.
BEZEL_TEST(lambda_body_captured_reassigned_owned_alias_dispose_4314,
           captured_reassigned_owned_local_alias_never_disposed_frees_exactly_once_28_3) {
    auto r = runAotChecked(
        "module M; class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    var a: Array<int>; a := new Array<int>(2); a.Add(1); "
        "    var f := lambda() => do var b := a; return b.Count; end; "
        "    var n := f(); "
        "    a.Dispose(); "
        "    i := i + n - n; i := i + 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A lambda that produces and disposes its OWN fresh value (unrelated to any
// captured, reassigned-owned outer local): the initializer is a NewExpr,
// never an IdentifierExpr, so neither #4286's nor this fix's enclosing-frame
// retry ever engages, by construction.
BEZEL_TEST(lambda_body_captured_reassigned_owned_alias_dispose_4314,
           lambda_owns_and_disposes_its_own_value_still_compiles_28_3) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var f := lambda() => do "
        "    var b := new Array<int>(2); b.Add(1); b.Dispose(); return 0; "
        "  end; "
        "  return f(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}
