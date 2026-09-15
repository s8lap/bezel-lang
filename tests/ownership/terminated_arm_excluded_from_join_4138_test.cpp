// Spec-derived regression tests for issue #4138 — a false-reject REGRESSION
// this batch's own #4116/#4117 introduced. Those two commits fixed a real
// use-after-free (a case/if/while/for/try arm-join was discarding every
// arm's effect on `states_` instead of JOINING it — #4138's own review of
// `96a5e2d`/`0570d2e`), but neither checked whether an arm's last reachable
// statement was a terminator before folding its end state into the join.
// Net effect: a `case`/`if` where a `return`/`throw`-terminated arm consumes
// an owned local, with a sibling REACHABLE arm that does not, produced a
// spurious maybe-consumed diagnostic on code after the whole construct —
// even though the only way to reach that code is via the non-consuming arm.
//
// FIX: `armNeverFallsThrough` (Ownership.cpp ~line 190) excludes an arm (or
// loop body) whose last reachable statement is `return`/`throw` — but NOT
// `break`/`continue` — from the four join sites #4116/#4117 touched:
// `MethodOwnership::visitStmt`'s real per-method `CaseStmt` arm, and
// `applyUsingVarBorrowPinInLambdaStmt`'s If/Case/While/For arms (the
// lambda-body pre-pass). The `Try` join (same function) is DELIBERATELY
// left unfixed — see the Try control row below and issue #4142.
//
// WHY NOT break/continue (measured, not designed up front — see issue
// #4141): `return`/`throw` leave the enclosing callable outright, so
// dropping their contribution from any join is always safe. `break`/
// `continue` reach a DIFFERENT, but still-live, merge point (the enclosing
// loop's own post-loop merge / header) — a first draft excluded them too
// and a measured counter-example (`while cond do if flag then move x;
// break; end; end; move x;`) showed that DISCARDS the move information
// instead of forwarding it to the loop's own join, silently un-rejecting a
// genuine double-move (reopening the #3951/#4117 UAF class). Rows 11-13
// below pin that this project's fix does NOT do that.
//
// WHY NOT Try (measured, not designed up front — see issue #4142): `finally`
// always runs, on every path, but is walked exactly ONCE from a single
// `states_` in this flow-insensitive walker — so the state fed into it must
// be the union of every path, terminated or not. Excluding a
// `return`-terminated try-body from the join the same way Case/If do is
// UNSOUND, not merely incomplete: measured, `try t := t + Take(move x);
// return t; finally t := t + Take(move x); end;` (no catch arm) correctly
// rejects the `finally` block's `move x` on both `origin/main` and this
// batch's base — a naive #4138-style exclusion silently ACCEPTED it,
// reopening a real double-move (`finally` always runs after the body's
// `return`, and would double-move `x`). Row 14 below pins that this
// project's fix leaves `Try` exactly as #4117 landed it.
//
// §19.6 (v0.295): "A `case` with no `_` arm is permitted. In statement
// position, an unmatched value falls through with no action." §28.3 K1.4:
// the maybe-consumed join. §21.5: `break`/`continue` semantics.
//
// MEASURED regression evidence (this file's Rows 1-9): every accept-shaped
// row below compiles clean (rc=0) on pristine `origin/main` (`102d29f`,
// before #4116/#4117) AND is falsely rejected on this batch's base
// (`3bdae03`, #4116+#4117 landed, before #4138) — i.e. #4116/#4117
// introduced these false rejects, #4138 fixes them.
//
// per §19, §19.6, §21.5, §28.2, §28.3 K1.4, v0.295, #4138, #4116, #4117

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

// A destructor-counter Res (matching the established #3951/#4116/#4117
// family idiom) plus a throwable MyErr, so both terminators (`return` and
// `throw`) are available to every row without a per-row prelude.
const char* kRes =
    "module M; "
    "class MyErr: Exception "
    "  public constructor() end constructor; "
    "end MyErr; "
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

const std::string kMaybeConsumedDisposeMsg =
    "cannot dispose `x`: it may have already been consumed on another "
    "branch (maybe-consumed join, §28.3)";

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
// Accept-side rows (runAotChecked, real execution) — the actual #4138 fix.
// FAIL-WITHOUT: rejected with a maybe-consumed diagnostic on this batch's
// base (`3bdae03`). PASS-WITH: compiles and RUNS, asserted via the runtime
// value (`Res.live`, exact-once disposal), not merely "it compiled" — per
// [[regression-test-pins-the-bug]] and this run's own accept-shaped-row
// discipline.
// ===========================================================================

// Row 1: the real per-method `CaseStmt` join (`MethodOwnership::visitStmt`)
// — the ISSUE'S OWN filed repro. A throwing arm consumes `x`; the sibling
// `_` arm (the one actually selected at runtime, `n == 2`) does not.
BEZEL_TEST(terminated_arm_excluded_from_join_4138,
           case_stmt_throw_arm_excluded_accepted_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    var n := 2; "
        "    case n of "
        "      1 => do t := t + Sink.Take(move x); throw new MyErr(); end, "
        "      _ => do end "
        "    end case; "
        "    x.Dispose(); "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 2: same site, `return` instead of `throw` (the other terminator the
// fix recognizes).
BEZEL_TEST(terminated_arm_excluded_from_join_4138,
           case_stmt_return_arm_excluded_accepted_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    var n := 2; "
        "    case n of "
        "      1 => do t := t + Sink.Take(move x); return t; end, "
        "      _ => do end "
        "    end case; "
        "    x.Dispose(); "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 3: the lambda-body pre-pass's `If` join
// (`applyUsingVarBorrowPinInLambdaStmt`) — the issue's second filed repro.
// `Sink.Flag(false)` means the `return`-arm is NOT taken at runtime, so the
// code after the `if` runs via the only live path, where `x` is untouched.
BEZEL_TEST(terminated_arm_excluded_from_join_4138,
           if_lambda_return_arm_excluded_accepted_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      if Sink.Flag(false) then "
        "        t := t + Sink.Take(move x); "
        "        return t; "
        "      end; "
        "      t := t + Sink.Take(move x); "
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

// Row 4: same site, `throw` instead of `return`.
BEZEL_TEST(terminated_arm_excluded_from_join_4138,
           if_lambda_throw_arm_excluded_accepted_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      if Sink.Flag(false) then "
        "        t := t + Sink.Take(move x); "
        "        throw new MyErr(); "
        "      end; "
        "      t := t + Sink.Take(move x); "
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

// Row 5: the lambda-body pre-pass's `Case` join (same function, its OTHER
// join site) — not in the issue's own filed table, but the textually
// identical shape one function up.
BEZEL_TEST(terminated_arm_excluded_from_join_4138,
           case_lambda_throw_arm_excluded_accepted_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      var n := 2; "
        "      case n of "
        "        1 => do t := t + Sink.Take(move x); throw new MyErr(); end, "
        "        _ => do end "
        "      end case; "
        "      x.Dispose(); "
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

// Row 6: the lambda-body pre-pass's `While` join — NOT in #4138's own filed
// table (found independently while fixing it: `stmtNeverFallsThrough`
// applies identically to a loop BODY, since only return/throw are
// recognized). `Sink.Flag(false)` means the loop runs 0 times.
BEZEL_TEST(terminated_arm_excluded_from_join_4138,
           while_lambda_return_arm_excluded_accepted_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      while Sink.Flag(false) do "
        "        t := t + Sink.Take(move x); "
        "        return t; "
        "      end; "
        "      t := t + Sink.Take(move x); "
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

// Row 7: the lambda-body pre-pass's `For` join — same independent finding
// as Row 6. `0..<0` (half-open, empty) means the loop runs 0 times.
BEZEL_TEST(terminated_arm_excluded_from_join_4138,
           for_lambda_return_arm_excluded_accepted_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      for var i in 0..<0 do "
        "        t := t + Sink.Take(move x); "
        "        return t; "
        "      end; "
        "      t := t + Sink.Take(move x); "
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
// Real-execution rows, LOOP-AMPLIFIED 500x per
// [[lsan-one-shot-leak-tests-pass-vacuously]] and run under ASan (verified
// manually via `scripts/agent_check.sh --probe --asan`, both clean — see
// this fix's commit body) — proves the fix's exclusion causes no leak, no
// double-free, alternating which arm actually runs at runtime.
// ===========================================================================

// Row 8: Row 2's real per-method `CaseStmt` shape, called 500 times
// alternating `n` between 0 (falls through the `_` arm, `x` disposed after
// the case) and 1 (consumed and disposed via `Sink.Take`'s own auto-drop,
// then an early `return` from the helper `Run` method) — every call
// disposes exactly once.
BEZEL_TEST(terminated_arm_excluded_from_join_4138,
           case_stmt_return_arm_excluded_disposes_once_amplified) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Run(n: int): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    case n of "
        "      1 => do t := t + Sink.Take(move x); return t; end, "
        "      _ => do end "
        "    end case; "
        "    x.Dispose(); "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      _ := C.Run(i mod 2); "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 9: Row 3's lambda-body `If` shape, called 500 times alternating the
// branch taken. `b` is a lambda PARAMETER (matching the established
// #3951/#4116/#4117 idiom, `f(i mod 2 = 0)`), NOT a capture of the outer
// loop's own mutated `i` — capturing it directly hits an unrelated,
// pre-existing capture-cell leak (#4130) that would be a false signal
// against this fix, which touches no capture machinery.
BEZEL_TEST(terminated_arm_excluded_from_join_4138,
           if_lambda_return_arm_excluded_disposes_once_amplified) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var f := lambda(b: bool): int => do "
        "        var x := new Res(5); "
        "        var t := 0; "
        "        if Sink.Flag(b) then "
        "          t := t + Sink.Take(move x); "
        "          return t; "
        "        end; "
        "        t := t + Sink.Take(move x); "
        "        return t; "
        "      end; "
        "      _ := f(i mod 2 = 0); "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// Anti-regression controls (parseAndCheckOwnership, diagnostic-only) — must
// be REJECTED identically before and after #4138. These pin (a) the
// #4116/#4117 UAF fixes are undiminished by #4138's narrower join, and (b)
// the two soundness traps a first draft of #4138 fell into (see the file
// header) stay closed.
// ===========================================================================

// Row 10: a REAL maybe-consumed through two NON-terminated sibling arms
// (neither ends in return/throw/break/continue) must still be rejected —
// #4116's own original purpose, unaffected by #4138's narrower exclusion.
BEZEL_TEST(terminated_arm_excluded_from_join_4138,
           case_stmt_real_maybe_consumed_still_rejected_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    var n := 2; "
        "    case n of "
        "      1 => do t := t + Sink.Take(move x); end, "
        "      _ => do end "
        "    end case; "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedDisposeMsg));
}

// Row 11: a `while` body ending in an UNLABELED `break` — `break` is
// deliberately NOT recognized by `armNeverFallsThrough` (it targets EXACTLY
// this loop's own merge, so its move must still be visible after the loop).
// A second `move x` right after the loop must still be rejected.
BEZEL_TEST(terminated_arm_excluded_from_join_4138,
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

// Row 12: same as Row 11, `continue` instead of `break` — `continue`
// re-enters the loop header, and when the loop's own condition then goes
// false, the state as of the `continue` point is what survives past the
// loop (this walker models one pass through the body as a proxy for "any"
// iteration), so it too must remain visible.
BEZEL_TEST(terminated_arm_excluded_from_join_4138,
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

// Row 13: the critical case a first draft of #4138 got WRONG (see the file
// header and issue #4141) — a `break` nested INSIDE an `if`, itself nested
// inside a `while`. The `if`'s own join must still fold the break-arm's
// move into ITS state (break is not excluded), so that state reaches the
// enclosing `while`'s own join too, and a second `move x` after the whole
// `while` is still rejected. A version of this fix that excluded `break`
// from the `if`'s join (treating "doesn't reach the if's own merge" as
// sufficient reason to drop it) silently ACCEPTED this program instead —
// reopening a real double-move.
BEZEL_TEST(terminated_arm_excluded_from_join_4138,
           if_nested_in_while_lambda_break_forwarded_still_rejected_28_3) {
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

// Row 14: the other trap a first draft fell into (see the file header and
// issue #4142) — a `try`/`finally` (no `catch` arm) whose body `move`s `x`
// then `return`s; `finally` ALWAYS runs, so its own `move x` is a genuine
// double-move and must stay rejected with the definite (non-maybe)
// "already moved out" message, exactly as on `origin/main` and this
// batch's base. A version of this fix that excluded the returning body
// from the `Try` join's `finally`-visible state silently ACCEPTED this.
BEZEL_TEST(terminated_arm_excluded_from_join_4138,
           try_finally_lambda_return_visibility_still_rejected_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      try "
        "        t := t + Sink.Take(move x); "
        "        return t; "
        "      finally "
        "        t := t + Sink.Take(move x); "
        "      end; "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kAlreadyMovedMsg));
}
