// Spec-derived tests for fix #4321 and fix #4304 — the last two axes of the
// #4236/#4268/#4283/#4286/#4314 "lambda-body alias of a captured owned
// binding double-frees" family. Both close a gap the previous fix in the
// series left EXPLICITLY DEFERRED:
//
// #4321 (sibling of #4314): #4314's `collectReassignedOwnedLocals` closed
// the REASSIGNMENT axis (`var a: Array<int>; a := new Array<int>(2);`) but
// only when the reassignment is reachable from the METHOD's own top-level
// statement tree — its scan deliberately never descends into a
// `LambdaExpr`'s own body (see that function's own header comment,
// Ownership_impl.hpp). When the captured local's DECLARATION and
// REASSIGNMENT both happen inside a DIFFERENT (outer) lambda's own body, and
// only a lambda nested inside THAT outer lambda aliases+disposes it,
// #4314's scan never finds the reassignment:
//
//   var f := lambda() => do
//     var a: Array<int>; a := new Array<int>(2);
//     var inner := lambda() => do var b := a; b.Dispose(); end;
//   end;
//
// compiled clean and double-freed in `bz_array_free`, byte-for-byte the
// family's crash signature (measured live SIGSEGV under ASan, 50/50 runs).
//
// THE FIX: `MethodOwnership::collectReassignedOwnedLocalsInLambdaFrames`
// (Ownership.cpp), called once from `run()` immediately after
// `collectReassignedOwnedLocals(body)`, additionally reuses
// `collectBodyLambdas` — the SAME flattening authority
// `applyUsingVarBorrowPinInLambdas` itself uses — to enumerate every
// `LambdaExpr` reachable from `body` AT EVERY NESTING DEPTH, and scans EACH
// one's own top-level `stmts` with the UNCHANGED `collectReassignedOwnedLocals`,
// feeding the SAME flat `reassignedOwnedLocals_` set
// `isReassignedOwnedLocalInEnclosingFrame` already consults. One flat set
// across every frame is sound (not merely convenient) because
// `reassignedOwnedLocals_` is keyed by `sema::Symbol*` — one per
// DECLARATION — and `isReassignedOwnedLocalInEnclosingFrame` is only ever
// consulted with a symbol SEMA has already resolved via real lexical
// scoping: that symbol's declaring frame is necessarily an ancestor of (or
// equal to) the consulting lambda's frame, so a lambda's own local can only
// ever be looked up from inside that lambda or a lambda nested within it —
// see the full argument at the function's declaration, Ownership_impl.hpp.
// Like #4314's own pre-pass, this is a pure static AST scan with no
// dataflow-order dependency, so it needs no reordering relative to
// `applyUsingVarBorrowPinInLambdas` beyond running alongside its sibling.
//
// #4304 (sibling of #4286): #4286's `isOwnedTrackedLocalInEnclosingFrame`
// closed the lambda-body capture gap for an owned LOCAL's declaration
// initializer but is restricted to a `LocalSymbol` root — an `owned`
// PARAMETER has no `LocalDecl`/initializer to classify via `producerState`
// in the first place, so the retry answers false outright for:
//
//   public static method Run(owned p: Array<int>): int
//     var f := lambda() => do var b := p; b.Dispose(); end;
//
// which compiled clean (only the pre-existing §28.3 auto-drop WARNING on
// `p`, not a §28.6 rejection) and double-freed identically (measured live
// SIGSEGV under ASan, 50/50 runs).
//
// THE FIX: a new predicate, `MethodOwnership::isOwnedParameterInEnclosingFrame`
// (Ownership.cpp), restricted to a `ParameterSymbol` root, answers via
// `symbolIsOwnedObligation` (Ownership_Checks.cpp; #3746's "is this symbol a
// binding whose owned value THIS frame disposes?") — but ONLY that
// predicate's `ParameterSymbol` arm (`ps->modifier == "owned"`), never
// falling through to its `LocalSymbol` arm. That restriction matters:
// `symbolIsOwnedObligation`'s `LocalSymbol` arm bottoms out in
// `localIsOwnedObligation`, the exact dataflow-timing trap #4286 already
// measured and rejected (populated only at the very END of `run()`'s main
// dataflow, well after this lambda-body pre-pass runs) — calling it
// unrestricted would silently reintroduce that trap for the LOCAL axis
// while fixing only the PARAMETER one. The `ParameterSymbol` arm itself has
// no such dependency: `ps->modifier` is a plain AST/symbol-table fact set
// during parsing/sema, available unconditionally before `run()` even
// begins — so restricting to that one arm carries none of the timing risk.
// `out owned` is excluded by that same arm (§28.3/§22.3: an `out owned`
// value is disposed by the CALLER's frame, not this one).
//
// Both new predicates are wired as an ADDITIONAL `||` alternative at the
// SAME two lambda-body DECLARATION call sites #4286/#4314 already widened
// (`handleLocalDecl`'s `fromLambdaBody` retry; the lambda-body
// `LocalConstDecl` arm) — none of #4286's, #4314's, or each other's own
// predicate logic changes.
//
// SCOPE (LANE_RULES §6, matching the whole family's own fence): only the
// two DECLARATION sites are widened. `handleAssignment` and the lambda-body
// reassignment arm (~line 9518) are UNTOUCHED — `fix #4272`'s banked rows
// deliberately pin the current accept-and-runtime-protected
// reassignment-OUTSIDE-a-lambda behavior; re-verified below
// (`explicit_dispose_borrow_became_owned_leak_4246`: 12/12 unchanged).
//
// EMPIRICAL ORDERING PROOF: built bezelc with the fix and ran it on both
// issues' own repros — the compiler now REJECTS both with a real §28.6
// diagnostic at the alias's `.Dispose()` call site, which is only possible
// if `reassignedOwnedLocals_` (#4321) and the parameter's `owned` modifier
// (#4304) were observed correctly at the point `handleLocalDecl`'s
// `fromLambdaBody` retry consulted them.

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
// #4321 REJECT arm — LOCAL declared AND reassigned inside a DIFFERENT
// (outer) lambda's own body; only a lambda NESTED inside that outer lambda
// aliases+disposes it. All three alias-declaration spellings, at the
// outer/inner (2-level) nesting depth the issue's own repro uses.
// =====================================================================

BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4321_4304,
           plain_var_alias_of_local_reassigned_in_outer_lambda_dispose_rejected_28_3) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var f := lambda() => do "
        "    var a: Array<int>; a := new Array<int>(2); a.Add(1); "
        "    var inner := lambda() => do "
        "      var b := a; b.Dispose(); return 0; "
        "    end; "
        "    var r := inner(); a.Dispose(); return r; "
        "  end; "
        "  return f(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4321_4304,
           using_var_alias_of_local_reassigned_in_outer_lambda_dispose_rejected_28_10) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var f := lambda() => do "
        "    var a: Array<int>; a := new Array<int>(2); a.Add(1); "
        "    var inner := lambda() => do "
        "      using var b := a; b.Dispose(); return 0; "
        "    end; "
        "    var r := inner(); a.Dispose(); return r; "
        "  end; "
        "  return f(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4321_4304,
           const_alias_of_local_reassigned_in_outer_lambda_dispose_rejected_28_3) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var f := lambda() => do "
        "    var a: Array<int>; a := new Array<int>(2); a.Add(1); "
        "    var inner := lambda() => do "
        "      const b := a; b.Dispose(); return 0; "
        "    end; "
        "    var r := inner(); a.Dispose(); return r; "
        "  end; "
        "  return f(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// =====================================================================
// #4321 REJECT arm — THREE levels of lambda nesting: `a`'s declaration and
// reassignment live in the OUTERMOST lambda `f`'s own body; the alias is
// declared in a lambda `inner` nested TWO frames deeper (`f` -> `mid` ->
// `inner`). Proves `collectReassignedOwnedLocalsInLambdaFrames`'s
// `collectBodyLambdas`-based flattening reaches an unbounded nesting depth,
// not just one frame over the declaring lambda.
// =====================================================================

BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4321_4304,
           three_levels_deep_alias_of_local_reassigned_in_outer_lambda_rejected_28_3) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var f := lambda() => do "
        "    var a: Array<int>; a := new Array<int>(2); a.Add(1); "
        "    var mid := lambda() => do "
        "      var inner := lambda() => do "
        "        var b := a; b.Dispose(); return 0; "
        "      end; "
        "      return inner(); "
        "    end; "
        "    var r := mid(); a.Dispose(); return r; "
        "  end; "
        "  return f(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// =====================================================================
// #4304 REJECT arm — a captured `owned` PARAMETER (not a local). All three
// alias-declaration spellings, direct (1-level) capture — the issue's own
// repro depth.
// =====================================================================

BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4321_4304,
           plain_var_alias_of_owned_parameter_dispose_rejected_28_3) {
    auto r = runAot(
        "module M; class C "
        "  public static method Run(owned p: Array<int>): int "
        "    p.Add(1); "
        "    var f := lambda() => do "
        "      var b := p; b.Dispose(); return 0; "
        "    end; "
        "    return f(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var a := new Array<int>(2); "
        "    return Run(move a); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `p`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4321_4304,
           using_var_alias_of_owned_parameter_dispose_rejected_28_10) {
    auto r = runAot(
        "module M; class C "
        "  public static method Run(owned p: Array<int>): int "
        "    p.Add(1); "
        "    var f := lambda() => do "
        "      using var b := p; b.Dispose(); return 0; "
        "    end; "
        "    return f(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var a := new Array<int>(2); "
        "    return Run(move a); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `p`"));
}

BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4321_4304,
           const_alias_of_owned_parameter_dispose_rejected_28_3) {
    auto r = runAot(
        "module M; class C "
        "  public static method Run(owned p: Array<int>): int "
        "    p.Add(1); "
        "    var f := lambda() => do "
        "      const b := p; b.Dispose(); return 0; "
        "    end; "
        "    return f(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var a := new Array<int>(2); "
        "    return Run(move a); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `p`"));
}

// =====================================================================
// #4304 REJECT arm — the owned parameter captured through TWO, then THREE
// levels of lambda nesting. `isOwnedParameterInEnclosingFrame` answers from
// a pure symbol-identity fact (`ps->modifier`), independent of nesting
// depth — this pins that the pre-existing #2166 lambda-body-recursion
// machinery still reaches the retry correctly at depth, not a new
// mechanism of this fix's own.
// =====================================================================

BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4321_4304,
           nested_two_levels_deep_alias_of_owned_parameter_rejected_28_3) {
    auto r = runAot(
        "module M; class C "
        "  public static method Run(owned p: Array<int>): int "
        "    p.Add(1); "
        "    var outer := lambda() => do "
        "      var inner := lambda() => do "
        "        var b := p; b.Dispose(); return 0; "
        "      end; "
        "      return inner(); "
        "    end; "
        "    return outer(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var a := new Array<int>(2); "
        "    return Run(move a); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `p`"));
}

BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4321_4304,
           nested_three_levels_deep_alias_of_owned_parameter_rejected_28_3) {
    auto r = runAot(
        "module M; class C "
        "  public static method Run(owned p: Array<int>): int "
        "    p.Add(1); "
        "    var outer := lambda() => do "
        "      var mid := lambda() => do "
        "        var inner := lambda() => do "
        "          var b := p; b.Dispose(); return 0; "
        "        end; "
        "        return inner(); "
        "      end; "
        "      return mid(); "
        "    end; "
        "    return outer(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var a := new Array<int>(2); "
        "    return Run(move a); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `p`"));
}

// =====================================================================
// ACCEPT-side controls (LANE_RULES §15) — programs that MUST keep compiling
// and running leak/UAF-free; a false-reject would be worse than either bug
// itself. Loop-amplified 200x (LANE_RULES §3/§30): a one-shot leak/UAF row
// can pass on both sides.
// =====================================================================

// #4321 axis: a local declared+reassigned in an OUTER lambda, read-only
// (no alias, no `.Dispose()`) from a NESTED lambda — must stay accepted.
BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4321_4304,
           local_reassigned_in_outer_lambda_read_only_from_inner_frees_exactly_once_28_9) {
    auto r = runAotChecked(
        "module M; class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    var f := lambda() => do "
        "      var a: Array<int>; a := new Array<int>(2); a.Add(1); "
        "      var inner := lambda() => do return a.Count; end; "
        "      var n := inner(); a.Dispose(); return n; "
        "    end; "
        "    var n := f(); "
        "    i := i + n - n; i := i + 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// #4321 axis: aliased inside the nested lambda but NEVER disposed — merely
// populating `disposeAliasSource` must be inert at runtime.
BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4321_4304,
           local_reassigned_in_outer_lambda_alias_never_disposed_frees_exactly_once_28_3) {
    auto r = runAotChecked(
        "module M; class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    var f := lambda() => do "
        "      var a: Array<int>; a := new Array<int>(2); a.Add(1); "
        "      var inner := lambda() => do var b := a; return b.Count; end; "
        "      var n := inner(); a.Dispose(); return n; "
        "    end; "
        "    var n := f(); "
        "    i := i + n - n; i := i + 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// #4304 axis: an `owned` parameter captured, read-only (no alias) — must
// stay accepted, with the parameter's own explicit disposal at method scope
// unaffected by the lambda's read.
BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4321_4304,
           owned_parameter_captured_read_only_frees_exactly_once_28_9) {
    auto r = runAotChecked(
        "module M; class C "
        "  public static method Run(owned p: Array<int>): int "
        "    p.Add(1); "
        "    var f := lambda() => do return p.Count; end; "
        "    var n := f(); "
        "    p.Dispose(); "
        "    return n; "
        "  end Run; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 200 do "
        "      var a := new Array<int>(2); "
        "      var n := Run(move a); "
        "      i := i + n - n; i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// #4304 axis: aliased inside the lambda (`var b := p;`) but NEVER disposed —
// the parameter's own explicit disposal at method scope must still free
// exactly once.
BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4321_4304,
           owned_parameter_captured_alias_never_disposed_frees_exactly_once_28_3) {
    auto r = runAotChecked(
        "module M; class C "
        "  public static method Run(owned p: Array<int>): int "
        "    p.Add(1); "
        "    var f := lambda() => do var b := p; return b.Count; end; "
        "    var n := f(); "
        "    p.Dispose(); "
        "    return n; "
        "  end Run; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 200 do "
        "      var a := new Array<int>(2); "
        "      var n := Run(move a); "
        "      i := i + n - n; i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A lambda that produces and disposes its OWN fresh value (unrelated to
// either axis): the initializer is a NewExpr, never an IdentifierExpr, so
// neither new predicate's enclosing-frame retry ever engages, by
// construction.
BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4321_4304,
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

// #4304's OWN axis control: a BORROWED (unmarked, non-`owned`) parameter
// captured by a lambda, aliased and read-only (never disposed). Proves
// `isOwnedParameterInEnclosingFrame`'s `ps->modifier == "owned"` gate
// correctly answers false for an unmarked parameter — the new predicate
// must not misclassify a plain borrow as an owned capture.
BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4321_4304,
           borrowed_unmarked_parameter_captured_read_only_frees_exactly_once_28_9) {
    auto r = runAotChecked(
        "module M; class C "
        "  public static method Peek(p: Array<int>): int "
        "    var f := lambda() => do var b := p; return b.Count; end; "
        "    return f(); "
        "  end Peek; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 200 do "
        "      var a := new Array<int>(2); a.Add(1); "
        "      var n := Peek(a); "
        "      a.Dispose(); "
        "      i := i + n - n; i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}
