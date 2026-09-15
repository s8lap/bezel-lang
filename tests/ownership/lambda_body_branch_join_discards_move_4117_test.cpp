// Spec-derived regression tests for issue #4117 — the lambda-body pre-pass's
// `states_` save/restore around `if`/`while`/`for`/`try`/`case` processing
// (`fix #3951`, commit `6d56e3c`) DISCARDS a real move recorded inside the
// branch instead of JOINING it into the post-branch state, so a double-move
// split across a branch construct — `move x` inside the branch, `move x`
// again right after it, all inside the same lambda body — is silently
// accepted and heap-use-after-frees, exactly the class of bug `#3951` itself
// exists to fix, surviving whenever the two moves are split by a branch.
//
// ROOT CAUSE: `MethodOwnership::applyUsingVarBorrowPinInLambdaStmt`
// (Ownership.cpp) is flow-INSENSITIVE — it has no real CFG, unlike the
// per-method dataflow's `runDataflow`/`joinInto` (see #4116's fuller
// rationale for that real join). `#3951` added a save-before/restore-after
// around each branch construct so a move recorded on one MUTUALLY-EXCLUSIVE
// arm would not be mistaken for a move on a SIBLING arm — correct for that
// purpose, and unchanged by this fix (each arm still resets to the pre-
// branch snapshot BEFORE it runs). But "restore the pre-branch snapshot
// unconditionally AFTER every arm ran" also throws away the branch's real
// effect on the state that code AFTER the whole construct sees — which is
// not a sibling of the branch, it is downstream of it. This fix instead
// JOINS every arm's exit state (mirroring the real per-method `IfStmt` join
// and `#4116`'s identical fix to the real per-method `CaseStmt` join),
// producing the same `Owned`+`maybeConsumed` state a real branch-join
// produces, which `MethodOwnership::markMovedByName`'s existing
// `st->maybeConsumed` branch (live since the #1626 era) then correctly
// rejects on the second `move`.
//
// MEASURED on this branch's base (102d29f, `--sanitize=address`), per the
// issue's own table: `if` (no else), `try`/`catch`, and `for` are ASan-
// CONFIRMED heap-use-after-frees; `while` and the `if`/`else`-both-arms-
// consume variant are diagnostic-silence-confirmed (rc=0, zero relevant
// diagnostics) but were NOT independently reproduced under ASan by the
// filer — this file does not claim otherwise for those two rows. The
// `case`-in-this-walker shape was not in the issue's own measured table at
// all; it is fixed here because it is the textually identical save/restore
// pattern in the SAME function, and pinned with a diagnostic-only row.
//
// §28.3 K1.3 (v0.295): "the obligation has transferred out; the binding or
// slot is unreadable until reassigned, and discharge machinery skips it."
// §28.3 (#1626, v0.180), quoted in `markMovedByName`: "a maybe-consumed
// local ... may not be consumed after the join either: on the consuming
// branch the value is already gone, so this `move`/`owned`-transfer/`using
// var` adoption would transfer a stale reference."
//
// ACCEPT-SIDE CONTROLS (§15): a branch construct where EVERY arm
// independently (and legally) consumes the same local must still converge
// to a definite discharge — not maybe-consumed — so a FURTHER consuming use
// after it is still correctly rejected with the PLAIN "already moved"
// message, never the more-conservative maybe-consumed one. Rows below pin
// this for the lambda-body pre-pass's `if`/`else` and `case` arms — mirrors
// `#3967`'s own `ifelse_both_branches_consume_reassign_accepted_28_3`
// control's INTENT (every-arm-consumes must not look maybe-consumed), via a
// further-move probe rather than a reassignment probe. A REASSIGNMENT probe
// was tried and abandoned here — see the note attached to Row 7 below,
// which documents an independent, pre-existing gap it uncovered in this
// same walker's `AssignmentStmt` handling, filed separately.
//
// per §28.3 K1.3, §19.6, v0.295, #3951, #4116, #4117

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "support/Diagnostic.hpp"
#include "support/ownership_support.hpp"   // parseAndCheckOwnership, hasErrorMessage
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// A destructor-counter Res, matching the established family idiom
// (lambda_body_move_transfer_3951_test.cpp,
// maybe_consumed_reassign_displaced_drop_3967_test.cpp).
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

const std::string kMaybeConsumedMsg =
    "cannot consume `x`: it may have already been consumed on another "
    "branch (maybe-consumed join, §28.3)";

const std::string kAlreadyMovedMsg =
    "owned value in `x` was already moved out at this point (§28.3)";

}  // namespace

// ===========================================================================
// Diagnostic-only rows (parseAndCheckOwnership) — one per construct the
// save/restore touches.
// ===========================================================================

// Row 1: `if` with no `else` — ASan-confirmed by the issue. `move x` inside
// the branch, `move x` again right after it.
// FAIL-WITHOUT the fix: hasErrors() is false (the restore discarded the
// branch's move); PASS-WITH: true, with the maybe-consumed message.
BEZEL_TEST(lambda_body_branch_join_discards_move_4117,
           if_no_else_second_move_after_rejected_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      if Sink.Flag(true) then "
        "        t := t + Sink.Take(move x); "
        "      end; "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedMsg));
}

// Row 2: `try`/`catch` — ASan-confirmed by the issue. `move x` inside the
// `try` body, `move x` again right after the whole `try` statement.
BEZEL_TEST(lambda_body_branch_join_discards_move_4117,
           try_catch_second_move_after_rejected_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      try "
        "        t := t + Sink.Take(move x); "
        "      catch e: Exception => "
        "        t := 0; "
        "      end; "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedMsg));
}

// Row 3: `for` — ASan-confirmed by the issue. `move x` inside the loop body,
// `move x` again right after the whole `for` statement.
BEZEL_TEST(lambda_body_branch_join_discards_move_4117,
           for_second_move_after_rejected_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      for var i in 0..1 do "
        "        t := t + Sink.Take(move x); "
        "      end; "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedMsg));
}

// Row 4: `while` — diagnostic-silence confirmed by the issue, NOT
// independently ASan-run (the issue's own table is explicit about this
// distinction; this test does not claim otherwise). `move x` inside the
// loop body, `move x` again right after the whole `while` statement.
BEZEL_TEST(lambda_body_branch_join_discards_move_4117,
           while_second_move_after_rejected_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      var i := 0; "
        "      while i < 1 do "
        "        t := t + Sink.Take(move x); "
        "        i := i + 1; "
        "      end; "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedMsg));
}

// Row 5: `case` (statement) inside a lambda body — not in the issue's own
// measured table (which covers if/try/for/while/if-else), but the identical
// save/restore-discards-a-move shape in the SAME function's CaseStmt arm.
// One consuming arm plus a non-consuming wildcard sibling, then a second
// `move x` right after the whole `case`.
BEZEL_TEST(lambda_body_branch_join_discards_move_4117,
           case_wildcard_sibling_second_move_after_rejected_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      var n := 1; "
        "      case n of "
        "        1 => do t := t + Sink.Take(move x); end, "
        "        _ => do end "
        "      end case; "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedMsg));
}

// ===========================================================================
// Accept-side controls (§15) — every arm consumes, so the converged state is
// a DEFINITE MovedFrom, never maybe-consumed. A THIRD touch after the join
// still correctly rejects (plain "already moved", not maybe-consumed); a
// REASSIGNMENT after the join is accepted, exactly as #3967's own
// `ifelse_both_branches_consume_reassign_accepted_28_3` requires.
// ===========================================================================

// Row 6: `if`/`else` where BOTH arms consume `x` — a third `move x` right
// after must still be rejected, but with the PLAIN "already moved" message
// (state converges to definite MovedFrom via same-state merge — `#1626`'s
// `maybeConsumed` flag is never armed for a same-state Owned/MovedFrom
// join), not the maybe-consumed one. Pins that the join's `joinInto` call
// (§28.3 same-state merge: `if (st.state == o.state) continue;`) behaves
// identically inside this walker as it does in the real per-method join.
BEZEL_TEST(lambda_body_branch_join_discards_move_4117,
           if_else_both_consume_third_move_after_rejected_plain_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(b: bool): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      if Sink.Flag(b) then "
        "        t := t + Sink.Take(move x); "
        "      else "
        "        t := t + Sink.Take(move x); "
        "      end; "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    end; "
        "    return f(true); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kAlreadyMovedMsg));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kMaybeConsumedMsg));
}

// Row 7 (accept-side control): `case` where every arm — including the
// wildcard — consumes `x`, then a THIRD `move x` after the whole `case`.
// The parallel of Row 6, for `case` instead of `if`/`else`: must still be
// rejected, but with the PLAIN "already moved" message (every arm's exit
// state is a definite MovedFrom; `caseHasUnguardedCatchAllArm` keeps the
// pre-case snapshot OUT of the join for this shape, so no spurious
// maybe-consumed marker is ever armed). Pins that this walker's `case` join,
// like #4116's real per-method twin, carves out the syntactic-catch-all
// shape correctly.
//
// NOTE ON WHAT THIS FILE DOES NOT TEST: an accept-side control that
// REASSIGNS `x` (`x := new Res(9);`) after a consuming branch and then
// touches it again was tried while drafting this file and found to be
// independently broken — `MethodOwnership::applyUsingVarBorrowPinIn-
// LambdaStmt`'s `AssignmentStmt` arm (Ownership.cpp) only walks the RHS
// (`checkRead`/`recordMovesIn` on `as->value`) and never updates the LHS
// symbol's OWN tracked state, so a plain STRAIGHT-LINE (no branch at all)
// `Sink.Take(move x); x := new Res(9); x.Dispose();` inside a lambda body
// already rejects `x.Dispose()` with "owned value in `x` was already moved
// out at this point (§28.3)" — `x` never became un-moved-from in this
// walker's eyes, reassignment or not. That is a PRE-EXISTING, INDEPENDENT
// gap (this fix touches no `AssignmentStmt` handling), filed separately —
// see the issue this file's own commit references — and is why every
// accept-side row here avoids reassigning a consumed local afterward.
BEZEL_TEST(lambda_body_branch_join_discards_move_4117,
           case_every_arm_consumes_third_move_after_rejected_plain_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      var n := 1; "
        "      case n of "
        "        1 => do t := t + Sink.Take(move x); end, "
        "        _ => do t := t + Sink.Take(move x); end "
        "      end case; "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kAlreadyMovedMsg));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kMaybeConsumedMsg));
}

// ===========================================================================
// Real-execution rows (runAotChecked) — the actual crash and its fix for the
// three ASan-confirmed constructs, plus loop-amplified accept-side memory
// safety.
// ===========================================================================

// Row 8: the `if` shape, real execution. FAIL-WITHOUT this fix: the program
// compiles rc=0 (zero relevant diagnostics) and heap-use-after-frees at
// runtime under `--sanitize=address` (measured on this branch's base) — so
// `failedAtCompileTimeWithRealLocation` is FALSE. PASS-WITH: the program is
// rejected at COMPILE time, so it is TRUE, paired with message text (a
// compile-time helper alone is not enough when the pre-fix behaviour is a
// crash).
BEZEL_TEST(lambda_body_branch_join_discards_move_4117,
           if_no_else_crash_fixed_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      if Sink.Flag(true) then "
        "        t := t + Sink.Take(move x); "
        "      end; "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kMaybeConsumedMsg.c_str()));
}

// Row 9: the `try`/`catch` shape, real execution. Mirrors Row 8.
BEZEL_TEST(lambda_body_branch_join_discards_move_4117,
           try_catch_crash_fixed_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      try "
        "        t := t + Sink.Take(move x); "
        "      catch e: Exception => "
        "        t := 0; "
        "      end; "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kMaybeConsumedMsg.c_str()));
}

// Row 10: the `for` shape, real execution. Mirrors Row 8.
BEZEL_TEST(lambda_body_branch_join_discards_move_4117,
           for_crash_fixed_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      for var i in 0..1 do "
        "        t := t + Sink.Take(move x); "
        "      end; "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kMaybeConsumedMsg.c_str()));
}

// Row 11 (accept-side control, real execution, LOOP-AMPLIFIED 500x per
// [[lsan-one-shot-leak-tests-pass-vacuously]]): `case` where every arm —
// including the wildcard — consumes `x`, alternating which arm runs across
// iterations, asserting exact-once disposal (`Res.live == 0`). This is the
// real-execution twin of Row 7, and — unlike #3951's own pre-existing
// `lambda_if_else_both_branches_dispose_once_amplified` real-execution
// control (which this file's `if`/`else` rows above deliberately do not
// re-duplicate) — the lambda-body pre-pass's `case`-arm join has no prior
// real-execution coverage at all, so this closes that gap and proves this
// fix's `case` join (and its catch-all carve-out) causes no leak, no
// double-free, and no false reject on this shape.
BEZEL_TEST(lambda_body_branch_join_discards_move_4117,
           case_every_arm_consumes_disposes_once_amplified) {
    // `n` is a lambda PARAMETER (`f(i mod 2)`), not a capture of the outer
    // loop's own mutated `i` — matching #3951's/#4116's own established
    // idiom (`lambda(b: bool): int => do if Sink.Flag(b) then ... end;`,
    // called `f(i mod 2 = 0)`). Deliberately NOT `var n := i mod 2;` inside
    // the lambda body capturing `i` directly: that shape was tried while
    // drafting this file and hit an UNRELATED, pre-existing, already-
    // measured-on-origin/main capture-cell leak for a lambda that captures
    // an enclosing loop's mutated variable (4 bytes leaked per PROGRAM, not
    // per iteration, present even at 1 outer iteration and with no `case`
    // involved at all) — filed separately; seeing it here would have been a
    // false signal against this fix, which touches no capture machinery.
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var f := lambda(n: int): int => do "
        "        var x := new Res(5); "
        "        var t := 0; "
        "        case n of "
        "          0 => do t := t + Sink.Take(move x); end, "
        "          _ => do t := t + Sink.Take(move x); end "
        "        end case; "
        "        return t; "
        "      end; "
        "      _ := f(i mod 2); "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
