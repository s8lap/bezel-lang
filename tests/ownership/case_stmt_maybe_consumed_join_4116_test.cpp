// Spec-derived regression tests for issue #4116 — a statement-position
// `case`'s arm join never produced a `maybeConsumed` state, unlike the
// byte-identical `if`/`while`(`break`)/`try` shapes `narrow #3967` already
// covers, so a reassignment (or even a bare READ) after a `case` whose arms
// disagree on whether they consumed an owned local was silently accepted and
// heap-use-after-freed at runtime.
//
// ROOT CAUSE (confirmed by reading `MethodOwnership::visitStmt`'s CaseStmt
// arm, Ownership.cpp): the CFG treats a `CaseStmt` as a single opaque node
// (flow/CFG.cpp's `visitStmt` has no case handler — §19/§28.8, #1280's own
// comment), so this transfer function walks each arm from a snapshot of the
// pre-case state and — before this fix — UNCONDITIONALLY restored that same
// snapshot after the arm loop, discarding every arm's effect on `states_`
// outright. `MethodOwnership::joinInto`'s `ownedDischargedSplit` rule (the
// mechanism that produces Owned+`maybeConsumed` for an Owned⊓MovedFrom
// branch-join, #1626) was therefore never even reached for a `case`'s
// arm-to-arm merge — not a bug in `narrow #3967`'s own diff (`Ownership.cpp`
// ~6504's `st.state == OwnershipState::Owned && st.maybeConsumed` check is a
// passive reader of a flag `CaseStmt` never set), but a pre-existing gap in
// the underlying join machinery `#3967` did not know to check.
//
// §28.3 K1 item 4 (v0.295): "maybe-consumed — the K7 join of owned with
// moved-from or explicitly-disposed; unreadable; scope-end disposal is
// per-path." §19.6 (v0.295): "A `case` with no `_` arm is permitted. In
// statement position, an unmatched value falls through with no action" —
// the fix models that no-match edge as an extra join operand (the pre-case
// state), mirroring how `IfStmt`'s own real join already folds in its
// implicit `entry` fallthrough when there is no `else`.
//
// MEASURED on this branch's base (102d29f, `--sanitize=address`): the
// issue's own repro (Row 5 below) compiles rc=0 with zero relevant
// diagnostics and heap-use-after-frees on the FIRST outer-loop iteration.
//
// ACCEPT-SIDE CONTROL (§15): a `case` where EVERY arm — including an
// unguarded `_`/untyped-`var` catch-all — consumes the same outer local must
// still converge to a definite discharge (not a spuriously conservative
// maybe-consumed), exactly as `#3967`'s own
// `ifelse_both_branches_consume_reassign_accepted_28_3` control requires for
// `if`/`else`. `caseHasUnguardedCatchAllArm` (Ownership.cpp, the anonymous
// namespace near the top of the file) is the carve-out that keeps this case
// working: it recognizes the syntactic catch-all and skips folding the
// pre-case fallthrough into the join, so this shape's join sees only
// MovedFrom⊓MovedFrom (same-state, no `ownedDischargedSplit`), which stays a
// definite discharge.
//
// per §19, §19.6, §28.3 K1 item 4, §28.7, v0.295, #4116, #3967

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
// (maybe_consumed_reassign_displaced_drop_3967_test.cpp,
// lambda_body_move_transfer_3951_test.cpp): ctor ++, dtor --, surfaced
// through Res.live so an ACCEPT test can assert exact-once disposal.
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
    "  public static method Peek(r: Res): int return r.tag; end Peek; "
    "end Sink; ";

const std::string kReassignExpectedMsg =
    "cannot reassign `x`: it may have already been consumed on another "
    "branch (maybe-consumed join, §28.3 K1.4)";

const std::string kReadExpectedMsg =
    "may have already been consumed on another branch and may not be read "
    "here (maybe-consumed join, §28.3)";

}  // namespace

// ===========================================================================
// Diagnostic-only rows (parseAndCheckOwnership).
// ===========================================================================

// Row 1: the issue's own repro — a `case` with one consuming arm (`1 =>`)
// and a sibling wildcard arm that does nothing (`_ => do end`). The
// reassignment after the `case` sees `Owned`+`maybeConsumed`, exactly as the
// `if`-no-else shape does.
// FAIL-WITHOUT the fix: hasErrors() is false (silently accepted, `saved` was
// unconditionally restored so `x` looks untouched); PASS-WITH: true, with
// the same §28.3 K1.4 message `narrow #3967` uses for `if`/`while`.
BEZEL_TEST(case_stmt_maybe_consumed_join_4116,
           case_arm_consume_wildcard_sibling_reassign_rejected_28_3_k1_4) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    var n := 1; "
        "    case n of "
        "      1 => do t := t + Sink.Take(move x); end, "
        "      _ => do end "
        "    end case; "
        "    x := new Res(9); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kReassignExpectedMsg));
}

// Row 2: the issue's own SECOND shape — a bare READ (`Sink.Peek(x)`, a
// borrow-position use, no `move`/`.Dispose()`/reassignment) after the same
// `case`. This proves the gap is upstream of #3967's reassignment-specific
// check: `checkRead`'s own pre-existing maybeConsumed branch (live since the
// #1626 era, long before #3967) never fired because `CaseStmt`'s join never
// produced the `maybeConsumed` flag for it to read.
BEZEL_TEST(case_stmt_maybe_consumed_join_4116,
           case_arm_consume_wildcard_sibling_bare_read_rejected_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    var n := 1; "
        "    case n of "
        "      1 => do t := t + Sink.Take(move x); end, "
        "      _ => do end "
        "    end case; "
        "    t := t + Sink.Peek(x); "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kReadExpectedMsg));
}

// Row 3 (accept-side control, §15): every arm — including the wildcard —
// consumes `x`, so every path is a definite move, never a maybe-move. Pins
// `caseHasUnguardedCatchAllArm`'s carve-out: without it, folding the
// pre-case snapshot into the join unconditionally would falsely mark `x`
// maybe-consumed even though the whole `case` always consumes it, and the
// reassignment below would be wrongly rejected.
BEZEL_TEST(case_stmt_maybe_consumed_join_4116,
           case_every_arm_including_wildcard_consumes_reassign_accepted) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    var n := 1; "
        "    case n of "
        "      1 => do t := t + Sink.Take(move x); end, "
        "      _ => do t := t + Sink.Take(move x); end "
        "    end case; "
        "    x := new Res(9); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Row 4 (accept-side control, §15): no arm touches `x` at all — the
// non-consuming sibling of Row 1. Must remain clean, no maybeConsumed
// marker at all (both arms leave `x` plain Owned; same-state merge, no
// `ownedDischargedSplit`).
BEZEL_TEST(case_stmt_maybe_consumed_join_4116,
           case_no_arm_consumes_reassign_accepted) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    var n := 1; "
        "    case n of "
        "      1 => do t := t + Sink.Peek(x); end, "
        "      _ => do end "
        "    end case; "
        "    x := new Res(9); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// Real-execution rows (runAotChecked) — the actual crash and its fix, plus
// loop-amplified accept-side memory safety.
// ===========================================================================

// Row 5: the issue's own repro, real execution, loop-amplified (300
// iterations, matching the issue's own repro exactly). FAIL-WITHOUT this
// fix: the program compiles rc=0 (zero relevant diagnostics) and heap-use-
// after-frees at runtime under `--sanitize=address` (measured on this
// branch's base) on the FIRST iteration — so `failedAtCompileTimeWithReal-
// Location` is FALSE. PASS-WITH: the program is rejected at COMPILE time
// (before codegen / execution ever runs), so it is TRUE, paired with the
// exact §28.3 K1.4 message text (a compile-time helper alone is not enough
// when the pre-fix behaviour is a crash — pair it with message text).
BEZEL_TEST(case_stmt_maybe_consumed_join_4116,
           case_arm_consume_wildcard_sibling_crash_fixed_28_3_k1_4) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var t := 0; "
        "    while i < 300 do "
        "      var x := new Res(5); "
        "      var n := 1; "
        "      case n of "
        "        1 => do t := t + Sink.Take(move x); end, "
        "        _ => do end "
        "      end case; "
        "      x := new Res(9); "
        "      x.Dispose(); "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kReassignExpectedMsg.c_str()));
}

// Row 6 (accept-side control, real execution, LOOP-AMPLIFIED 500x per
// [[lsan-one-shot-leak-tests-pass-vacuously]]): the every-arm-consumes
// control (Row 3's real-execution twin), alternating which arm runs across
// iterations so BOTH are exercised, asserting exact-once disposal
// (`Res.live == 0`) — proves the fix's join (and its catch-all carve-out)
// causes no leak, no double-free, and no false reject on this shape.
BEZEL_TEST(case_stmt_maybe_consumed_join_4116,
           case_every_arm_consumes_disposes_once_amplified) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var t := 0; "
        "    while i < 500 do "
        "      var x := new Res(5); "
        "      var n := i mod 2; "
        "      case n of "
        "        0 => do t := t + Sink.Take(move x); end, "
        "        _ => do t := t + Sink.Take(move x); end "
        "      end case; "
        "      x := new Res(9); "
        "      x.Dispose(); "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
