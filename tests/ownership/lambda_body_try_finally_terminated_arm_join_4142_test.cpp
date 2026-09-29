// Spec-derived regression tests for issue #4142 — a `try` body's normal-
// completion path that ends in `return`/`throw` leaves the enclosing
// callable outright, so the post-`try` code is unreachable via that path.
// The lambda-body pre-pass's `Try` join (`applyUsingVarBorrowPinInLambdaStmt`,
// Ownership.cpp ~line 10934) had been JOINING the body's exit state with
// every catch arm's exit state into the post-`try` state unconditionally,
// the SAME save/restore-discards-a-move shape #4116/#4117 introduced and
// #4138 fixed for `case`/`if`/`while`/`for` — but the `try` case was
// deliberately left unfixed in #4138 because the body's operand also feeds
// `finally` (which ALWAYS runs, even on a path that just returned), and
// naively excluding the terminated operand broke #4138's finally-soundness
// property (measured: `try t := t + Take(move x); return t; finally t :=
// t + Take(move x); end;` would silently re-accept the double-move).
//
// FIX (narrow): split `tryEnds` into two views. `tryEndsForFinally` is the
// FULL union — what `finally` walks from, exactly as before. `tryEndsForJoin`
// is the same set with every `return`/`throw`-terminated operand excluded —
// what survives past the whole `try`. The finally walk runs first (mutating
// `states_` in place, the way it always has), then `states_` is REASSIGNED
// to the filtered join. Same shape as `CaseStmt`/`IfStmt` #4138, generalized
// to the {body-normal-completion} ∪ {catch arms} ∪ {finally-always-runs}
// control surface. `armNeverFallsThrough`/`stmtNeverFallsThrough` already
// had the predicate machinery — this fix is wiring, not new logic.
//
// ROW PLAN
//   Row 1: try body ends in `return`, finally does nothing, post-`try`
//          `move x` — body excluded from post-`try` join (body returns),
//          finally walks from body's MovedFrom (still MovedFrom after
//          finally's no-op), post-`try` join is empty so states reverts
//          to pre-try (Owned), `move x` ACCEPTED. Pre-fix: rejected with
//          definite "already moved". Post-fix: accepted.
//   Row 2: try body ends in `return`, finally has `move x` — finally
//          MUST see the body's operand (finally always runs). The
//          double-move rejection is preserved. This is the load-bearing
//          soundness pin (a sloppy filter accidentally excludes the body
//          from BOTH views would silently re-accept the double-move).
//   Row 3: try body ends in `return`, catch arm does nothing, post-`try`
//          `move x` — body excluded, catch kept. Post-`try` join sees
//          only catch's Owned. `move x` ACCEPTED. Pre-fix: rejected
//          (body's MovedFrom ⊓ catch's Owned = maybeConsumed).
//   Row 4: try/catch/finally where BOTH body and catch end in `return`,
//          finally does nothing, post-`try` `move x` — both operands
//          excluded (post-`try` code unreachable on every path), post-`try`
//          join empty → states reverts to pre-try (Owned), `move x`
//          ACCEPTED. Pre-fix: rejected with maybe-consumed (both
//          MovedFrom/Owned joined). Post-fix: accepted. This row
//          verifies the multi-terminator case is not over-conservative.
//   Row 5 (accept-side regression guard, §15): the existing working
//          shape — try body consumes `x` (no terminator), catch arm
//          does nothing, post-`try` `move x` again. BOTH paths reach
//          post-`try` code (body normal-completion OR body throws into
//          catch), so the join must remain `maybeConsumed`. This is the
//          SAME shape as lambda_body_branch_join_discards_move_4117
//          Row 2. My filter must NOT over-relax this join.
//
// §21.7 (try/catch/finally semantics), §28.3 K1.4 (branch join), §28.8.1
// (exception path discharge), v0.298.
//
// per §21.7, §28.3 K1.4, §28.8.1, v0.298, #4142, #4138

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "support/Diagnostic.hpp"
#include "support/ownership_support.hpp"   // parseAndCheckOwnership, hasErrorMessage
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// A destructor-counter Res (matching the established #3951/#4116/#4117/
// #4138/#4141 family idiom): ctor ++, dtor --, surfaced through Res.live
// so an ACCEPT test can assert exact-once disposal. The throwable MyErr
// makes the throw-terminator available to every row without a per-row
// prelude. Sink.Take is the established owned-parameter receiver.
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

const std::string kAlreadyMovedMsg =
    "owned value in `x` was already moved out at this point (§28.3)";

const std::string kMaybeConsumedMsg =
    "cannot consume `x`: it may have already been consumed on another "
    "branch (maybe-consumed join, §28.3)";

}  // namespace

// ===========================================================================
// Diagnostic-only rows (parseAndCheckOwnership).
// ===========================================================================

// Row 1: try body ends in `return` (no catch), finally that does nothing,
// post-`try` `move x`. The body always returns, so the post-`try` `move x`
// is on an UNREACHABLE path. Pre-#4142 the body's MovedFrom was folded
// into the post-`try` join (the only operand), so the unreachable `move x`
// was rejected with the definite "already moved" message — a false reject
// on dead code. Post-#4142 the body is excluded, the post-`try` join is
// empty, `states_` reverts to the pre-try snapshot (Owned), and the dead
// `move x` is no longer rejected.
// FAIL-WITHOUT the fix: hasErrors() is true, with the
// "already moved" message. PASS-WITH: hasErrors() is false.
BEZEL_TEST(lambda_body_try_finally_terminated_arm_join_4142,
           try_finally_return_body_post_try_move_dead_code_accepted_28_3) {
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
        "        _ := 0; "
        "      end; "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Row 2 (load-bearing soundness pin): try body ends in `return`,
// finally has `move x`. The finally walk MUST see the body's operand
// (finally always runs, even on the body's return path). Pre-#4142 the
// body's operand was already in `tryEnds` for finally; the fix's
// `tryEndsForFinally` preserves that exactly. A first draft of #4142
// accidentally excluded the body from BOTH views — silently re-accepting
// this double-move. This row pins that the fix does NOT regress #4138's
// finally-soundness property.
// FAIL-WITHOUT: hasErrors() is true (this row is the #4138 soundness
// pin, separate from Row 14 of terminated_arm_excluded_from_join_4138
// because this file pins the lambda-body pre-pass's `try` join
// specifically). PASS-WITH: same rejection.
BEZEL_TEST(lambda_body_try_finally_terminated_arm_join_4142,
           try_finally_return_body_finally_double_move_rejected_28_3) {
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

// Row 3: try body ends in `return`; a `catch e => ; end;` arm keeps `x`
// in Owned state. The post-`try` `move x` is unreachable via the body
// (which always returned) but reachable via the catch arm's `;` (which
// falls through). With my fix the body is excluded (return) and the catch
// is kept (no terminator) — the post-`try` join sees ONLY the catch's
// Owned state, so the post-`try` `move x` is ACCEPTED.
// FAIL-WITHOUT: rejected with maybe-consumed (body's MovedFrom ⊓ catch's
// Owned = maybeConsumed). PASS-WITH: accepted.
BEZEL_TEST(lambda_body_try_finally_terminated_arm_join_4142,
           try_catch_return_body_post_try_move_accepted_28_3) {
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
        "      catch e: MyErr => "
        "        t := 0; "
        "      end; "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Row 4: try/catch/finally where BOTH the body AND a catch arm end in
// `return`, finally does nothing, post-`try` `move x`. Both terminated
// operands are excluded from the post-`try` join (the post-`try` code is
// unreachable on every path), so the post-`try` join is empty and
// `states_` reverts to the pre-try snapshot (Owned). The unreachable
// `move x` is ACCEPTED (no diagnostic). Pre-fix: rejected with
// maybe-consumed (body's MovedFrom ⊓ catch's Owned = maybeConsumed).
// Post-fix: accepted. This row verifies the multi-terminator case is
// not over-conservative.
// FAIL-WITHOUT: rejected with maybe-consumed. PASS-WITH: accepted.
BEZEL_TEST(lambda_body_try_finally_terminated_arm_join_4142,
           try_catch_finally_dual_return_post_try_move_accepted_28_3) {
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
        "      catch e: MyErr => "
        "        return 0; "
        "      finally "
        "        _ := 0; "
        "      end; "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Row 5 (accept-side regression guard, §15): the existing working
// shape — try body normal-completion `move x`, catch arm `;`, post-`try`
// `move x` again. BOTH paths reach post-`try` code (body normal-
// completion OR body throws into catch), so the join must remain
// `maybeConsumed`. This is the SAME shape as lambda_body_branch_join
// _discards_move_4117 Row 2. My filter must NOT over-relax this join.
// FAIL-WITHOUT: rejected with maybe-consumed (matches #4117 Row 2).
// PASS-WITH: same rejection — this row pins the FIX did not break the
// working shape, distinct from Rows 1/3/4 which pin NEW accept behavior.
BEZEL_TEST(lambda_body_try_finally_terminated_arm_join_4142,
           try_catch_normal_body_post_try_move_still_maybe_consumed_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := 0; "
        "      try "
        "        t := t + Sink.Take(move x); "
        "      catch e: MyErr => "
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

// ===========================================================================
// Run-time row: Row 1 must NOT just compile clean — it must NOT heap-use-
// after-free at runtime, either. The lambda runs end-to-end and exits.
// Res.live returns to 0 exactly.
// ===========================================================================

// Row 6: the runtime twin of Row 1. The try body consumes `x` and
// returns; the post-try code is unreachable. Running the program
// exercises the live path (return t) and asserts the Res.live counter
// returns to 0 — proves no UAF, no leak.
BEZEL_TEST(lambda_body_try_finally_terminated_arm_join_4142,
           try_finally_return_body_no_leak_no_uaf_28_3) {
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
        "        _ := 0; "
        "      end; "
        "      return t; "
        "    end; "
        "    var v := f(); "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}
