// Spec-derived regression tests for issue #4005 — a lambda checked against a
// `() -> owned T` function-type slot never verified that its return is an
// OWNED PRODUCTION, so the IDENTICAL #3993 borrow-through-owned-return
// double-free reproduces through a function value instead of a named
// method/operator return.
//
// §28.3 *Borrow lifetime rules*, v0.294 (verbatim, first bullet): "A
// borrowed local may not be returned from a method whose return type is
// `owned`." §28.3 *Concrete-slot function bodies* states the callee-side
// obligation generally: "every return path supplies a fresh or owned value;
// a borrow is rejected" — but (per the issue) that obligation was never
// actually implemented for a function-typed target's `owned`-marked return.
//
// THE BUG (measured on this worktree with #3993's fix — `handleReturn`'s own
// `checkOwnedReturnDirectBorrow` — ALREADY APPLIED, i.e. the middle state of
// a three-way fail-without; #3993's fix touches only named method/operator
// RETURN checking, an entirely different code path from lambda-body
// checking):
//
//   using var b := new Res(4);
//   var mk: () -> owned Res := lambda() => b;
//   var c := mk();
//
//   compiled with rc=0 and zero diagnostics (only the unrelated, expected
//   "`c` is implicitly disposed at scope end" auto-drop note); running the
//   compiled program under AddressSanitizer produced a heap-use-after-free —
//   `b`'s object freed once via `c`'s scope-end auto-drop (`c`'s tracked
//   state is Owned, taken from the DECLARED `owned` slot of `mk`'s function
//   type) and again via `b`'s own `using var` scope-end disposal.
//
// ROOT CAUSE (two layers, both closed by this fix):
//
//   1. The one existing lambda-body check that inspects a target's `owned`
//      marker, `checkOneLambdaTaskFamilyBorrowReturn` (§28.3 SB01-B), reads
//      `effOwned` and explicitly declines it ("An explicit `owned` target
//      return is a different, explicit transfer contract, not this rule's
//      plain-`Task` case") — correct for ITS rule, but nothing else in the
//      lambda-body-checking machinery ever picked up the owned case
//      afterward.
//   2. Even a NEW check reading the same `effOwned` signal would have found
//      it always false for a TYPED-LOCAL lambda: the lambda LITERAL's own
//      inferred function TypeRef (`ctx_.exprType(lam)`) never copies the
//      target's `owned` marker onto `funcReturnOwned` — sema's LambdaExpr
//      arm (ExprTypecheck_Exprs.cpp) sets `lamTy->funcReturn =
//      inferredReturn;` with no accompanying `funcReturnOwned` assignment at
//      all, even though the #1820 hint (`pendingLambdaTargetReturnOwned_`)
//      carrying the true marker IS available at that point (and used for a
//      DIFFERENT check, `checkConcreteSlotLambdaBody`, which explicitly
//      SKIPS the owned case — `if (targetOwnedMarker) return;` — leaving no
//      other consumer of the hint for this shape).
//
// THE FIX: `checkOneLambdaOwnedReturnDirectBorrow` (Ownership.cpp) — the
// owned-target sibling of `checkOneLambdaTaskFamilyBorrowReturn` — driven
// from the same two entry points (`checkLambdaTaskFamilyBorrowReturns` /
// `-Expr`), reusing the exact `ownedArgSourceIsBorrow` classifier
// `checkOwnedReturnDirectBorrow` (#3993) uses, with the same exclusions
// (`move E`, a bare owned-field read, a bare catch-bound identifier). To
// close root-cause (2) for the exact measured shape, a new collector
// (`collectLocalDeclDirectLambdaTargets`, the local-decl-DIRECT sibling of
// the existing `collectReturnDirectLambdas`) supplies the LocalDecl's own
// declared type as `expectedFnType` when the lambda's own inferred type
// doesn't carry the marker; this function ORs the two `funcReturnOwned`
// signals together rather than only falling back when `lamTy` supplied
// nothing, so a correctly-inferred `effRet` from `lamTy` is never
// discarded. A further, structural addition NOT present in
// `checkOwnedReturnDirectBorrow` (which needs no such thing — a named
// method's OWN locals ARE tracked in the real dataflow's `states_`): a bare
// identifier resolving to a CAPTURED local from an ENCLOSING scope (as
// opposed to one this lambda's own body declared) is flagged even though
// `ownedArgSourceIsBorrow` alone cannot see it (a fresh `MethodOwnership`
// has no `states_` to classify a plain local's Owned/Borrowed state at
// all) — distinguished structurally by span containment: a local this
// lambda's own body declared has its LocalDecl positioned INSIDE the
// lambda's own source span; a captured enclosing local's does not.
//
// A broad `lambda`-filtered sweep (852 tests across 5 binaries, 0 failed,
// plus the full 2308-test lit diagnostic track) found no false-reject
// fallout from this addition in the existing corpus.
//
// per §28.3 *Borrow lifetime rules*, *Concrete-slot function bodies*, v0.294

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot / runAotChecked — AOT(+ASan) oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// This fix's diagnostic — distinct wording from #3993's named-method form
// (which this file does not reach at all; a lambda body is a structurally
// different code path).
constexpr const char* kLambdaOwnedBorrowReturn =
    "the lambda is checked against a target function type whose return is "
    "`owned`";

// A destructor-counter Res: ctor ++, dtor --, surfaced through Res.live so an
// ACCEPT test can assert exact-once disposal (0 == no leak, no double-free).
const char* kRes =
    "module M; "
    "class Res "
    "  public static live: int := 0; "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; Res.live := Res.live + 1; end constructor; "
    "  destructor Res.live := Res.live - 1; end destructor; "
    "end Res; ";

}  // namespace

// ===========================================================================
// REJECT — a lambda checked against a `() -> owned T` slot whose body is a
// borrow of a CAPTURED variable from the enclosing scope. Fail-without
// (both #3993 alone and neither fix): accepted with zero diagnostics,
// double-freed at runtime (ASan heap-use-after-free, measured). Pass-with
// (this fix): rejected at compile time with a real source location.
// ===========================================================================

// The issue's exact repro: an EXPRESSION-bodied lambda returning a captured
// `using var` (Owned-at-the-enclosing-level) local bare, checked against a
// typed LocalDecl's `() -> owned Res` slot.
BEZEL_TEST(lambda_owned_return_direct_borrow_4005,
           exprbody_lambda_captured_using_var_rejected_spec_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    var mk: () -> owned Res := lambda() => b; "
        "    var c := mk(); return c.tag; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// The BLOCK-bodied sibling (`=> do ... return b; end`) — exercises
// `scanReturnsIn`'s traversal rather than the direct-exprBody path.
BEZEL_TEST(lambda_owned_return_direct_borrow_4005,
           blockbody_lambda_captured_using_var_rejected_spec_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    var mk: () -> owned Res := lambda() => do return b; end; "
        "    var c := mk(); return c.tag; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// A CAPTURED plain (non-`owned`) PARAMETER of the enclosing method, rather
// than a `using var` local — exercises `ownedArgSourceIsBorrow`'s existing
// ParameterSymbol classification (modifier-based, not span-based), reached
// through this fix's new owned-target driving rather than the new
// capture-span addition.
BEZEL_TEST(lambda_owned_return_direct_borrow_4005,
           captured_plain_parameter_rejected_spec_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Wrap(p: Res): int "
        "    var mk: () -> owned Res := lambda() => p; "
        "    var c := mk(); return c.tag; "
        "  end Wrap; "
        "  public static method Main(): int "
        "    using var b := new Res(4); return Wrap(b); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// ===========================================================================
// ACCEPT — a genuinely owned production through a lambda is NOT
// over-rejected, and disposes the value EXACTLY once under the AOT+ASan
// oracle (Res.live == 0 at exit — no leak, no double-free).
// ===========================================================================

// A fresh `new` production — the ordinary factory shape this rule must not
// disturb.
BEZEL_TEST(lambda_owned_return_direct_borrow_4005,
           fresh_new_accepted_disposes_once_spec_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    do "
        "      var mk: () -> owned Res := lambda() => new Res(9); "
        "      do using var c := mk(); end; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// A local this LAMBDA'S OWN body declared (not a capture) — the
// span-containment distinction this fix's capture check depends on must NOT
// flag the lambda's own fresh production.
BEZEL_TEST(lambda_owned_return_direct_borrow_4005,
           own_internal_local_accepted_disposes_once_spec_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    do "
        "      var mk: () -> owned Res := lambda() => do "
        "        var x := new Res(9); return x; "
        "      end; "
        "      do using var c := mk(); end; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// A relayed OWNED call result — the callee-returns-`owned`-T producer form,
// through a lambda.
BEZEL_TEST(lambda_owned_return_direct_borrow_4005,
           relayed_owned_call_result_accepted_disposes_once_spec_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Make(): owned Res return new Res(9); end Make; "
        "  public static method Main(): int "
        "    do "
        "      var mk: () -> owned Res := lambda() => Make(); "
        "      do using var c := mk(); end; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// A lambda with NO target-type context at all (`var f := lambda() => new
// Res(9);`, no `() -> owned Res` ascription anywhere) must not be flagged by
// THIS check — it only fires when a target function type's return is
// affirmatively marked `owned`. This check's own diagnostic is
// Severity::Error, so `jr.ran` alone (compile+run SUCCEEDED) is conclusive
// proof it did not fire — a `diagnosticMessageContains` scan would be
// VACUOUS here regardless (`runAot` discards captured stderr on a
// successful compile). (An untyped function-value local's OWN call result
// is a SEPARATE, pre-existing gap — its inferred function type carries no
// `owned` marker either, so the call result is itself under-tracked for
// disposal and LEAKS; filed as issue #4027, out of scope here since it is a
// leak in an unrelated position, not a rejection this check could
// introduce — asserted at COMPILE-success only, deliberately not exit-code,
// to stay independent of that adjacent gap.)
BEZEL_TEST(lambda_owned_return_direct_borrow_4005,
           untyped_context_lambda_not_rejected) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda() => new Res(9); "
        "    var c := f(); return c.tag; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(jr.ran);
}

// ===========================================================================
// REVIEW FOLLOW-UPS on this fix's original commit (40053ed) — three real
// gaps in `collectLocalDeclDirectLambdaTargets`'s coverage, each measured
// separately and each fixed here in a follow-up commit (none is a
// regression: before 40053ed nothing checked the owned-target case at all,
// so every one of these shapes double-freed either way).
// ===========================================================================

// fix #4031 (also closes check_localdecl_only_walkers, LANE_RULES §14): a
// `const` local's function-typed initializer is exactly as reachable as a
// `var` local's — §21.1 permits a `const` to hold a runtime-managed value —
// but `ast::LocalConstDecl` is a DIFFERENT AST node from `ast::LocalDecl`,
// and the original collector recognized only the latter. Byte-identical to
// this file's own `exprbody_lambda_captured_using_var_rejected_spec_28_3`
// test above except `var` -> `const`.
BEZEL_TEST(lambda_owned_return_direct_borrow_4005,
           const_local_captured_using_var_rejected_spec_21_1) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    const mk: () -> owned Res := lambda() => b; "
        "    var c := mk(); return c.tag; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// A LEGITIMATE `const` function-typed local (a fresh producer, not a
// borrow) must keep compiling — the accept side of the same collector arm
// (LANE_RULES §15: FAIL-WITHOUT only exercises the reject arms).
BEZEL_TEST(lambda_owned_return_direct_borrow_4005,
           const_local_fresh_new_accepted_disposes_once_spec_21_1) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    do "
        "      const mk: () -> owned Res := lambda() => new Res(9); "
        "      do using var c := mk(); end; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// fix #4033: a NESTED lambda's own body can hold a further local-decl-direct
// target one level down (`var outer := lambda() => do var mk: () -> owned
// Res := lambda() => b; ...; end;`). `collectLocalDeclDirectLambdaTargets`
// only ever walked the ENCLOSING METHOD's own `body`; the fix additionally
// walks every collected lambda's OWN `stmts` (checkLambdaTaskFamilyBorrow
// Returns, Ownership.cpp), reaching a target at ANY nesting depth since the
// collected-lambda list already includes every nesting level.
BEZEL_TEST(lambda_owned_return_direct_borrow_4005,
           nested_lambda_body_local_decl_target_rejected_spec_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    var outer := lambda() => do "
        "      var mk: () -> owned Res := lambda() => b; "
        "      var c := mk(); "
        "      return c.tag; "
        "    end; "
        "    var t := outer(); return t; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// fix #4035: a lambda reached through a `(if ... then ... else ...)`
// conditional-expression LocalDecl initializer (§20/A.7) — the original
// collector required the initializer to itself be exactly a bare
// `LambdaExpr`. `recordLocalDeclDirectLambdaTarget` now recurses through
// `ConditionalExpr::thenValue`/`elseValue` (which right-nests an elif chain
// in `elseValue`), applying the local's declared type to whichever arm(s)
// are themselves directly a lambda. The `else` arm here is the
// borrow-returning one.
BEZEL_TEST(lambda_owned_return_direct_borrow_4005,
           conditional_expr_arm_lambda_rejected_spec_20) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    var flag := false; "
        "    var mk: () -> owned Res := "
        "      (if flag then lambda() => new Res(1) else lambda() => b); "
        "    var c := mk(); return c.tag; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// The ACCEPT twin of the conditional-arm shape: BOTH arms are legitimate
// fresh productions — neither is over-rejected by the new recursion. This
// check's own diagnostic is Severity::Error, so `jr.ran` (plain, non-ASan
// `runAot`) is conclusive proof it did not fire. Deliberately NOT asserted
// via `runAotChecked`/exact-once disposal: a `(if ... then <lambda> else
// <lambda>)` conditional assigned to a typed local leaks 40 B under ASan
// regardless of which arm's Res ends up selected or whether the local is
// ever called — reproduces with NEITHER arm being a borrow (so this check
// never fires) and with NO lambda at all involved in EITHER arm's own
// ownership classification, i.e. a codegen-only defect, pre-existing and
// entirely unrelated to this check (this fix touches no codegen file at
// all); filed as issue #4040, out of scope here.
BEZEL_TEST(lambda_owned_return_direct_borrow_4005,
           conditional_expr_both_arms_fresh_not_rejected_spec_20) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var flag := false; "
        "    var mk: () -> owned Res := "
        "      (if flag then lambda() => new Res(1) else lambda() => new Res(2)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(jr.ran);
}

// ===========================================================================
// NON-REGRESSION — the shared `common::isNonOwningIterFormatEnumerableHead`
// predicate (BuiltinRegistry.hpp/#4004, factored out of a duplicated
// hand-spelling this check and #3993's `checkOwnedReturnDirectBorrow` each
// carried — check_enumerable_grid_spellings, LANE_RULES §14) must still
// exempt a lambda checked against an `owned IEnumerable<T>` target exactly
// as #3993's own analogous method-level test does (`iterable_interface_
// head_stored_view_exclusion_3295_test.cpp`,
// owned_return_type_exempt_for_ienumerable): `IEnumerable<T>` does not
// extend `IDisposable` (stdlib §4.12), so this check's own double-dispose
// reasoning does not apply to it. // per stdlib §4.12, #3419, #4004
BEZEL_TEST(lambda_owned_return_direct_borrow_4005,
           ienumerable_target_exempt_not_rejected_stdlib_4_12) {
    JitResult jr = runAot(
        "module M; "
        "class Cell "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "class C "
        "  public static method Main(): int "
        "    var mk: (IEnumerable<Cell>) -> owned IEnumerable<Cell> := "
        "      lambda(a: IEnumerable<Cell>) => a; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(jr.ran);
}

// ===========================================================================
// fix #4043 — review follow-up on this fix's own span-containment check.
// Span containment answers "is this local declared inside the lambda's own
// body" (eligible to be one of ITS productions), NOT "is it a producer".
// The OLD logic treated "yes" as unconditional proof of a fresh production;
// it is not — a local declared inside the lambda that is itself a bare
// (non-`move`) RE-BINDING of something else is still just a borrow of that
// something else (§28.8: only `move` transfers ownership through a place
// read), regardless of where the re-binding local's OWN declaration sits.
// ===========================================================================

// The issue's exact repro: a captured `using var` re-bound through the
// lambda's OWN local before being returned. Fail-without (this fix's
// PRE-#4043 state, 227b46b): accepted with zero diagnostics, measured
// heap-use-after-free under ASan — `items`'s span lies inside the lambda,
// so the old check stopped there without ever looking at what `items` is
// bound to.
BEZEL_TEST(lambda_owned_return_direct_borrow_4005,
           own_local_alias_of_captured_using_var_rejected_spec_28_8) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    var mk: () -> owned Res := lambda() => do "
        "      var items := b; "
        "      return items; "
        "    end; "
        "    var c := mk(); return c.tag; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// The same shape through a captured PLAIN (non-`using`) local — the capture
// classification does not depend on `using`.
BEZEL_TEST(lambda_owned_return_direct_borrow_4005,
           own_local_alias_of_captured_plain_local_rejected_spec_28_8) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var b := new Res(4); "
        "    var mk: () -> owned Res := lambda() => do "
        "      var items := b; "
        "      return items; "
        "    end; "
        "    var c := mk(); return c.tag; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// NEWLY CAUGHT by the #4043 fix (was ALSO wrongly accepted pre-fix, by the
// same unconditional-span-containment hole, just with no ENCLOSING capture
// involved at all): a lambda's OWN fresh local, bare-re-bound through a
// SECOND own local before being returned. §28.8's rule applies uniformly —
// a bare (non-`move`) read is a borrow regardless of what it names, so
// `items`'s bare read of `x` is a borrow of `x`, not a producer, and `x`
// itself is never discharged (no `move`, no direct return) — the same
// double-free shape one level removed, entirely within the lambda.
BEZEL_TEST(lambda_owned_return_direct_borrow_4005,
           own_local_bare_alias_of_own_fresh_local_rejected_spec_28_8) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var mk: () -> owned Res := lambda() => do "
        "      var x := new Res(4); "
        "      var items := x; "
        "      return items; "
        "    end; "
        "    var c := mk(); return c.tag; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// The ACCEPT twin: the SAME shape, but the re-binding is a `move` (a genuine
// transfer, not a bare read) — must NOT be over-rejected by the new
// producer-resolution. This is the row that matters most: a fix here that
// over-rejects breaks every legitimate `() -> owned T` lambda, and a
// one-shot compile-only (or even single-run exact-once) assertion cannot
// distinguish "correctly accepted" from "accepted and leaking" — a leaked
// object and a disposed one both leave `Res.live` looking plausible on a
// SINGLE iteration only by coincidence of timing. LOOP-AMPLIFY 500x (LANE_
// RULES §3): each iteration constructs a FRESH lambda literal, calls it, and
// disposes the result — a per-iteration leak of even one object would drive
// the final `Res.live` to 500, not 0, so the observable is unambiguous
// either way. Runs under `runAotChecked` (ASan), so a double-free would also
// abort rather than silently under- or over-count.
BEZEL_TEST(lambda_owned_return_direct_borrow_4005,
           own_local_move_alias_of_own_fresh_local_accepted_disposes_once_spec_28_8) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var mk: () -> owned Res := lambda() => do "
        "        var x := new Res(4); "
        "        var items := move x; "
        "        return items; "
        "      end; "
        "      do using var c := mk(); end; "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// A lambda's OWN plain (non-`owned`) parameter re-bound through an own
// local before being returned — the parameter-modifier classification
// (`ownedArgSourceIsBorrow`'s ParameterSymbol branch, unaffected by this
// fix) already flags a DIRECT `return p;`; this pins that a bare alias of
// it is caught too, via the SAME producerState-of-the-initializer path this
// fix adds (producerState has no parameter-specific case either — a bare
// identifier is Borrowed by default regardless of what kind of place it
// names).
BEZEL_TEST(lambda_owned_return_direct_borrow_4005,
           own_local_alias_of_own_plain_parameter_rejected_spec_28_8) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Wrap(p: Res): int "
        "    var mk: () -> owned Res := lambda() => do "
        "      var items := p; "
        "      return items; "
        "    end; "
        "    var c := mk(); return c.tag; "
        "  end Wrap; "
        "  public static method Main(): int "
        "    using var b := new Res(4); return Wrap(b); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}
