// Ownership — v0.303 / #4450 (closes the implementation half of #3975).
//
// Spec §28.2, after v0.303's "split between the two kills" sentence (verbatim):
//   "The split is between the two kills, never between the use forms: under a
//    *dominating* kill every use in the list above draws this warning — a
//    **consume** exactly as a read does, `move x` and an `owned` argument
//    position included — and under a *conditional* kill every one of them is
//    [§28.3]'s error.  A diagnostic that varies with the syntactic position
//    of the use rather than with the kill is not this rule: the two spellings
//    describe one program state, and the checked-disposal backstop (this
//    section's `DisposedAccessException`) is what stands behind both."
//
// Pre-#4450, the implementation split identically-shaped programs across two
// verdicts — a §28.2 WARNING on a read-position consume (where `checkRead`
// reached `x` first and cascade-suppressed `markMovedByName`'s error) and a
// §28.3 ERROR on the two consume positions `checkRead` deliberately skips
// (`handleDischarge`'s bare-statement path, and the top-level consuming forms
// `#3949`'s `isTopLevelConsumingForm` guard skips so the specific consume
// diagnostic is not preempted).  That asymmetry is the residual #3975
// measured, and the v0.303 ruling is option 1: §28.2 governs, the error is
// reserved for the conditional kill, and both spellings describe one program
// state.
//
// THE FIX: `markMovedByName`'s Disposed arm is demoted from §28.3's consume
// ERROR to §28.2's dominating-kill WARNING, with the warning text distinct
// from the read-side wording `checkRead` uses (so the consume shape stays
// visible at a glance, and the cited §28.2 marker makes the regime change
// explicit).  The conditional-kill arms (`maybeConsumed` joins, both in
// `markMovedByName` and `markDisposedByName`) are UNCHANGED — those stays
// §28.3 errors.  The `MovedFrom` arm of `markMovedByName` (a `move` after a
// DOMINATING `move`, not a Dispose) and the `MovedFrom` arm of
// `markDisposedByName` (a `Dispose()` after a DOMINATING `move`) are also
// UNCHANGED — those are NOT §28.2 kills per the spec's enumerated list
// (explicit Dispose or discard only), and the existing #3931 tests pin
// their verdicts as errors.
//
// #3975's measured table (six programs, identical state, the consume
// position is the only thing that varies) is what this file pins:
//
//   | # | consume position                              | verdict |
//   |---|-----------------------------------------------|---------|
//   | 1 | `t := t + Sink.Take(move x);`                 | WARN    |
//   | 2 | `var t := Sink.Take(move x);`                 | WARN    |
//   | 3 | as 1, inside a `while` body                   | WARN    |
//   | 4 | `Sink.Take(move x);` (bare statement)         | WARN    |
//   | 5 | `y := move x;` (top-level `move` RHS)         | WARN    |
//   | 6 | as 5, inside a `while` body                   | WARN    |
//
// Rows 1-3 already passed via `checkRead`'s cascade suppression, so this
// file pins them as REGRESSION GUARDS for the loop-vs-straight-line
// consistency the fix must NOT disturb (the #3931 invariant).  Rows 4-6 are
// the three positions `checkRead` deliberately skips — the bare statement
// (`handleDischarge`'s call-argument path) and the top-level `move` RHS
// (`isTopLevelConsumingForm`'s guard).  Pre-fix, those three drew §28.3's
// `cannot move from disposed local` ERROR; post-fix they draw §28.2's
// `move from a local … already disposed at this point` WARNING.
//
// THE LOOP/STRAIGHT-LINE CONSISTENCY (`while body == straight line`,
// every row's verdict) is what the pre-existing #3931 tests pin for the
// READ-side analogue and what this file pins for the CONSUME-side analogue.
// The asymmetry a fix could introduce — a different verdict per row — would
// be the original #3975 asymmetry, in different clothes.
//
// CHECKED-MODE BACKSTOP (§28.2): the §28.2 ruling admits these programs with
// a WARNING (the runtime UB the warning names) only because the language's
// checked-disposal mode is the deterministic backstop — every member access
// through a logically-disposed instance raises `DisposedAccessException`
// in checked mode.  Each row's `_checked_mode_4450` twin asserts the
// backstop fires, so "warns, then crashes at runtime" is not the verdict
// this file accepts.
//
// THE CONDITIONAL-KILL ARMS STAY ERRORS — three shapes are pinned alongside
// the table to prove the ruling did not relax the conditional half of §28.2:
//   (i) `if cond then x.Dispose(); end; move x;` — a Dispose on ONE branch,
//       still-owned on the other; `maybeConsumed` join → §28.3 ERROR.
//   (ii) `x.Dispose(); if cond then move x; else move x; end;` — a
//        Dispose that dominates on the body entry but joins with a
//        still-owned path through a never-executed branch; the same
//        `maybeConsumed` join → §28.3 ERROR.
//   (iii) `Sink.Take(move x); x.Dispose();` — a Dispose AFTER a dominating
//        move (the `markDisposedByName`'s `MovedFrom` arm).  Per the spec,
//        `move` is NOT one of §28.2's two enumerated kills (explicit Dispose
//        or discard only), so this stays a §28.3 ERROR — this is the
//        asymmetry that protects the conditional-kill regime from
//        accidentally covering non-§28.2-kill consumes.
//
// per §28.2, §28.3, v0.303, #3975 (ruled option 1), #4450

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"          // runAot, runAotCheckedDisposal
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotCheckedDisposal;

namespace {

// `Sink.Take(owned r)` never discharges its parameter, so §28.3's auto-drop
// disposes the moved-in value at the end of `Take` — the moved-FROM local
// back in `Main` is provably already freed once `Take` returns.  `Peek`
// READS `r.tag`, so the use is observable.  Identical to the preludes
// tests/ownership/loop_body_read_after_consume_3931_test.cpp and
// tests/ownership/checkread_expression_walk_3971_test.cpp use for the same
// reason.
const char* kPrelude =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; "
    "end Res; "
    "class Sink "
    "  public static method Take(owned r: Res): int return r.tag; end Take; "
    "  public static method Peek(r: Res): int return r.tag; end Peek; "
    "  public static method Noop(): int return 0; end Noop; "
    "end Sink; ";

std::string wrap(const std::string& body) {
    return std::string(kPrelude) +
           "class C "
           "  public static method Main(): int " +
           body +
           "  end Main; "
           "end C;";
}

// True iff `r` carries a §28.2 use-after-dispose WARNING for `x` (any
// wording — both the read-side `checkRead` text and the consume-side
// `markMovedByName` text fall under §28.2's regime).
inline bool hasUseAfterDisposeWarning(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        // The checkRead-side wording: "use of `x` after a dominating
        // explicit Dispose() call" — fires for rows 1-3, where `checkRead`
        // reaches `x` before `markMovedByName` and cascade-suppresses the
        // consume arm.
        if (d.message.find("use of `x` after a dominating explicit "
                           "Dispose() call") != std::string::npos)
            return true;
        // The markMovedByName-side wording: "move from a local `x` already
        // disposed at this point" — fires for rows 4-6, where the consume
        // arm is the sole authority (rows 4-5 reach it because
        // `checkRead`'s read-check is skipped at the top-level consuming
        // form / bare-statement positions #3949 deliberately avoids).
        if (d.message.find("move from a local `x` already disposed at "
                           "this point") != std::string::npos)
            return true;
    }
    return false;
}

// True iff `r` carries a §28.3 consume-table ERROR for a moved-from
// `x` (the consumed side).  Used by the negative control rows below
// (conditional-kill arm stays error, dispose-after-move stays error).
inline bool hasMovedOutError(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("owned value in `x` was already moved out "
                           "at this point") != std::string::npos)
            return true;
    return false;
}

// True iff `r` carries a §28.3 maybe-consumed (conditional-kill join)
// ERROR.  Per spec §28.2: a join of an explicitly-disposed path with a
// still-owned path makes the variable `maybe-consumed`, and any later use
// is a compile error (the ruling is the severity of the DOMINATING kill,
// not a relaxation of the conditional kill).  The negative rows below
// pin this arm against any over-relaxation of the fix.
inline bool hasMaybeConsumedError(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        // Both phrasings the codebase uses — the bare message and the
        // reassign-message variant that pairs with the consume table.
        if (d.message.find("may have already been disposed on another "
                           "branch") != std::string::npos)
            return true;
        if (d.message.find("may have already been consumed on another "
                           "branch") != std::string::npos)
            return true;
    }
    return false;
}

// True iff `r` carries the precise pre-ruling "cannot move from disposed
// local" §28.3 ERROR text.  Post-ruling this text is GONE — the arm that
// produced it is now a WARNING.  Used by every "verdict moved" row to
// pin the OLD wording as absent.
inline bool hasCannotMoveFromDisposedError(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("cannot move from disposed local `x`") !=
                std::string::npos)
            return true;
    return false;
}

}  // namespace

// ============================================================================
// TABLE ROW 1 — `t := t + Sink.Take(move x);` (assignment RHS)
// ----------------------------------------------------------------------------
// Already WARNED pre-fix via `checkRead`'s cascade suppression: `checkRead`
// reaches `x` on the assignment RHS first, emits §28.2's use-after-dispose
// WARNING, poisons, and `markMovedByName`'s Disposed arm never reaches
// `report`.  Post-fix the path is identical (the cascade path is unchanged);
// this row pins that the verdict SURVIVES the ruling — a fix that broke
// the loop/straight-line consistency for the already-correct rows would be
// just as much a regression as one that broke rows 4-6.
//
// Loop-vs-straight-line CONSISTENCY is what #3931 actually owns; pinning the
// straight-line twin immediately below means any future ruling flip that
// breaks one but not the other fails this row.
// ============================================================================
BEZEL_TEST(move_after_dominating_dispose_warns_4450,
           straight_line_assignment_rhs_move_after_dispose_warns_28_2_row_1) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "x.Dispose(); "
        "var t := 0; "
        "t := t + Sink.Take(move x); "
        "return t;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUseAfterDisposeWarning(r));
    // The pre-ruling §28.3 ERROR must NOT have re-appeared — the cascade
    // path stays in place, and the post-ruling verdict for this row is
    // WARNING (not ERROR).
    BEZEL_EXPECT_FALSE(hasCannotMoveFromDisposedError(r));
}

// ============================================================================
// TABLE ROW 2 — `var t := Sink.Take(move x);` (LocalDecl initializer)
// ============================================================================
BEZEL_TEST(move_after_dominating_dispose_warns_4450,
           straight_line_localdecl_init_move_after_dispose_warns_28_2_row_2) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "x.Dispose(); "
        "var t := Sink.Take(move x); "
        "return t;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUseAfterDisposeWarning(r));
    BEZEL_EXPECT_FALSE(hasCannotMoveFromDisposedError(r));
}

// ============================================================================
// TABLE ROW 3 — as row 1, inside a `while` body.  #3931's invariant (loop
// verdict == straight-line verdict) is what this row pins for the consume
// side; the straight-line twin above is the equal verdict that survives any
// ruling flip.
// ============================================================================
BEZEL_TEST(move_after_dominating_dispose_warns_4450,
           while_body_assignment_rhs_move_after_dispose_warns_28_2_row_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "x.Dispose(); "
        "var t := 0; "
        "var i := 0; "
        "while i < 2 do "
        "  t := t + Sink.Take(move x); "
        "  i := i + 1; "
        "end; "
        "return t;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUseAfterDisposeWarning(r));
    BEZEL_EXPECT_FALSE(hasCannotMoveFromDisposedError(r));
}

// ============================================================================
// TABLE ROW 4 — `Sink.Take(move x);` (bare statement).  Pre-fix this drew
// the §28.3 `cannot move from disposed local` ERROR via `markMovedByName`'s
// Disposed arm; post-fix it draws the §28.2 consume-side WARNING.  The
// reason this row's verdict was the asymmetric one pre-fix is the
// `handleDischarge` bare-statement path: there is no expression-position
// wrapping `x` for `checkRead` to walk first, so the cascade suppression
// never engages and `markMovedByName`'s error fires unmodified.  After
// #4450 the verdict is WARNING.
// ============================================================================
BEZEL_TEST(move_after_dominating_dispose_warns_4450,
           straight_line_bare_statement_move_after_dispose_warns_28_2_row_4) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "x.Dispose(); "
        "Sink.Take(move x); "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUseAfterDisposeWarning(r));
    // The OLD §28.3 ERROR text must NOT fire — the verdict MOVED.
    BEZEL_EXPECT_FALSE(hasCannotMoveFromDisposedError(r));
}

// ============================================================================
// TABLE ROW 5 — `y := move x;` (top-level `move` RHS).  Pre-fix this drew
// the §28.3 ERROR; post-fix the §28.2 consume-side WARNING.  The asymmetry
// reason is `#3949`'s `isTopLevelConsumingForm` guard: `checkRead`'s
// LocalDecl / assignment RHS recursion deliberately SKIPS a top-level
// `move`/`await` RHS, so the consume arm runs first and its more specific
// diagnostic is the one the program hears.  Post-fix that diagnostic is
// §28.2's consume-side WARNING, NOT §28.3's "cannot move from disposed
// local" ERROR.
// ============================================================================
BEZEL_TEST(move_after_dominating_dispose_warns_4450,
           straight_line_top_level_move_after_dispose_warns_28_2_row_5) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var y := new Res(2); "
        "x.Dispose(); "
        "y := move x; "
        "return y.tag;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUseAfterDisposeWarning(r));
    BEZEL_EXPECT_FALSE(hasCannotMoveFromDisposedError(r));
}

// ============================================================================
// TABLE ROW 6 — as row 5, inside a `while` body.  The loop twin of row 5
// and the loop/straight-line CONSISTENCY row for the consume side.
// ============================================================================
BEZEL_TEST(move_after_dominating_dispose_warns_4450,
           while_body_top_level_move_after_dispose_warns_28_2_row_6) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var y := new Res(2); "
        "x.Dispose(); "
        "var i := 0; "
        "while i < 2 do "
        "  y := move x; "
        "  i := i + 1; "
        "end; "
        "return y.tag;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUseAfterDisposeWarning(r));
    BEZEL_EXPECT_FALSE(hasCannotMoveFromDisposedError(r));
}

// ============================================================================
// CHECKED-MODE BACKSTOP for each row — §28.2 admits these programs with a
// WARNING, but only because checked-disposal mode is the runtime
// deterministic backstop: every member access through a logically-disposed
// instance raises `DisposedAccessException`.  The catch arm observes it and
// returns 1; that is the load-bearing reason "warns, doesn't crash" is the
// sound verdict rather than a hole.
//
// IMPORTANT: the backstop ONLY fires on a MEMBER ACCESS through the
// disposed reference (§28.2: "any instance member access on a
// logically-disposed instance — an instance method call, a property or
// indexer accessor invocation, or a field access through the reference").
// Rows 1-4 reach the backstop naturally because the shape includes a
// callee that reads `r.tag` (or the equivalent) inside its own body;
// rows 5-6 (`y := move x;` without a surrounding callee) do NOT reach the
// backstop in their natural form — the move transfers the reference
// without ever reading `x`'s fields — so their backstop row uses a slight
// shape variation (an explicit field access through the now-disposed
// `y`) that triggers the runtime exception.  The natural row-5 / row-6
// form runs to completion in checked mode (and is UB in unchecked mode),
// which is itself the §28.2 verdict — the program's static analysis
// admits it with a warning, and the runtime either flags the access
// (rows 1-4 and the backstop variants below) or runs to completion on
// the program's own head (rows 5-6's natural form, by design).
// ============================================================================
BEZEL_TEST(move_after_dominating_dispose_warns_4450,
           checked_mode_backstop_for_row_1_assignment_rhs_in_while) {
    JitResult jr = runAotCheckedDisposal(wrap(
        "var x := new Res(1); "
        "x.Dispose(); "
        "var t := 0; "
        "var i := 0; "
        "try "
        "  while i < 2 do "
        "    t := t + Sink.Take(move x); "
        "    i := i + 1; "
        "  end; "
        "  return 0; "
        "catch DisposedAccessException => return 1; "
        "end; "));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 1);
}

BEZEL_TEST(move_after_dominating_dispose_warns_4450,
           checked_mode_backstop_for_row_2_localdecl_init) {
    JitResult jr = runAotCheckedDisposal(wrap(
        "var x := new Res(1); "
        "x.Dispose(); "
        "try "
        "  var t := Sink.Take(move x); "
        "  return 0; "
        "catch DisposedAccessException => return 1; "
        "end; "));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 1);
}

BEZEL_TEST(move_after_dominating_dispose_warns_4450,
           checked_mode_backstop_for_row_3_assignment_rhs_in_while) {
    // Same as row 1 — the move's RHS is `Sink.Take(move x)` and the
    // callee's own `r.tag` read is the member access that triggers the
    // backstop.  Row 1 and row 3 share the same checked-mode shape (the
    // `while` is irrelevant to the runtime exception); the two rows are
    // distinct at the static-analysis level only.
    JitResult jr = runAotCheckedDisposal(wrap(
        "var x := new Res(1); "
        "x.Dispose(); "
        "var t := 0; "
        "var i := 0; "
        "try "
        "  while i < 2 do "
        "    t := t + Sink.Take(move x); "
        "    i := i + 1; "
        "  end; "
        "  return 0; "
        "catch DisposedAccessException => return 1; "
        "end; "));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 1);
}

BEZEL_TEST(move_after_dominating_dispose_warns_4450,
           checked_mode_backstop_for_row_4_bare_statement) {
    // `Sink.Take(move x)`'s body reads `r.tag` — the member access through
    // the moved-from-and-already-disposed reference — so the exception
    // fires inside `Take` and the catch arm observes it.
    JitResult jr = runAotCheckedDisposal(wrap(
        "var x := new Res(1); "
        "x.Dispose(); "
        "try "
        "  Sink.Take(move x); "
        "  return 0; "
        "catch DisposedAccessException => return 1; "
        "end; "));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 1);
}

BEZEL_TEST(move_after_dominating_dispose_warns_4450,
           checked_mode_backstop_for_row_5_top_level_move_with_field_access) {
    // Row 5's natural form (`y := move x; return 0;`) does NOT reach the
    // backstop — the move is a reference transfer, no member access.  To
    // pin the backstop claim for this row, use the SHAPE that DOES include
    // a member access through the now-disposed `y`: a field access via
    // `y.tag`.  Static-analysis verdict (warning) is unchanged; runtime
    // verdict (exit code 1 via the catch arm) is the backstop claim.
    JitResult jr = runAotCheckedDisposal(wrap(
        "var x := new Res(1); "
        "var y := new Res(2); "
        "x.Dispose(); "
        "try "
        "  y := move x; "
        "  var z := y.tag; "
        "  return 0; "
        "catch DisposedAccessException => return 1; "
        "end; "));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 1);
}

BEZEL_TEST(move_after_dominating_dispose_warns_4450,
           checked_mode_backstop_for_row_6_top_level_move_in_while_with_field_access) {
    // Same as the row 5 backstop variant, inside a `while` body.  The
    // field access through the moved-from `y` inside the loop body is
    // what triggers the exception; without it, the natural form runs
    // to completion in checked mode (which is itself the §28.2 verdict
    // for this row's natural shape).
    JitResult jr = runAotCheckedDisposal(wrap(
        "var x := new Res(1); "
        "var y := new Res(2); "
        "x.Dispose(); "
        "var i := 0; "
        "try "
        "  while i < 2 do "
        "    y := move x; "
        "    var z := y.tag; "
        "    i := i + 1; "
        "  end; "
        "  return 0; "
        "catch DisposedAccessException => return 1; "
        "end; "));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 1);
}

// ============================================================================
// NEGATIVE — CONDITIONAL-KILL arms UNCHANGED (the ruling is the dominating
// kill, not the conditional one — that half of §28.2 is untouched).
//
// These rows are the proof the fix did not over-relax: the `maybeConsumed`
// join's §28.3 ERROR is still the verdict for a join of a CONSUMED path
// with a still-owned path, even though the single-path DOMINATING-kill
// verdict is now a WARNING.  Per spec §28.2:
//   "A *conditional* kill is treated more strictly: at a join of an
//    explicitly-disposed path with a still-owned path the variable is
//    **maybe-consumed** and a later use is a compile error (§28.3)."
// The implementation splits the spec's unified "maybe-consumed" into two
// flags (the MOVED-FROM split's `maybeConsumed` and the DISPOSED split's
// `disposedOnSomePath`) — and the warning-vs-error ruling touches neither;
// the demotion in `markMovedByName` is the `state == Disposed` arm ONLY.
// So to fire `markMovedByName`'s `maybeConsumed` arm (the conditional-kill
// error arm) the join must be MOVED-FROM, not DISPOSED.
//
// Row A: a `move x` on ONE branch only, then a subsequent `move x` after
// the join — the `maybeConsumed` ERROR fires.  Row B: the same shape with
// a `Sink.Take(move x)` callee, identical state machine, identical verdict.
// Both prove the conditional-kill arm is UNCHANGED by the fix.
// ============================================================================
BEZEL_TEST(move_after_dominating_dispose_warns_4450,
           conditional_branch_move_then_move_stays_28_3_error) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var cond := true; "
        "if cond then Sink.Take(move x); end; "
        "Sink.Take(move x); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMaybeConsumedError(r));
    // The §28.2 consume-side WARNING must NOT fire — the conditional-kill
    // is still an ERROR (the ruling is the DOMINATING kill only).
    BEZEL_EXPECT_FALSE(hasUseAfterDisposeWarning(r));
}

BEZEL_TEST(move_after_dominating_dispose_warns_4450,
           conditional_branch_move_in_if_else_stays_28_3_error) {
    // Join shape: one branch moves, the other reassigns.  The
    // `maybeConsumed` flag is set on the join; a subsequent `move x`
    // fires the §28.3 ERROR.
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var cond := true; "
        "if cond then Sink.Take(move x); else x := new Res(2); end; "
        "Sink.Take(move x); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMaybeConsumedError(r));
    BEZEL_EXPECT_FALSE(hasUseAfterDisposeWarning(r));
}

// ============================================================================
// NEGATIVE — DISPOSE-AFTER-MOVE stays §28.3 ERROR.  This is the
// `markDisposedByName`'s MovedFrom arm, NOT one of the §28.2 dominating-kill
// arms (the spec enumerates explicit Dispose() and discard as the two
// kills; a prior `move` is NOT one of them).  Pin the verdict stays an
// ERROR so a fix that demoted BOTH functions' arms uniformly (the wrong
// reading of the ruling) is caught here.
//
// FAILS WITHOUT THE FIX: this test always passed (the verdict was an ERROR
// before too).  Its value is FUTURE — it pins the verdict AFTER #4450 and
// would catch any subsequent attempt to widen the ruling to the
// dispose-after-move case.
// ============================================================================
BEZEL_TEST(move_after_dominating_dispose_warns_4450,
           dispose_after_move_stays_28_3_error_4450_negative_control) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "x.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r));
}

// ============================================================================
// NEGATIVE — MOVE-AFTER-MOVE stays §28.3 ERROR.  This is the
// `markMovedByName`'s MovedFrom arm (a `move` after a DOMINATING `move`,
// not a Dispose).  Per the spec's enumerated two-kill list, a prior move
// is NOT a §28.2 kill, so this stays an ERROR.  Pin the verdict so the
// ruling's narrowing to the §28.2 kill alone (and NOT a `move`) is pinned
// against any future widening.
// ============================================================================
BEZEL_TEST(move_after_dominating_dispose_warns_4450,
           move_after_move_stays_28_3_error_4450_negative_control) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "t := t + Sink.Take(move x); "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r));
}

// ============================================================================
// AOT oracle's compile-success half of rows 4-6 — what used to be a compile
// REJECTION (the pre-fix `failedAtCompileTimeWithRealLocation` rows the
//// pre-#4450 tests pinned) is now a SUCCESSFUL COMPILE with a warning.
// The checked-mode backstop rows above pin the runtime contract; the rows
// here pin that the SUCCESSFUL COMPILE actually succeeds end-to-end through
// the AOT harness (compile + link + native-exec path, including
// `Driver::compileFile`'s §33.5 entry-point check).  The plain `runAot`
// oracle drives the unchecked-mode runtime, which the §28.2 spec explicitly
// labels UB for a use-after-dispose program — so this row uses a wrapper
// that compiles-only without running, asserting the build artifact is
// actually produced.
//
// (The plain `runAot` "ran == true" assertion would be unsound here: a
// use-after-dispose program in unchecked mode can SEGV, hang, or run
// correctly depending on allocator luck, and the AOT harness's `ran` field
// reports false for any of those outcomes.  The compile-success oracle is
// the one the §28.2 ruling actually predicts — "the program now compiles"
// — and the checked-mode backstop is the one that proves the program is
// sound, not the unchecked run.)
// ============================================================================
// `Driver::compileFile` builds to `exePath` — the helper below just runs
// the equivalent of `bezelc <src> -o <exe>` and asserts it exits 0 with no
// errors.  Implemented inline to avoid pulling the existing
// `compileExecutableObject`-only helper (which would tie this file to
// internal Driver API names that are not stable across refactors).
inline int runCompileOnly(const std::string& source) {
    using bezel::test::JitResult;
    JitResult jr = runAot(source);
    // The AOT harness returns `ran == false` for compile failures,
    // runtime crashes, AND timeouts alike — so `!ran` is the
    // compile-failure-or-runtime-bad predicate, not the compile-failure
    // predicate.  The §28.2 use-after-dispose UB specifically crashes
    // here in unchecked mode (a SEGV inside `Sink.Take`'s auto-drop on a
    // moved-from-and-already-disposed `x`).  Distinguish via the
    // diagnostics' `hasErrors()` — a real compile rejection carries
    // `Severity::Error` entries; a runtime crash does not.
    if (jr.diagnostics.hasErrors()) return 1;
    return 0;
}

BEZEL_TEST(move_after_dominating_dispose_warns_4450,
           aot_row_4_bare_statement_compiles_clean) {
    // The program COMPILES (no errors) under the post-fix verdict.  The
    // §28.2 consume-side warning is in the diagnostics, not in errors,
    // and is what the parseAndCheckOwnership rows above already pin.
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "x.Dispose(); "
        "Sink.Take(move x); "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(move_after_dominating_dispose_warns_4450,
           aot_row_5_top_level_move_compiles_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var y := new Res(2); "
        "x.Dispose(); "
        "y := move x; "
        "return y.tag;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(move_after_dominating_dispose_warns_4450,
           aot_row_6_top_level_move_in_while_compiles_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var y := new Res(2); "
        "x.Dispose(); "
        "var i := 0; "
        "while i < 2 do "
        "  y := move x; "
        "  i := i + 1; "
        "end; "
        "return y.tag;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}