// Spec-derived regression tests for issue #3951 — inside a lambda body, an
// `owned`-parameter transfer (`move x` handed to an `owned` sink, in
// assignment-RHS or bare-statement position) was never recorded by the
// ownership pass, because `MethodOwnership::run`'s lambda-body pre-passes
// route `LocalDecl`s through `handleLocalDecl` (`applyUsingVarBorrowPinIn-
// Lambdas`) but never routed an `AssignmentStmt` / `ExpressionStmt` through
// `handleAssignment` / `handleDischarge` — the two functions `visitStmt`
// calls for exactly those statement kinds at the method level, where the
// real dataflow DOES run.
//
// §28.3 K1.3 (v0.295, discharge machinery): "the obligation has transferred
// out; the binding or slot is unreadable until reassigned, and discharge
// machinery skips it". §28.3 K3.5 (owned-argument transfer): the callee's
// `owned` parameter takes over the disposal obligation; the caller's
// binding is left moved-from.
//
// MEASURED ON THIS BRANCH'S PARENT (before this fix): the assignment-RHS and
// bare-statement forms of `Sink.Take(move x)` inside a lambda body left `x`'s
// obligation "never discharged" for the rest of the (lambda-scoped)
// pre-pass walk, with two independent, measured consequences:
//
//   1. A SPURIOUS §28.3 auto-drop WARNING at `x`'s declaration — `x` IS
//      properly discharged (moved into `Sink.Take`'s `owned` parameter), but
//      `everDischarged_` (the set the end-of-scope auto-drop loop consults,
//      Ownership.cpp ~1131) never gained `x`'s symbol, so the loop treated it
//      as a never-moved genuine owner. Rows lambda_single_consume_assign_rhs_
//      no_spurious_autodrop_warning / lambda_single_consume_bare_stmt_no_
//      spurious_autodrop_warning pin the ABSENCE of that warning.
//
//   2. A SECOND `move x` of the SAME already-moved-from local inside the
//      lambda body was silently ACCEPTED (`states_` never recorded the first
//      move), passing the first move's already-nulled slot as a second
//      argument to `Sink.Take`'s non-nullable `owned r: Res` parameter — a
//      null-pointer read inside the callee. Measured: rc=0, zero ownership
//      diagnostics, SIGSEGV (exit 139) on the plain build and an
//      AddressSanitizer SEGV (null-pointer READ) in `bz$M.Sink$Take` under
//      `--sanitize=address`. The byte-identical METHOD-BODY spelling of the
//      same double-move has been rejected since #3949 ("owned value in `x`
//      was already moved out at this point (§28.3)"). Rows
//      lambda_double_consume_assign_rhs_rejected_28_3 /
//      lambda_double_consume_bare_stmt_rejected_28_3 (diagnostic-only) and
//      lambda_double_consume_bare_stmt_crash_fixed_28_3 (the real runAot-
//      checked crash reproduction) pin this.
//
// ACCEPT-SIDE CONTROLS (§15 — a fix that adds a rejection must not
// false-reject working code): the walker that records the move inside a
// lambda body has no real CFG join, so recording a move made in one
// mutually-exclusive branch (an `if`/`else` arm) or possibly-zero-times loop
// body must NOT leak into a sibling branch or past the loop as a false
// "already moved". Rows lambda_if_else_both_branches_consume_accepted_28_3
// and lambda_while_loop_fresh_local_each_iteration_accepted pin the accept
// side, both via the diagnostic-only harness and (loop-amplified 500x,
// [[lsan-one-shot-leak-tests-pass-vacuously]]) via the real runAotChecked
// execution path, asserting `Res.live == 0` — exact-once disposal, no leak,
// no double-free.
//
// per §28.3 K1.3, K3.5, v0.295

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "support/Diagnostic.hpp"
#include "support/ownership_support.hpp"   // parseAndCheckOwnership, hasErrorMessage, hasAutoDropWarning
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::hasAutoDropWarning;
using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A destructor-counter Res, matching the family's established idiom
// (lambda_return_move_own_local_double_free_4049_test.cpp): ctor ++, dtor --,
// surfaced through Res.live so an ACCEPT test can assert exact-once
// disposal.
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

}  // namespace

// ===========================================================================
// Diagnostic-only rows (parseAndCheckOwnership) — the spurious-warning half
// and the missing-rejection half, both independent of codegen/runtime.
// ===========================================================================

// Row 1: assignment-RHS single consume — `t := t + Sink.Take(move x);`. `x`
// is properly discharged; must carry NO §28.3 auto-drop warning.
// FAIL-WITHOUT the fix: hasAutoDropWarning(r, "x") is true (the spurious
// warning fires); PASS-WITH: false.
BEZEL_TEST(lambda_body_move_transfer_3951,
           lambda_single_consume_assign_rhs_no_spurious_autodrop_warning) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var t := 0; "
        "      var x := new Res(5); "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "x"));
}

// Row 2: bare-statement single consume — `Sink.Take(move x);` as its own
// statement. Same discharge; same absence-of-warning pin.
// FAIL-WITHOUT / PASS-WITH mirrors Row 1.
BEZEL_TEST(lambda_body_move_transfer_3951,
           lambda_single_consume_bare_stmt_no_spurious_autodrop_warning) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      Sink.Take(move x); "
        "      return 1; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "x"));
}

// Row 3: assignment-RHS double consume — `x` moved twice via two
// `Sink.Take(move x)` calls each used as an assignment RHS. Must be
// REJECTED with the same §28.3 wording the method-body spelling gets since
// #3949.
// FAIL-WITHOUT the fix: hasErrors() is false (silently accepted); PASS-WITH:
// true, with the exact "already moved out" wording.
BEZEL_TEST(lambda_body_move_transfer_3951,
           lambda_double_consume_assign_rhs_rejected_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var t := 0; "
        "      var x := new Res(5); "
        "      t := t + Sink.Take(move x); "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, std::string("owned value in `x` was already moved out at this "
                       "point (§28.3)")));
}

// Row 4: bare-statement double consume — the same double-move, but both
// consumes are bare `ExpressionStmt`s (`Sink.Take(move x); Sink.Take(move
// x);`) rather than assignment RHS. This is `handleDischarge`'s own
// statement form, the sibling gap to Row 3's `handleAssignment` gap.
// FAIL-WITHOUT / PASS-WITH mirrors Row 3.
BEZEL_TEST(lambda_body_move_transfer_3951,
           lambda_double_consume_bare_stmt_rejected_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      Sink.Take(move x); "
        "      Sink.Take(move x); "
        "      return 1; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, std::string("owned value in `x` was already moved out at this "
                       "point (§28.3)")));
}

// Row 5 (accept-side control, §15): `if`/`else` where EACH arm independently
// (and legally) consumes `x` via a bare-statement `owned`-transfer — the
// arms are mutually exclusive, so this must compile clean with NO error and
// NO spurious auto-drop warning. Pins that the save/restore added around
// IfStmt processing (so a move recorded in one arm does not leak into its
// sibling) does not itself introduce a false reject.
BEZEL_TEST(lambda_body_move_transfer_3951,
           lambda_if_else_both_branches_consume_accepted_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(b: bool): int => do "
        "      var x := new Res(5); "
        "      if Sink.Flag(b) then "
        "        Sink.Take(move x); "
        "      else "
        "        Sink.Take(move x); "
        "      end; "
        "      return 1; "
        "    end; "
        "    return f(true); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "x"));
}

// Row 6 (accept-side control, §15): a `while` loop declaring a FRESH owned
// local each iteration and consuming it via a bare-statement transfer. The
// walker cannot know how many times the loop body runs, so this pins that
// the save/restore added around WhileStmt processing does not turn a legal
// per-iteration consume into a false "already moved" (there is no scope
// aliasing across iterations here — `x` is redeclared each time — but the
// walker's OWN `states_` map is keyed by symbol and shared across the single
// structural visit of the body, so this exercises the same code path).
BEZEL_TEST(lambda_body_move_transfer_3951,
           lambda_while_loop_fresh_local_each_iteration_accepted) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var i := 0; "
        "      var s := 0; "
        "      while i < 5 do "
        "        var x := new Res(i); "
        "        s := s + Sink.Take(move x); "
        "        i := i + 1; "
        "      end; "
        "      return s; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "x"));
}

// ===========================================================================
// Real-execution rows (runAot / runAotChecked) — the actual crash
// reproduction and its fix, plus loop-amplified accept-side memory safety.
// ===========================================================================

// Row 7: the double-move's REAL runtime consequence. FAIL-WITHOUT this fix:
// the program compiles rc=0 (zero ownership diagnostics) and then crashes at
// runtime — measured plain-build SIGSEGV (exit 139) and, under
// `--sanitize=address`, an ASan SEGV (null-pointer read) inside
// `bz$M.Sink$Take` — so `runAotChecked` returns `ran == true` with a nonzero/
// crash outcome and `failedAtCompileTimeWithRealLocation` is FALSE. PASS-WITH
// the fix: the program is rejected at COMPILE time (the double-move is
// caught before codegen ever runs), so `failedAtCompileTimeWithRealLocation`
// is true, paired with the exact §28.3 message text (lane rule: a
// compile-time helper alone is not enough when the pre-fix behaviour is a
// crash — pair it with message-text).
BEZEL_TEST(lambda_body_move_transfer_3951,
           lambda_double_consume_bare_stmt_crash_fixed_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      Sink.Take(move x); "
        "      Sink.Take(move x); "
        "      return 1; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "owned value in `x` was already moved out at this point (§28.3)"));
}

// Row 8 (accept-side control, real execution, LOOP-AMPLIFIED 500x per
// [[lsan-one-shot-leak-tests-pass-vacuously]]): the single-consume
// bare-statement form, called 500 times. A single call was already
// memory-safe before this fix (the call-argument `move` lowering nulls the
// source slot, so the pre-existing spurious auto-drop's null-guarded
// scope-end dispose was already a no-op) — this row is a control that PASSES
// on BOTH sides of the fix; its purpose is to pin that this fix's addition
// of real move-tracking does not turn a previously-safe, previously-silent
// program into a rejected one, and to give the diagnostic-only Row 2 above a
// real-execution twin.
BEZEL_TEST(lambda_body_move_transfer_3951,
           lambda_single_consume_bare_stmt_disposes_once_amplified) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var f := lambda(): int => do "
        "        var x := new Res(5); "
        "        Sink.Take(move x); "
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

// Row 9 (accept-side control, real execution, LOOP-AMPLIFIED 500x): the
// if/else-both-consume control (Row 5's real-execution twin), alternating
// which arm runs across iterations so BOTH are exercised, asserting exact-
// once disposal (`Res.live == 0`) — proves the save/restore added around
// IfStmt processing (this fix) causes no leak and no double-free either.
BEZEL_TEST(lambda_body_move_transfer_3951,
           lambda_if_else_both_branches_dispose_once_amplified) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var f := lambda(b: bool): int => do "
        "        var x := new Res(5); "
        "        if Sink.Flag(b) then "
        "          Sink.Take(move x); "
        "        else "
        "          Sink.Take(move x); "
        "        end; "
        "        return 1; "
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

// Row 10 (accept-side control, real execution, LOOP-AMPLIFIED 500x): the
// while-loop-fresh-local control (Row 6's real-execution twin) — 5
// consumes per outer iteration, 500 outer iterations, asserting exact-once
// disposal across all 2500 allocations.
BEZEL_TEST(lambda_body_move_transfer_3951,
           lambda_while_loop_fresh_local_disposes_once_amplified) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var outer := 0; "
        "    while outer < 500 do "
        "      var f := lambda(): int => do "
        "        var i := 0; "
        "        var s := 0; "
        "        while i < 5 do "
        "          var x := new Res(i); "
        "          s := s + Sink.Take(move x); "
        "          i := i + 1; "
        "        end; "
        "        return s; "
        "      end; "
        "      _ := f(); "
        "      outer := outer + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 11 — regression control: the method-body (non-lambda) spelling of the
// double-move keeps its pre-existing #3949 rejection. This fix touches only
// the lambda-body pre-passes; it must not change the method-level path.
BEZEL_TEST(lambda_body_move_transfer_3951,
           method_body_double_consume_still_rejected_28_3_control) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var t := 0; "
        "    var x := new Res(5); "
        "    t := t + Sink.Take(move x); "
        "    t := t + Sink.Take(move x); "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, std::string("owned value in `x` was already moved out at this "
                       "point (§28.3)")));
}
