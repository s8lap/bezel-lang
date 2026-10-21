// Ownership / codegen — the lambda-body reassignment arm
// (`applyUsingVarBorrowPinInLambdaStmt`'s `AssignmentStmt` case,
// src/ownership/Ownership.cpp) never reset `LocalOwnership::
// explicitDisposeReclaimedSlot` (the sticky per-slot reclaim bit `fix #4246`
// added), unlike the method-scope arm (`handleAssignment`,
// src/ownership/Ownership.cpp:6888), which does. Issue #4270 — the SAME
// sibling-arm omission `fix #4269` repaired for `disposeAliasSource`, one
// field over: when #4246 added its field, only one of the two mirrored arms
// got the reset.
//
// SPEC — settled. §28.3 "Local variables, ownership, and borrows" / §28.6
// "Who may call Dispose()" (v0.298). `explicitDisposeReclaimedSlot`'s own
// field comment (OwnershipState.hpp ~115-123) states the reset is
// "load-bearing: the slot's CONTENT changes at a reassignment, so a stale
// `true` would make a LATER, unrelated dispose on a freshly-reassigned
// genuine borrow also reclaim, double-freeing a value this local never
// owned."
//
// WHAT HAPPENS (matches the issue's own repro exactly):
//   1. `y := new Res(9); y.Dispose();` — first dispose reclaims (frees
//      Res(9)), sets the STICKY bit true.
//   2. `var z := x; y := z;` — `y` is reassigned to `z`, a genuine borrow of
//      `x`. `disposeAliasSource` correctly clears (fix #4269 covers this),
//      so the checker's alias rejection does not fire. But pre-fix, the
//      STICKY bit is not reset by this arm, so it stays stale `true`.
//   3. `y.Dispose();` (second occurrence) — the checker admits this call
//      (no rejection path fires). Codegen's
//      `markExplicitDisposeReclaimIfReassignedBorrow` condition is
//      `state == Owned || explicitDisposeReclaimedSlot` — `state` is
//      correctly `Borrowed`, but the stale `true` fires the OR anyway, so
//      codegen frees the pointer `y` currently holds — `x`'s own object
//      (since `y` now aliases `z`, which aliases `x`).
//   4. `x.Dispose();` then reads/frees the same, already-freed block — the
//      measured heap-use-after-free.
//
// DIRECTION OF TRAVEL: a stale `true` reclaim bit makes codegen free a slot
// on a path that never took ownership — a DOUBLE FREE. This fix clears the
// bit more EAGERLY (on every plain `:=` reassignment inside a lambda body,
// mirroring the method-scope arm exactly) — the safe direction, per the
// field's own comment: a missed reclaim (clearing too eagerly on a case that
// should have stayed reclaim-eligible) can only LEAK, never double-free,
// because `markExplicitDisposeReclaimIfReassignedBorrow` re-derives
// eligibility from `state == Owned` independently at every dispose call —
// the sticky bit only ever ADDS eligibility for the repeated-dispose-on-the-
// SAME-epoch case (§28.2 idempotence), it never SUBTRACTS it. Test C below
// proves the "new balance is exact": the sticky bit still correctly
// recognises a repeated dispose on the SAME epoch (no intervening
// reassignment) inside a lambda body, so the fix's eagerness is scoped
// exactly to reassignment, not to every dispose call.
//
// MEASURED PRE-FIX (this lane, base f8b12ba, scripts/agent_check.sh --probe
// --asan on the issue's exact repro, 1 lambda invocation):
//   ==ERROR: AddressSanitizer: heap-use-after-free ... READ of size 8
//       #0 ... in lam$0 bezel
//   freed by thread T0 here: ... in lam$0 bezel
//   previously allocated by thread T0 here: ... in lam$0 bezel
//   SUMMARY: AddressSanitizer: heap-use-after-free bezel in lam$0
//
// per §28.3, §28.6, v0.298, #4270, #4246, #4269.

#include <string>

#include "support/aot_support.hpp"  // runAot / runAotChecked — the AOT oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kPrelude =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; "
    "end Res; "
    "class Holder "
    "  public item: Res; "
    "  public constructor(r: Res) this.item := r; end constructor; "
    "end Holder; "
    "class C "
    "  public static method Main(): int ";

const char* kEpilogue =
    "  end Main; "
    "end C; ";

std::string wrap(const std::string& body) {
    return std::string(kPrelude) + body + kEpilogue;
}

// Counting prelude — pins EXACT disposal counts, alongside the ASan/LSan
// oracle (an under-fire reads the same destructor-dispatch count as a
// correct run; only the RECLAIM differs, which ASan/LSan observe, not the
// counter).
const char* kCountingPrelude =
    "module M; "
    "class Counter "
    "  public static Dropped: int := 0; "
    "end Counter; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor Counter.Dropped := Counter.Dropped + 1; end destructor; "
    "end Res; "
    "class C "
    "  public static method Main(): int ";

std::string wrapCounting(const std::string& body) {
    return std::string(kCountingPrelude) + body + kEpilogue;
}

}  // namespace

// ============================================================================
// A. THE PRIMARY REGRESSION — the issue's exact reproducer, loop-amplified
// (LANE_RULES §3: a one-shot ASan row can pass on both sides by luck of
// allocator reuse; 200 iterations gives the freed block many chances to be
// reused/poisoned between free and the later use).
// ============================================================================

BEZEL_TEST(lambda_reassign_explicit_dispose_reclaimed_slot_4270,
           issue_repro_lambda_reassign_to_borrow_after_reclaim_loop_28_3) {
    JitResult jr = runAotChecked(wrap(
        "var f := lambda(): int => do "
        "  var x := new Res(5); "
        "  var y := x; "
        "  y := new Res(9); "
        "  y.Dispose(); "
        "  var z := x; "
        "  y := z; "
        "  y.Dispose(); "
        "  x.Dispose(); "
        "  return 0; "
        "end; "
        "var i := 0; "
        "var acc := 0; "
        "while i < 200 do "
        "  acc := acc + f(); "
        "  i := i + 1; "
        "end; "
        "return acc;"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(jr));
    // Pre-fix: the stale sticky bit makes the second y.Dispose() free x's
    // live object; x.Dispose() then use-after-frees it. jr.ran becomes
    // false with an ASan heap-use-after-free report. Post-fix: the reset
    // makes the second y.Dispose() correctly dispatch-only (no reclaim);
    // x.Dispose() is the sole reclaim of x's object. Clean, exit 0.
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// ============================================================================
// B. OWNERSHIP-PASS POSITIVE — the fix is a codegen/dataflow-state gate, not
// a new rejection; the issue's own repro shape must be accepted with zero
// ownership diagnostics.
// ============================================================================

BEZEL_TEST(lambda_reassign_explicit_dispose_reclaimed_slot_4270,
           ownership_check_stays_clean_28_3) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        "var f := lambda(): int => do "
        "  var x := new Res(5); "
        "  var y := x; "
        "  y := new Res(9); "
        "  y.Dispose(); "
        "  var z := x; "
        "  y := z; "
        "  y.Dispose(); "
        "  x.Dispose(); "
        "  return 0; "
        "end; "
        "return f();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ============================================================================
// C. STICKY-BIT PRECISION CONTROL — same-epoch idempotence must survive the
// fix. The sticky bit exists so a REPEATED `.Dispose()` on the SAME
// reclaimed value (no intervening reassignment) is recognised as reclaim-
// eligible too (§28.2 idempotence) — proves the new reset is scoped exactly
// to REASSIGNMENT, not to every dispose call, i.e. the fix does not clear
// the bit more eagerly than the safe direction requires. Counting
// destructor pins the exact dispose-dispatch count: the load-bearing
// invariant is that both destructor dispatches from a double-dispose of the
// SAME epoch count once each (idempotent, no crash), matching the
// method-scope sibling test `explicit_dispose_borrow_became_owned_leak_4246
// .double_dispose_after_reassignment_is_idempotent_not_a_crash_28_2`, moved
// inside a lambda body.
// ============================================================================

BEZEL_TEST(lambda_reassign_explicit_dispose_reclaimed_slot_4270,
           same_epoch_double_dispose_inside_lambda_stays_idempotent_28_2) {
    JitResult jr = runAotChecked(wrapCounting(
        "var f := lambda(): int => do "
        "  var x := new Res(5); "
        "  var y := x; "
        "  y := new Res(1); "
        "  y.Dispose(); "
        "  y.Dispose(); "
        "  x.Dispose(); "
        "  return 0; "
        "end; "
        "return f() + Counter.Dropped;"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(jr));
    BEZEL_EXPECT_TRUE(jr.ran);
    // MEASURED: exit=2 (Res(1) reclaimed once via the first y.Dispose(),
    // Res(5) reclaimed once via x.Dispose(); the second y.Dispose() is a
    // clean no-op — the sticky bit is still sticky WITHIN one epoch).
    BEZEL_EXPECT_EQ(jr.exitCode, 2);
}

// ============================================================================
// D. AXIS — MEMBER-ACCESS REBIND (not a bare identifier). The reset this fix
// adds is unconditional in the target-state-transition block, so it must
// fire regardless of the RHS SHAPE the reassignment reads from — vs test A's
// bare-identifier rebind (`y := z;`), this rebinds through a member access
// (`y := h.item;`), the same axis issue #4272 used to show
// `disposeAliasSource` is NOT set for a member-access RHS at all (a
// different, unrelated gap). This row is unaffected by that gap: the sticky
// bit reset does not depend on `disposeAliasSource`/`aliasSrc` in any way,
// so it must clear on this axis exactly as it does on the bare-identifier
// axis.
// ============================================================================

BEZEL_TEST(lambda_reassign_explicit_dispose_reclaimed_slot_4270,
           axis_member_access_rebind_after_reclaim_no_crash_28_3) {
    JitResult jr = runAotChecked(wrap(
        "var f := lambda(): int => do "
        "  var x := new Res(5); "
        "  var y := x; "
        "  var h := new Holder(x); "
        "  y := new Res(9); "
        "  y.Dispose(); "
        "  y := h.item; "
        "  y.Dispose(); "
        "  x.Dispose(); "
        "  return 0; "
        "end; "
        "var i := 0; "
        "var acc := 0; "
        "while i < 150 do "
        "  acc := acc + f(); "
        "  i := i + 1; "
        "end; "
        "return acc;"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(jr));
    // Pre-fix: same stale-bit hazard as test A, reached via a member-access
    // rebind instead of a bare-identifier one — the reset this fix adds
    // does not branch on RHS shape, so both axes must be fixed together.
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}
