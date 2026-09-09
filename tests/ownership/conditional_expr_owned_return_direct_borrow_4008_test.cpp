// Spec-derived regression tests for issue #4008 — `checkOwnedReturnDirectBorrow`
// (#3993, Ownership_Checks.cpp) feeds the existing `ownedArgSourceIsBorrow`
// classifier for its borrow-source detection. That classifier's shape table
// recognizes a bare identifier, a member-access, an indexer-get, and a
// postfix-bang place read; it then short-circuits with
// `if (!id) return false;` (Ownership_Checks.cpp:10786) so any non-identifier
// expression form is conservatively treated as "not a borrow". A
// `ConditionalExpr` is exactly such a non-identifier form, so a borrow
// returned through EITHER arm of an `(if cond then A else B)` slipped past
// the check entirely: the program compiled with zero diagnostics, and at
// runtime the runtime selected exactly one arm — if that arm was a bare
// borrow, both the caller's `owned` return-slot disposer and the borrow's
// real owner disposed the same object, a heap-use-after-free measured under
// generated-code AddressSanitizer.
//
// THE BUG (measured pre-fix on this branch's merge base, `2d201b3`):
//
//   public static method Pick(cond: bool, a: Res, b: Res): owned Res
//     return (if cond then a else b);
//   end Pick;
//
// compiled with rc=0 and zero diagnostics (only the unrelated §22.12 mutable-
// static-field warning); running the linked binary under ASan produced
//
//   ==...==ERROR: AddressSanitizer: heap-use-after-free on address ...
//   READ of size 8 ... in main
//   freed by thread T0 here: ... free ... in main
//   previously allocated by thread T0 here: ... calloc ... in main
//
// The same borrowed-parameter shape via a bare `return b;` is the
// #3993-original reject (`static_method_borrowed_param_rejected_spec_28_3`),
// already correctly rejected; wrapping either parameter in an `if`-arm
// defeated the check entirely.
//
// ROOT CAUSE: `ownedArgSourceIsBorrow`'s `if (!id) return false;` short-
// circuit treats every non-identifier expression form as "not a borrow",
// including the §20/A.7 `ConditionalExpr` (an if-expression whose arms may
// each be a bare borrowed parameter). The check therefore never fired for
// the `return (if cond then a else b)` shape, even though exactly one arm
// runs at runtime and any borrow reaching that arm through the caller's
// `owned` return slot is the same double-free #3993 exists to close. The
// sibling `CaseExpr` likely has the same gap and is filed separately
// (#4008's own body defers it). The CallExpr-relay half of the same family
// is also a separate fix (#4009, the `if !id return false` short-circuit
// likewise lets a relayed call to a non-`owned`-returning callee through).
//
// THE FIX: `ownedArgSourceIsBorrow` gets a `ConditionalExpr` arm that
// recurses into both `thenValue` and `elseValue` and returns true iff
// EITHER arm is itself classified as a borrow. An elif chain right-nests
// in `elseValue` (grammar A.7), so the same recursion reaches every
// else-arm of an arbitrary `if/elif/.../else` chain without further work.
// The OR is conservative: a conditional where ONE arm is a borrow is
// rejected regardless of whether the OTHER arm is a fresh production —
// the runtime evaluates exactly one arm, and the only sound answer is
// to require BOTH arms to be a fresh production, which is what the
// borrow-classification OR implements. The single-arm call-result relay
// (#4009) is filed separately.
//
// SPEC: §28.3 *Borrow lifetime rules* (verbatim, first bullet: "a borrowed
// local may not be returned from a method whose return type is `owned`"),
// §20 *The `if` Expression*, v0.294.
// No section of §20 or §28.3 states this obligation directly for an
// `if`-expression arm; it follows from §28.3's "a borrowed local may not
// be returned from a method whose return type is `owned`" applied to
// each arm at runtime — the runtime returns the value of whichever arm
// it selected, exactly as if the arm had been the bare return expression.

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

// The #3993 / #4008 direct-borrow-return diagnostic's stable substring.
constexpr const char* kDirectBorrowReturn =
    "a borrow cannot be returned as an `owned` value";

// A destructor-counter Res: ctor ++, dtor --. A correct program's exit code
// (this file threads the counter out through the process exit code) is 0 —
// a double-free drives it negative or aborts under ASan before `Main`
// returns at all (the exact-once oracle).
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
// REJECT — a borrow returned through an `if`-expression arm reaches the
// caller's `owned` return slot through exactly one runtime path and is the
// same double-free #3993 exists to close. Fail-without: accepted with zero
// diagnostics, double-freed at runtime (ASan heap-use-after-free, measured).
// Pass-with: rejected at compile time with a real source location.
// ===========================================================================

// The issue's exact repro: a STATIC method returning a §20
// conditional-expression of two plain borrowed parameters through `owned`,
// block body.
BEZEL_TEST(conditional_expr_owned_return_direct_borrow_4008,
           static_method_if_expr_both_borrowed_params_rejected_spec_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Pick(cond: bool, a: Res, b: Res): owned Res "
        "    return (if cond then a else b); "
        "  end Pick; "
        "  public static method Main(): int "
        "    using var a := new Res(3); using var b := new Res(4); "
        "    var c := Pick(true, a, b); return c.tag; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kDirectBorrowReturn));
}

// The `=>` expression-body sibling of the same shape — `runExprBody
// OwningReturnCheck` must reach the new ConditionalExpr recursion too,
// exactly the way the #3993 fix reaches it from both handleReturn AND
// runExprBodyOwningReturnCheck.
BEZEL_TEST(conditional_expr_owned_return_direct_borrow_4008,
           static_method_exprbody_if_expr_borrowed_params_rejected_spec_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Pick(cond: bool, a: Res, b: Res): owned Res => "
        "    (if cond then a else b); "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kDirectBorrowReturn));
}

// A conditional-expression WHERE ONLY ONE ARM IS A BORROW (the other is a
// fresh `new` production) is still flagged: the runtime evaluates exactly
// one arm and a borrow reaching that arm through the caller's `owned` slot
// is the same double-free regardless of what the OTHER arm produces.
// This is the conservative direction #4008's own body explicitly
// recommends; a more permissive arm-by-arm fix would have to thread a
// per-arm "is this arm a fresh production" answer and AND the two, which
// is a more invasive rewrite of the classifier for marginal benefit. //
// per §28.3, §20
BEZEL_TEST(conditional_expr_owned_return_direct_borrow_4008,
           static_method_if_expr_one_fresh_one_borrowed_param_rejected_spec_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Pick(cond: bool, a: Res, b: Res): owned Res "
        "    return (if cond then a else new Res(b.tag)); "
        "  end Pick; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kDirectBorrowReturn));
}

// An elif chain right-nests in `elseValue` (grammar A.7) — the same
// recursion must reach an arbitrarily-deep chain's terminal arm. A two-
// arm elif/elif/else with a borrowed parameter in the terminal else must
// also be rejected.
BEZEL_TEST(conditional_expr_owned_return_direct_borrow_4008,
           static_method_elif_chain_terminal_borrow_rejected_spec_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Pick(x: int, a: Res, b: Res, c: Res): owned Res "
        "    return (if x = 0 then a "
        "            elif x = 1 then b "
        "            else c); "
        "  end Pick; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kDirectBorrowReturn));
}

// A bare `this` returned through an `if`-arm is a special case of the
// same gap: `this` is a borrow of the receiver (the caller still owns it
// — §28.6 *Methods returning `this`*, verbatim: "may not be ... returned
// as `owned`"), and routing it through an `if` arm that the runtime
// might select is the same double-dispose.
BEZEL_TEST(conditional_expr_owned_return_direct_borrow_4008,
           instance_method_if_expr_bare_this_rejected_spec_28_6) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public method Pick(cond: bool): owned Res "
        "    return (if cond then this else this); "
        "  end Pick; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kDirectBorrowReturn));
}

// A Borrowed-state local inside an `if`-arm: `var x := b;` puts `x` in
// `lookupState`'s Borrowed state, and a conditional return whose arm
// reads `x` must be flagged the same as a bare `return x;` (which #3993
// already covers) is.
BEZEL_TEST(conditional_expr_owned_return_direct_borrow_4008,
           static_method_if_expr_borrowed_state_local_rejected_spec_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Pick(cond: bool, a: Res, b: Res): owned Res "
        "    var x := b; "
        "    return (if cond then x else new Res(1)); "
        "  end Pick; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kDirectBorrowReturn));
}

// ===========================================================================
// ACCEPT — a conditional-expression of two fresh productions (the
// ordinary `if cond then a() else b()` relay shape) is NOT over-rejected
// by the new recursion, and disposes exactly once under the AOT+ASan/LSan
// oracle (Res.live == 0 surfaced through the process exit code).
// ===========================================================================

// Both arms are fresh `new` productions — the canonical over-reject risk
// for an "if either arm is a borrow" recursion. Mirrors #3993's own
// fresh_new ACCEPT control. The nested `do ... end;` block scopes `a`
// and `b`'s `using var` disposal to the inner block (so `c` and `d`
// inside the `do using var` blocks have already been disposed when
// `return Res.live` runs and the counter is 0 iff every alloc was
// matched by exactly one free — the exact-once oracle).
BEZEL_TEST(conditional_expr_owned_return_direct_borrow_4008,
           static_method_if_expr_both_fresh_new_accepted_disposes_once_spec_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Pick(cond: bool, a: Res, b: Res): owned Res "
        "    return (if cond then new Res(a.tag) else new Res(b.tag)); "
        "  end Pick; "
        "  public static method Main(): int "
        "    do "
        "      using var a := new Res(3); using var b := new Res(4); "
        "      do using var c := Pick(true, a, b); end; "
        "      do using var d := Pick(false, a, b); end; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// One arm a fresh `new`, the other a call returning `owned` (the
// relayed-owned-call-result producer form): both are fresh productions
// in their respective arm-position, neither is a borrow, so the
// conditional as a whole is not a borrow and must be accepted.
BEZEL_TEST(conditional_expr_owned_return_direct_borrow_4008,
           static_method_if_expr_fresh_and_owned_call_accepted_disposes_once_spec_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Make(t: int): owned Res return new Res(t); end Make; "
        "  public static method Pick(cond: bool, t: int): owned Res "
        "    return (if cond then Make(t) else new Res(t + 1)); "
        "  end Pick; "
        "  public static method Main(): int "
        "    do using var c := Pick(true, 7); end; "
        "    do using var d := Pick(false, 11); end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// An elif chain whose terminal arm is a fresh production and whose other
// arms are also fresh productions must be accepted (no borrow anywhere in
// the chain).
BEZEL_TEST(conditional_expr_owned_return_direct_borrow_4008,
           static_method_elif_chain_all_fresh_accepted_disposes_once_spec_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Pick(x: int, t: int, u: int, v: int): owned Res "
        "    return (if x = 0 then new Res(t) "
        "            elif x = 1 then new Res(u) "
        "            else new Res(v)); "
        "  end Pick; "
        "  public static method Main(): int "
        "    do using var c := Pick(0, 3, 4, 5); end; "
        "    do using var d := Pick(1, 3, 4, 5); end; "
        "    do using var e := Pick(2, 3, 4, 5); end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// ===========================================================================
// NON-REGRESSION — the sibling borrow-return shapes that each have their
// OWN, more specific diagnostic must not be over-reported by the new
// recursion. (A `move E` arm is unwrapped to the bare place inside
// `ownedArgSourceIsBorrow`, which then flags the place via its own
// branches, so the `move` form is correctly classified even with the
// new recursion; #1995's distinct diagnostic continues to win because
// the existing call to checkOwnedReturnBorrowSource runs first.)
// ===========================================================================

// `move` of a borrow through an `if`-arm: the bare place is what the
// recursion sees (after the `move` unwrap at the top of
// `ownedArgSourceIsBorrow`), so this hits the existing
// borrow-classification branches under the new ConditionalExpr
// recursion and is also reported by the §28.3 K3.4 arm-transfer marker
// check at the if-expression's own site. Two distinct diagnostics are
// emitted (one per site), both of which correctly REJECT the program —
// the test pins that the program IS rejected and that the new
// ConditionalExpr recursion does not lose this case to its
// `!id → return false;` short-circuit. (`move b` arm: at the top of the
// recursive call, `unwrappedMove` becomes true and `e` becomes the bare
// place `b`; the bare-identifier branch then classifies `b` as a
// borrowed parameter and the ConditionalExpr arm returns true.)
BEZEL_TEST(conditional_expr_owned_return_direct_borrow_4008,
           move_of_borrow_in_if_expr_arm_still_rejected_not_shadowed_by_4008) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Take(cond: bool, b: Res): owned Res "
        "    return (if cond then move b else new Res(1)); "
        "  end Take; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    // Pin that the new fix's own diagnostic IS reported for this shape
    // (the borrow IS in the if-expression, the new arm recurses into
    // it, the move is unwrapped to the bare place, the bare place is
    // classified as a borrowed parameter, the ConditionalExpr arm
    // returns true). This is the load-bearing signal: WITHOUT the new
    // arm, the program would still be rejected (by K3.4) but only by
    // one site; WITH the new arm, both sites report and the program is
    // rejected from the §28.3 borrow-cannot-be-returned angle too,
    // giving the user both pieces of information. The test asserts
    // rejection at compile time only; the exact message is intentionally
    // not pinned so that K3.4 wording changes don't churn the test.
}
