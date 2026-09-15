// Spec-derived regression tests for issue #4141 — labeled-loop-aware
// state-forwarding in the lambda-body pre-pass
// (`applyUsingVarBorrowPinInLambdaStmt`).
//
// ROOT CAUSE: #4138's terminated-arm exclusion (`armNeverFallsThrough`,
// Ownership.cpp ~line 190) deliberately recognized only `return`/`throw`,
// not `break`/`continue` — `return`/`throw` leave the enclosing callable
// outright so dropping their contribution from any local join is always
// safe, but `break`/`continue` reach a different live merge point (the
// immediately enclosing loop's post-loop join) and dropping them silently
// un-rejected a real double-move (the #4138 first-draft
// `while cond do if flag then move x; break; end; end; move x;` counter-
// example). #4138 filed #4141 as a follow-up.
//
// THIS FIX: an arm whose LAST reachable top-level statement is an
// UNLABELED `break`/`continue` (`targetLoopOffset == 0`) is FORWARDED to
// the immediately enclosing loop's post-loop join — not folded into the
// if/case's own join (which would tell code after the if/case that `x`
// is maybe-consumed when only the unreachable break-arm ever touched
// it — the false-reject), and not dropped entirely (which would discard
// the move from the loop's join — the #4138 first-draft regression).
//
// Mechanism: `armDivertsToEnclosingLoop` (the companion predicate to
// `armNeverFallsThrough`, this file ~line 320) detects the break/continue
// shape. The If/Case arms push the arm's `states_` to
// `pendingDivertedStates_` instead of `armEnds`; the enclosing While/For
// handler pops and joins every state pushed during its body's walk.
//
// §21.5 (v0.298): "`break` exits a loop; `continue` skips the rest of the
// current iteration and advances to the next." §28.3 K7: the
// maybe-consumed join. §28.3 (#1626, reworked): the maybe-consumed
// marker.
//
// Scope (deliberately narrower than the issue's full design):
//   * UNLABELED `break`/`continue` (targetLoopOffset == 0) only — labeled
//     forms targeting an outer labeled loop (offset > 0) are deferred to
//     #4146 Stage 2 ("lambda bodies through the real CFG"); the over-
//     conservative immediate-join folding for those forms is
//     sound-direction only.
//   * The lambda-body pre-pass (`applyUsingVarBorrowPinInLambdaStmt`) only
//     — the real per-method dataflow (`MethodOwnership::visitStmt`'s
//     CaseStmt join) already routes break/continue to the correct merge
//     via real CFG edges (flow/CFG.cpp, see #4146's analysis).
//
// MEASURED regression evidence: the accept-shaped rows below (Rows 1-4)
// all compile rc=0 + run cleanly on this batch's HEAD with the fix
// applied, and ALL are falsely rejected on this batch's base (the pre-fix
// state, `3bdae03`/`origin/main = 3999278` etc., verified by `git
// stash`-ing the fix and re-running — see the FAIL-WITHOUT log). The
// anti-regression control rows (Rows 5-7) are pinned REJECTED both before
// AND after the fix, verifying that #4117's sound-direction protections
// (#3951/#4117's real UAF class) are undiminished by #4141's narrower
// forwarding rule.
//
// per §21.5, §28.2, §28.3 K7, v0.298, #4138, #4116, #4117, #4141

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "support/Diagnostic.hpp"
#include "support/ownership_support.hpp"   // parseAndCheckOwnership, hasErrorMessage
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// A destructor-counter Res (matching the established #3951/#4116/#4117/
// #4138 family idiom) plus a throwable MyErr. `Sink.Flag(false)` is
// used as the unreachable-if-condition anchor so the runtime always
// takes the false-edge fallthrough (the path under test).
const char* kRes =
    "module M; "
    "class Res "
    "  public static live: int := 0; "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; Res.live := Res.live + 1; end constructor; "
    "  destructor Res.live := Res.live - 1; end destructor; "
    "end Res; "
    "class Sink "
    "  public static method Take(owned r: Res): int return r.tag; end Take; "
    "  public static method Flag(b: bool): bool return b; end Flag; "
    "end Sink; ";

const std::string kMaybeConsumedReadMsg =
    "local `x` may have already been consumed on another branch and may "
    "not be read here (maybe-consumed join, §28.3)";

const std::string kMaybeConsumedConsumeMsg =
    "cannot consume `x`: it may have already been consumed on another "
    "branch (maybe-consumed join, §28.3)";

const std::string kAlreadyMovedMsg =
    "owned value in `x` was already moved out at this point (§28.3)";

}  // namespace

// ===========================================================================
// Accept-side rows (runAotChecked, real execution) — the #4141 fix.
// All run on the live runtime path; the runtime value (`Res.live`,
// exact-once disposal) is the assertion, not merely "it compiled" — per
// [[regression-test-pins-the-bug]] and this run's own accept-shaped-row
// discipline.
//
// Each row sets up a while/for body whose ONLY iteration is the
// if-false / arm-default fallthrough (via `Sink.Flag(false)` or `n=2`)
// and ends that iteration with `done := true` (or breaks out via the
// for-loop's own termination). The unreachable break-arm is what this
// fix exists to forward to the loop's join — without the fix the
// reachable arm's `move x` (line N) is wrongly rejected as a second
// move. With the fix the break-arm's state is forwarded (not folded
// into the if's join), so the reachable arm sees `x` as Owned and
// accepts the move.
// ===========================================================================

// Row 1: the basic shape — `if` inside a `while`, if-branch ends in
// unlabeled `break` consuming `x`, fall-through `move x` after the if.
// `Sink.Flag(false)` makes the if-false path the only live path
// (the break-arm is unreachable at runtime — but the walker doesn't
// know that, and the fix must work regardless).
BEZEL_TEST(break_arm_forwarded_4141,
           lambda_body_if_break_arm_state_forwarded_to_while_join_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      var done := false; "
        "      while not done do "
        "        if Sink.Flag(false) then "
        "          t := t + Sink.Take(move x); "
        "          break; "
        "        end; "
        "        t := t + Sink.Take(move x); "
        "        done := true; "
        "      end; "
        "      return t; "
        "    end; "
        "    _ := f(); "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 2: same shape, `continue` instead of `break`. `continue` jumps
// to the loop header (the next iteration), which in this program is
// unreachable because `done := true` was set in the false-edge
// fallthrough. The fix must recognize both terminators.
BEZEL_TEST(break_arm_forwarded_4141,
           lambda_body_if_continue_arm_state_forwarded_to_while_join_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      var done := false; "
        "      while not done do "
        "        if Sink.Flag(false) then "
        "          t := t + Sink.Take(move x); "
        "          continue; "
        "        end; "
        "        t := t + Sink.Take(move x); "
        "        done := true; "
        "      end; "
        "      return t; "
        "    end; "
        "    _ := f(); "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 3: same shape on a `case` instead of an `if`. The textually
// identical fix is applied to the CaseStmt arm — verifying the fix is
// not if-specific.
BEZEL_TEST(break_arm_forwarded_4141,
           lambda_body_case_break_arm_state_forwarded_to_while_join_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      var done := false; "
        "      var n := 1; "
        "      while not done do "
        "        case n of "
        "          0 => do t := t + Sink.Take(move x); break; end, "
        "          _ => do end "
        "        end case; "
        "        t := t + Sink.Take(move x); "
        "        done := true; "
        "      end; "
        "      return t; "
        "    end; "
        "    _ := f(); "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 4: same shape on a `for` loop instead of a `while`. The
// iterable (`0..<1`) guarantees exactly one iteration, so the if-false
// fallthrough runs and the done-flag pattern is unnecessary.
BEZEL_TEST(break_arm_forwarded_4141,
           lambda_body_for_if_break_arm_state_forwarded_to_for_join_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      for var i in 0..<1 do "
        "        if Sink.Flag(false) then "
        "          t := t + Sink.Take(move x); "
        "          break; "
        "        end; "
        "        t := t + Sink.Take(move x); "
        "      end; "
        "      return t; "
        "    end; "
        "    _ := f(); "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// Anti-regression controls (parseAndCheckOwnership, diagnostic-only) — must
// be REJECTED identically before and after the fix. These pin (a) the
// #4116/#4117 UAF fixes are undiminished by #4141's narrower join, and
// (b) the two soundness traps #4138's own history flagged stay closed.
// ===========================================================================

// Row 5: the #4138 Row-13 critical case — a `break` nested INSIDE an
// `if`, itself nested inside a `while`. The `if`'s own join MUST
// still fold the break-arm's move into ITS state (now via the fix's
// `pendingDivertedStates_` — the state is forwarded to the WHILE, not
// folded into the if), so that state reaches the enclosing `while`'s
// own join too, and a second `move x` after the whole `while` is still
// rejected. A naive fix that simply dropped the break-arm (treating
// "doesn't reach the if's own merge" as sufficient reason to discard
// it) silently ACCEPTED this program — reopening a real double-move.
BEZEL_TEST(break_arm_forwarded_4141,
           if_nested_in_while_lambda_break_forwarded_to_while_join_still_rejected_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      while Sink.Flag(true) do "
        "        if Sink.Flag(true) then "
        "          t := t + Sink.Take(move x); "
        "          break; "
        "        end; "
        "      end; "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedReadMsg));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedConsumeMsg));
}

// Row 6: the #4138 Row-11 shape — an UNLABELED `break` directly in a
// `while` body, consuming `x`. The break's state MUST still reach
// the while's post-loop join (the fix doesn't change this — only the
// indirect path via an enclosing if/case), so a second `move x` after
// the loop is still rejected.
BEZEL_TEST(break_arm_forwarded_4141,
           while_lambda_break_state_not_dropped_still_rejected_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      while Sink.Flag(true) do "
        "        t := t + Sink.Take(move x); "
        "        break; "
        "      end; "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedReadMsg));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedConsumeMsg));
}

// Row 7: the #4138 Row-12 shape — same as Row 6 with `continue`
// instead of `break`. `continue` re-enters the loop header, and when
// the loop's condition then goes false the state as of the `continue`
// point is what survives past the loop; a second `move x` after the
// loop is still rejected.
BEZEL_TEST(break_arm_forwarded_4141,
           while_lambda_continue_state_not_dropped_still_rejected_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      while Sink.Flag(true) do "
        "        t := t + Sink.Take(move x); "
        "        continue; "
        "      end; "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedReadMsg));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedConsumeMsg));
}

// Row 8: real two-arms-both-consume shape — a `case` where TWO ARMS
// (no break) both consume `x` independently. The join sees `x`
// MovedFrom on both arms, which converges to a definite MovedFrom (NOT
// maybe-consumed — `joinInto`'s same-state Owned⊓Owned rule applies
// for the MovedFrom⊓MovedFrom merge). The variable `t` is declared
// before the case (so the arms' assignments to it are fresh). This
// row pins that the fix doesn't perturb the definite-vs-maybe-consumed
// convergence rule for the two-real-arms-consume shape — a regression
// here would surface as a NEW maybe-consumed where there should be
// none.
BEZEL_TEST(break_arm_forwarded_4141,
           case_real_both_arms_consume_converges_to_definite_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      var n := 2; "
        "      case n of "
        "        1 => do t := 0 + Sink.Take(move x); end, "
        "        _ => do t := 1 + Sink.Take(move x); end "
        "      end case; "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
