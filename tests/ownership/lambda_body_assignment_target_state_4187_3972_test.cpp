// Spec-derived regression tests for issues #4187 and #3972 — inside a
// LAMBDA body, `MethodOwnership::applyUsingVarBorrowPinInLambdaStmt`'s
// `AssignmentStmt` arm (fix #3951, Ownership.cpp) runs `checkRead` +
// `recordMovesIn` over the assignment's RHS only. Nothing in that arm ever
// updated `states_` for the assignment's TARGET symbol, so a reassigned
// local's PRE-assignment state (most commonly MovedFrom, from an earlier
// unconditional `move`) survived the rebind unchanged.
//
// §28.3 K1, item 3 (v0.298), verbatim: "moved-from — the obligation has
// transferred out; the binding or slot is unreadable until reassigned, and
// discharge machinery skips it." The read in both issues' repros is AFTER
// the reassignment, so it is legal and was over-rejected (#4187 — reproduces
// with a single straight-line unconditional move, NO branching required;
// #3972 — the general "reassignment does not clear the moved-from state"
// framing, filed independently and earlier).
//
// #3972's own "second consequence" section is the reason this fix must set
// `AssignmentStmt::lhsMovedFromAtStore` (#3950), not merely stop reporting
// the error: §28.7 (v0.298) "Reassigning owned fields and locals —
// drop-on-overwrite" — "assignment to an owned field or owned local disposes
// the displaced value... if the previous value is live (non-null and not
// moved-from)... A moved-from or null location therefore stores without
// disposal." Codegen's drop-on-overwrite pass (disposeDisplacedOwnedClassLocal)
// runs UNCONDITIONALLY for a covered local unless `lhsMovedFromAtStore` says
// otherwise (the flag defaults false = "run the drop"), and a PLAIN `var`'s
// slot is deliberately left non-null after a move (so `new T(move n, n)` can
// still read it, LlvmBackend_Exprs2.cpp) — so simply suppressing the
// over-reject WITHOUT setting the flag would have compiled #4187's own repro
// straight into a heap-use-after-free: the reassignment's drop-on-overwrite
// would load the stale (already-freed-by-Sink.Take) pointer from `x`'s slot,
// see it is non-null, and dispose it a second time. This is exactly the class
// of use-after-free #3950 fixed at method scope; #3972 named it directly as
// "the masked use-after-free... currently masked only by this very
// over-rejection refusing to compile the shape."
//
// The fix also had to avoid trading the over-reject for a DIFFERENT
// unsoundness: a maybe-consumed Owned local (an Owned/MovedFrom join,
// reachable inside a lambda body via #4116/#4117's if/case/while/for joins)
// has no static per-path disposal answer, and #3967 rejects reassigning one
// at method scope for exactly that reason. Rows below confirm the lambda-body
// fix reproduces that SAME rejection, with the SAME wording, rather than
// silently accepting an unsound reassignment.
//
// per §28.3 K1.3, §28.7, v0.298, #4187, #3972

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

// Destructor-counting Res, matching the family's established idiom
// (lambda_body_move_transfer_3951_test.cpp's kRes): ctor ++, dtor --, so a
// real-execution row can assert exact-once disposal via Res.live.
const char* kPreamble =
    "module M; "
    "class Res "
    "  public static live: int := 0; "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; Res.live := Res.live + 1; end constructor; "
    "  destructor Res.live := Res.live - 1; end destructor; "
    "end Res; "
    "class Sink "
    // Auto-dropped `owned` param (matches #4187's own repro literally).
    "  public static method Take(owned r: Res): int return r.tag; end Take; "
    // Explicitly-disposed `owned` param (matches #3972's own repro literally).
    "  public static method TakeDispose(owned r: Res): int "
    "    var v := r.tag; r.Dispose(); return v; "
    "  end TakeDispose; "
    "  public static method Flag(b: bool): bool return b; end Flag; "
    "end Sink; ";

}  // namespace

// ===========================================================================
// #4187 — straight-line, zero-branching false reject.
// ===========================================================================

// Row 1 (diagnostic-only): the issue's own repro verbatim. FAIL-WITHOUT the
// fix: hasErrors() true with "already moved out" at the `x.Dispose()` site
// (the target's stale MovedFrom state survives the reassignment). PASS-WITH:
// no errors.
BEZEL_TEST(lambda_body_assignment_target_state_4187_3972,
           lambda_reassign_after_unconditional_move_accepted_4187) {
    std::string src =
        std::string(kPreamble) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := Sink.Take(move x); "
        "      x := new Res(9); "
        "      x.Dispose(); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Row 2 (real execution): the same program actually compiles AND runs,
// returning `t` (Sink.Take's return, `Res(5).tag == 5`) — proving the fix is
// not merely diagnostic-suppression but a real, memory-safe accept.
// Measured on this branch's HEAD, built + probed under `--sanitize=address`:
// exit=5, no sanitizer report (no leak, no use-after-free).
BEZEL_TEST(lambda_body_assignment_target_state_4187_3972,
           lambda_reassign_after_unconditional_move_runs_correctly_4187) {
    std::string src =
        std::string(kPreamble) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var t := Sink.Take(move x); "
        "      x := new Res(9); "
        "      x.Dispose(); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ===========================================================================
// #3972 — a legal READ after the reassignment, not just a dispose.
// ===========================================================================

// Row 3 (diagnostic-only): the issue's own repro verbatim (`x.tag` read
// after reassignment). FAIL-WITHOUT: rejected at the `x.tag` read with
// "already moved out". PASS-WITH: no errors.
BEZEL_TEST(lambda_body_assignment_target_state_4187_3972,
           lambda_read_after_reassign_accepted_3972) {
    std::string src =
        std::string(kPreamble) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda() => do "
        "      var x := new Res(5); "
        "      var t := Sink.TakeDispose(move x); "
        "      x := new Res(7); "
        "      var v := x.tag + t; "
        "      x.Dispose(); "
        "      return v; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Row 4 (real execution): value-checked — v == 7 + 5 == 12, exactly as the
// issue's own text states ("compiles rc=0 and returns 12 (7 + 5)"). Measured
// on this branch's HEAD under `--sanitize=address`: exit=12, no sanitizer
// report.
BEZEL_TEST(lambda_body_assignment_target_state_4187_3972,
           lambda_read_after_reassign_returns_correct_value_3972) {
    std::string src =
        std::string(kPreamble) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda() => do "
        "      var x := new Res(5); "
        "      var t := Sink.TakeDispose(move x); "
        "      x := new Res(7); "
        "      var v := x.tag + t; "
        "      x.Dispose(); "
        "      return v; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 12);
}

// Row 5 (real execution, LOOP-AMPLIFIED 500x per
// [[lsan-one-shot-leak-tests-pass-vacuously]]): #3972's "second consequence"
// — the masked §28.7 use-after-free. Each iteration allocates a Res, moves it
// into `Sink.Take`, reassigns `x` to a FRESH Res, and disposes it. Without
// `lhsMovedFromAtStore` being set on the reassignment (the fix's other half),
// codegen's drop-on-overwrite would load `x`'s stale, already-freed slot on
// EVERY iteration and double-dispose it (500 double-frees). Asserting
// `Res.live == 0` at the end proves exact-once disposal across all 1000
// allocations (500 moved-into-Take + 500 freshly reassigned) — no leak, no
// double-free.
//
// The lambda deliberately does NOT read the outer loop counter `i` (uses a
// fixed literal instead): a captured outer local combined with a `new`
// allocation inside a loop-created lambda hits an unrelated, pre-existing
// one-time 4-byte leak (found while writing this row; filed separately —
// see the commit body / issue tracker; NOT this fix's concern — reproduces
// with no `move`, no reassignment, and no ownership diagnostics at all).
// Avoiding the capture keeps this row isolated to what it actually tests.
BEZEL_TEST(lambda_body_assignment_target_state_4187_3972,
           lambda_reassign_after_move_disposes_exactly_once_amplified) {
    std::string src =
        std::string(kPreamble) +
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var f := lambda(): int => do "
        "        var x := new Res(5); "
        "        var t := Sink.Take(move x); "
        "        x := new Res(t + 1); "
        "        x.Dispose(); "
        "        return t; "
        "      end; "
        "      _ := f(); "
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
// Soundness control (§25) — the fix must not trade the over-reject for a
// DIFFERENT unsoundness: a maybe-consumed reassignment must still reject.
// ===========================================================================

// Row 6: an if-with-no-else where the taken arm consumes `x` and the
// (implicit) not-taken arm leaves it Owned — #4116/#4117's join correctly
// produces maybe-consumed (Owned sqcap MovedFrom, K7.4) for `x` after the
// `if`. Reassigning a maybe-consumed local has no static per-path disposal
// answer (#3967, mirrored into this arm by this fix) — must still be
// REJECTED, with the reassignment-specific wording, not silently accepted.
BEZEL_TEST(lambda_body_assignment_target_state_4187_3972,
           lambda_maybe_consumed_reassign_still_rejected_control) {
    std::string src =
        std::string(kPreamble) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      if Sink.Flag(true) then "
        "        Sink.Take(move x); "
        "      end; "
        "      x := new Res(7); "
        "      x.Dispose(); "
        "      return 1; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, std::string("cannot reassign `x`: it may have already been "
                       "consumed on another branch (maybe-consumed join, "
                       "§28.3 K1.4)")));
}

// Row 7: the BYTE-IDENTICAL method-body (non-lambda) spelling of Row 6 —
// confirms the lambda-body diagnostic this fix adds is not a second,
// drifting spelling (lane-rules §6) but reproduces the REAL
// `handleAssignment`'s own wording exactly, character for character.
BEZEL_TEST(lambda_body_assignment_target_state_4187_3972,
           method_body_maybe_consumed_reassign_matches_wording_control) {
    std::string src =
        std::string(kPreamble) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    if Sink.Flag(true) then "
        "      Sink.Take(move x); "
        "    end; "
        "    x := new Res(7); "
        "    x.Dispose(); "
        "    return 1; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, std::string("cannot reassign `x`: it may have already been "
                       "consumed on another branch (maybe-consumed join, "
                       "§28.3 K1.4)")));
}

// ===========================================================================
// Accept-side controls (§15) — the fix must not false-reject or mis-dispose
// shapes that never touch MovedFrom at all.
// ===========================================================================

// Row 8 (accept-side control, real execution, LOOP-AMPLIFIED 500x): a plain
// double reassignment with NO `move` anywhere — pins that giving the target
// arm a real state-transition does not disturb the ordinary (non-move)
// drop-on-overwrite case, which was already correct before this fix (the
// state started and stayed Owned either way). Exact-once disposal across all
// 1000 allocations (500 first values + 500 second values). Uses fixed
// literals rather than the outer loop counter `i` inside the lambda — see
// Row 5's comment on the unrelated capture-triggered leak this sidesteps.
BEZEL_TEST(lambda_body_assignment_target_state_4187_3972,
           lambda_plain_double_reassign_no_move_disposes_once_amplified_control) {
    std::string src =
        std::string(kPreamble) +
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var f := lambda(): int => do "
        "        var x := new Res(5); "
        "        x := new Res(9); "
        "        x.Dispose(); "
        "        return 1; "
        "      end; "
        "      _ := f(); "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 9 (accept-side control, real execution): a COMPOUND assignment
// (`t += ...`) whose RHS moves an owned local. This fix's new target-state
// code is scoped to `as->op == ":="` ONLY — mirroring `handleAssignment`'s
// own header comment (Ownership.cpp ~line 6253): a compound assignment's
// target must NOT be reclassified by the RHS's producer state (its correct
// producer is the OPERATOR RESULT, not the RHS operand). This row pins that
// the compound-assignment path (unaffected by this fix, exactly as before)
// still compiles and runs correctly.
BEZEL_TEST(lambda_body_assignment_target_state_4187_3972,
           lambda_compound_assignment_target_unaffected_control) {
    std::string src =
        std::string(kPreamble) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var t := 0; "
        "      var x := new Res(5); "
        "      t += Sink.Take(move x); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// Row 10 — regression control: the method-body (non-lambda) spelling of
// #4187's own repro keeps working exactly as before. This fix touches only
// the lambda-body pre-pass; it must not change the method-level path (which
// already handled this shape correctly via the real `handleAssignment`).
BEZEL_TEST(lambda_body_assignment_target_state_4187_3972,
           method_body_reassign_after_move_unaffected_control) {
    std::string src =
        std::string(kPreamble) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := Sink.Take(move x); "
        "    x := new Res(9); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}
