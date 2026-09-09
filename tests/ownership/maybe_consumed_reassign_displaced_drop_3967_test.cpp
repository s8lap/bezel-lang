// Spec-derived regression tests for issue #3967 — reassigning a MAYBE-
// CONSUMED (conditionally moved-from) owned local still heap-use-after-frees;
// #3950's static gate (`lhsMovedFromAtStore`, keyed on the dataflow's
// converged state being exactly `MovedFrom`) cannot cover the K7.4 join case,
// where the converged state is `Owned` with `maybeConsumed` set.
//
// §28.3 K1, item 4 (v0.295), quoted verbatim in the issue:
//   "4. **maybe-consumed** — the K7 join of owned with moved-from or
//   explicitly-disposed; unreadable; **scope-end disposal is per-path**."
//
// §28.7 (drop-on-overwrite)'s own liveness test, stated dynamically: "if the
// previous value is live (non-null and not moved-from ...), it is disposed".
// For a plain (non-`using`) owned local, codegen's `move` lowering
// deliberately leaves the slot POPULATED after a move (so `new T(move n, n)`
// can still read the second operand — LlvmBackend_Exprs2.cpp), so there is no
// null to guard on for the moved path; and #3950's static skip only fires for
// a definite `MovedFrom` state, never for `Owned`+`maybeConsumed`. Neither
// half of the liveness test is available for this shape today: on the
// consumed path, `disposeDisplacedOwnedClassLocal` (LlvmBackend_Disposal.cpp)
// silently disposes a pointer the consumer already freed.
//
// MEASURED on this branch's base (2e25915, `--sanitize=address`): both the
// issue's own `if`-conditional-consume repro and its `break`-path sibling
// compile rc=0 with zero ownership diagnostics (beyond the unrelated §28.3
// owned-parameter auto-drop note) and heap-use-after-free.
//
// THE FIX in this commit is deliberately NARROW, not the full per-path
// runtime bit the spec's K1.4 wording ultimately calls for (which would
// thread a shadow "moved" flag through the move-lowering machinery three
// OTHER established null-site special cases already depend on —
// LlvmBackend_Exprs2.cpp's `new T(move n, n)` idiom chief among them — a
// change out of scope for this pass). Instead, `handleAssignment` now
// REJECTS a reassignment of a local it can prove is `Owned` with
// `maybeConsumed` set, scoped via `localDropOnOverwriteCovered` — the SAME
// predicate #3950 and the #1465 drop-on-overwrite acceptance already use to
// decide "does codegen's disposeDisplacedOwnedClassLocal actually run for
// this reassignment" — so this converts the measured heap-use-after-free
// into a compile-time error instead of leaving it silently unsound.
//
// ACCEPT-SIDE CONTROLS (§15): the shapes #3950 already fixed — EVERY path
// consumes (state converges to definite `MovedFrom`, not maybe-consumed) —
// must remain accepted and clean. Rows *_ifelse_both_branches_consume_* and
// *_reacquire_cycle_* pin this, both via the diagnostic-only harness and
// (loop-amplified 500x per [[lsan-one-shot-leak-tests-pass-vacuously]]) via
// real execution asserting `Res.live == 0`.
//
// per §28.3 K1 item 4, §28.7, v0.295, #3950

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
// (lambda_return_move_own_local_double_free_4049_test.cpp,
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
    "  public static method Flag(b: bool): bool return b; end Flag; "
    "end Sink; ";

const std::string kExpectedMsg =
    "cannot reassign `x`: it may have already been consumed on another "
    "branch (maybe-consumed join, §28.3 K1.4)";

}  // namespace

// ===========================================================================
// Diagnostic-only rows (parseAndCheckOwnership).
// ===========================================================================

// Row 1: the issue's own repro — an `if` with no `else` conditionally
// consumes `x`; the reassignment after the join sees `Owned`+`maybeConsumed`.
// FAIL-WITHOUT the fix: hasErrors() is false (silently accepted); PASS-WITH:
// true, with the new §28.3 K1.4 message naming `x`.
BEZEL_TEST(maybe_consumed_reassign_displaced_drop_3967,
           if_no_else_conditional_consume_reassign_rejected_28_3_k1_4) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    if Sink.Flag(true) then t := t + Sink.Take(move x); end; "
        "    x := new Res(7); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kExpectedMsg));
}

// Row 2: the issue's own SECOND shape — a consume on a `break` path inside a
// loop is the same K7 join (maybe-consumed) as an `if` with no `else`. The
// reassignment after the (inner) loop sees the same `Owned`+`maybeConsumed`
// state.
// FAIL-WITHOUT / PASS-WITH mirrors Row 1.
BEZEL_TEST(maybe_consumed_reassign_displaced_drop_3967,
           break_path_consume_reassign_rejected_28_3_k1_4) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); "
        "    var s := 0; "
        "    var j := 0; "
        "    while j < 5 do "
        "      if j = 2 then s := Sink.Take(move x); break; end; "
        "      j := j + 1; "
        "    end; "
        "    x := new Res(2); "
        "    x.Dispose(); "
        "    return s; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kExpectedMsg));
}

// Row 3 (accept-side control, §15): `if`/`else` where BOTH arms consume `x`
// — every path moves it, so the converged state is a definite `MovedFrom`,
// not maybe-consumed. #3950 already covers this (`lhsMovedFromAtStore` skips
// the disposal entirely); this fix's new maybe-consumed gate must not fire
// for it.
BEZEL_TEST(maybe_consumed_reassign_displaced_drop_3967,
           ifelse_both_branches_consume_reassign_accepted_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    if Sink.Flag(true) then t := t + Sink.Take(move x); "
        "    else t := t + Sink.Take(move x); end; "
        "    x := new Res(7); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Row 4 (accept-side control, §15): two consume/re-acquire cycles — every
// move is unconditional (straight-line), so each reassignment sees a
// definite `MovedFrom`, never maybe-consumed.
BEZEL_TEST(maybe_consumed_reassign_displaced_drop_3967,
           reacquire_cycle_accepted_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    t := t + Sink.Take(move x); "
        "    x := new Res(6); "
        "    t := t + Sink.Take(move x); "
        "    x := new Res(7); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Row 5 — regression control: reassigning `x` INSIDE the same branch that
// consumed it (before the join) is a definite `MovedFrom` at that exact
// point, not maybe-consumed — the pre-existing #1626 "reassign after join
// clears marker" pattern (maybe_consumed_join_1626_test.cpp). This fix's new
// gate is keyed on the state AT THE REASSIGNMENT, so it must not fire here.
BEZEL_TEST(maybe_consumed_reassign_displaced_drop_3967,
           reassign_inside_consuming_branch_before_join_accepted_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    if Sink.Flag(true) then "
        "      t := t + Sink.Take(move x); "
        "      x := new Res(6); "
        "    end; "
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

// Row 6: the issue's own repro, real execution. FAIL-WITHOUT this fix: the
// program compiles rc=0 (zero ownership diagnostics) and heap-use-after-
// frees at runtime under `--sanitize=address` (measured on this branch's
// base) — so `failedAtCompileTimeWithRealLocation` is FALSE. PASS-WITH: the
// program is rejected at COMPILE time, so it is TRUE, paired with the exact
// §28.3 K1.4 message text (a compile-time helper alone is not enough when
// the pre-fix behaviour is a crash — pair it with message-text).
BEZEL_TEST(maybe_consumed_reassign_displaced_drop_3967,
           if_no_else_conditional_consume_crash_fixed_28_3_k1_4) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    if Sink.Flag(true) then t := t + Sink.Take(move x); end; "
        "    x := new Res(7); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kExpectedMsg.c_str()));
}

// Row 7: the break-path sibling, real execution. Mirrors Row 6.
BEZEL_TEST(maybe_consumed_reassign_displaced_drop_3967,
           break_path_consume_crash_fixed_28_3_k1_4) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); "
        "    var s := 0; "
        "    var j := 0; "
        "    while j < 5 do "
        "      if j = 2 then s := Sink.Take(move x); break; end; "
        "      j := j + 1; "
        "    end; "
        "    x := new Res(2); "
        "    x.Dispose(); "
        "    return s; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kExpectedMsg.c_str()));
}

// Row 8 (accept-side control, real execution, LOOP-AMPLIFIED 500x per
// [[lsan-one-shot-leak-tests-pass-vacuously]]): the if/else-both-consume
// control (Row 3's real-execution twin), alternating which arm runs across
// iterations so BOTH are exercised, asserting exact-once disposal
// (`Res.live == 0`) — proves this fix's new maybe-consumed gate does not
// itself cause a false reject OR any leak/double-free on the already-sound
// #3950 shape.
BEZEL_TEST(maybe_consumed_reassign_displaced_drop_3967,
           ifelse_both_branches_dispose_once_amplified) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var t := 0; "
        "    while i < 500 do "
        "      var x := new Res(5); "
        "      if Sink.Flag(i mod 2 = 0) then t := t + Sink.Take(move x); "
        "      else t := t + Sink.Take(move x); end; "
        "      x := new Res(7); "
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

// Row 9 (accept-side control, real execution, LOOP-AMPLIFIED 500x): the
// reacquire-cycle control (Row 4's real-execution twin) — two unconditional
// consume/re-acquire cycles per outer iteration, asserting exact-once
// disposal across all 1500 allocations (3 per iteration).
BEZEL_TEST(maybe_consumed_reassign_displaced_drop_3967,
           reacquire_cycle_disposes_once_amplified) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var t := 0; "
        "    while i < 500 do "
        "      var x := new Res(5); "
        "      t := t + Sink.Take(move x); "
        "      x := new Res(6); "
        "      t := t + Sink.Take(move x); "
        "      x := new Res(7); "
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
