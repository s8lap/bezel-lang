// Ownership — a read of a MOVED-FROM or explicitly-DISPOSED local reached
// around a LOOP BACK EDGE went completely undiagnosed, for `var` and `const`
// alike, where the byte-identical straight-line spelling is correctly
// rejected/warned. Issue #3931.
//
// Spec, v0.294:
//   §28.3 "Local variables, ownership, and borrows", K1 "Value
//     classification", verbatim: "3. **moved-from** — the obligation has
//     transferred out; the binding or slot is unreadable until reassigned,
//     and discharge machinery skips it".  The same list's auxiliary states
//     define "**explicitly-disposed** — the binding after a site-announced
//     kill that moves no obligation ... and a straight-line read falls under
//     [§28.2]'s use-after-dispose warning regime rather than the moved-from
//     error."
//   §28.3, K7 "The join rule", verbatim: "At every control-flow join —
//     statement merges, `??`, `if` expressions, expression-position `case`
//     arms, loop back edges — a variable's or expression's state is the
//     lattice meet, owned ⊑ borrowed"; K7.4 adds "... unreadable after the
//     join, no flow around a back edge into a use; scope-end disposal
//     per-path".
//   §28.2 "The `IDisposable` contract": "The compiler additionally **must**
//     emit a compile-time warning when a local variable is used (read,
//     borrowed from, or consumed) after an explicit `Dispose()` call ... on
//     the same variable that dominates the use site on every path".
//
// THE BUG (measured pre-fix on HEAD b6ca3d1, the #3864 fix included):
//
//   var x := new Res(1); Sink.Take(move x); Sink.Peek(x);
//     -> REJECTED: "owned value in `x` was already moved out at this point
//        (§28.3)".
//   var x := new Res(1); Sink.Take(move x);
//   var i := 0; while i < 1 do Sink.Peek(x); i := i + 1; end;
//     -> ACCEPTED, ZERO diagnostics, and a genuine heap-use-after-free:
//        AddressSanitizer reported "heap-use-after-free ... READ of size 4"
//        in `bz$M.Sink$Peek`, freed in `bz$M.Sink$Take` (which auto-drops
//        its `owned` parameter at method end, §28.3).  The `for var i in
//        1..1` spelling behaved identically.
//   The DISPOSE twin (`x.Dispose();` then the same loop) drew NOTHING where
//   the straight-line twin draws §28.2's warning, and SEGV'd at runtime.
//
// A 36-probe matrix measured 15 silently-accepted negative shapes: `while`
// and `for`, move and dispose, bare-statement / `var` / `const` / member
// read positions, nested loops, labeled loops, `continue` bodies, a `try`
// inside the body, and — because the corrupted state escapes the loop —
// a read placed AFTER the loop as well.  The shapes that stayed correctly
// diagnosed were exactly those whose reading block has NO live back edge:
// a `return` or a `break` in the body, or a move earlier in the same block.
// That signature is the mechanism.
//
// ROOT CAUSE: `checkRead`'s three plain-state arms (MovedFrom, Disposed,
// Uninitialized, in Ownership_Checks.cpp) poisoned the record as an
// UNCONDITIONAL side effect of reporting — including during the O3b
// fixpoint's diagnostic-SUPPRESSED phase.  Poisoning is a cascade
// suppressor, and a suppressed pass emits no diagnostic to cascade from,
// but it still writes Poisoned into the block's EXIT state — which the
// loop's back edge feeds into the next pass's header entry (K7: back edges
// are joins).  `joinInto` has no MovedFrom-⊓-Poisoned rule, so the header's
// meet falls through its generic "other mismatches" catch-all and collapses
// to bare Poisoned; the body block's entry on every later pass — the final,
// diagnosing one included — is then Poisoned, a state this chain
// deliberately has no arm for.  The read is silently accepted.
//
// This is the same defect shape #2439 fixed in `markMovedByName`, and the
// same one the two JOIN-FLAG arms of this very function (maybeConsumed,
// disposedOnSomePath) already guard against — the three PLAIN-state arms
// beside them were simply never swept.
//
// THE FIX: defer the poisoning of all three arms to the final emit pass
// (`if (!suppressDiag_) st->state = OwnershipState::Poisoned;`), exactly as
// the two sibling arms above them already do.  The fixpoint then converges
// on the true fact (MovedFrom ⊓ MovedFrom = MovedFrom, K7.1's same-state
// case; likewise Disposed and Uninitialized) and the final pass observes it
// fresh, reports for real, and poisons for real.
//
// ---------------------------------------------------------------------------
// LATER (fix-forward on the batch): two rows in this file pinned the §28.3
// consume ERROR for `x.Dispose(); while … move x … end;`.  Stacking #3949
// (which read-checks the assignment RHS) beneath this commit moved that
// shape's verdict to §28.2's use-after-dispose WARNING — the SPECIFIED
// verdict for a *dominating* kill, and identical to what its straight-line
// twin has always produced.  The rows were rewritten to pin the invariant
// #3931 actually owns (loop verdict == straight-line verdict) plus §28.2's
// checked-mode backstop, and a third shape was added — a top-level `move`
// RHS, the one consume position #3949 deliberately does NOT read-check —
// which used to stay a hard §28.3 error and pin the guarded arm directly.
//
// THAT THIRD SHAPE'S VERDICT MOVED, AND THE MOVE IS CORRECT (#4450, v0.303):
// §28.2's spec ruling on the dominating kill now applies to the consume
// shape too — `move x` and `owned`-argument transfers, exactly as a read.
// The `markMovedByName` Disposed arm is demoted to §28.2's WARNING, and the
// pinned verdict on the third shape is updated to match (still pinning
// loop-vs-straight-line consistency — the #3931 invariant that DOES survive
// any ruling flip — plus the new §28.2 wording).  See the rows' own
// comments for the measurements and the precise ruling citation.
//
// per §28.2, §28.3 K1.3/K7, v0.294 / v0.303, #3931, #4450
// ---------------------------------------------------------------------------

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"       // runAot — AOT oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotCheckedDisposal;

namespace {

bool diagWithSeverity(const OwnershipResult& r, bezel::Severity sev,
                      const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != sev) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// True iff the diagnostic carries a real source location — never 0:0.
bool diagHasRealLocation(const OwnershipResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(needle) == std::string::npos) continue;
        if (d.location.line != 0 && d.location.column != 0) return true;
    }
    return false;
}

// `Sink.Take(owned r)` never discharges its parameter, so §28.3's auto-drop
// disposes the moved-in value at the end of `Take` — the moved-FROM local
// back in `Main` is provably already freed once `Take` returns.  `Peek`
// READS `r.tag`, so the use is observable (that is the ASan READ).
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

const char* kMovedOut =
    "owned value in `x` was already moved out at this point (§28.3)";
const char* kUseAfterDispose =
    "use of `x` after a dominating explicit Dispose() call";
// The two CONSUME-side texts, distinct from the two READ-side texts above.
// `markDisposedByName`'s MovedFrom arm reuses the same "already moved out"
// wording `checkRead` does, which is why `kMovedOut` covers the
// dispose-after-move rows. `markMovedByName`'s Disposed arm — a `move x`
// after `x.Dispose()` — is §28.2's dominating-kill WARNING (v0.303, #4450);
// the wording below is distinct from the OLD §28.3 "cannot move from
// disposed local" error text the row before the ruling pinned, so the
// severity change (Error -> Warning) and the message change are both
// pinned by the post-ruling rows, and the OLD constant above is preserved
// for the dispose-after-move rows it has always meant.
const char* kCannotMoveDisposed =
    "cannot move from disposed local `x` (§28.3)";
const char* kMoveAfterDisposeWarn =
    "move from a local `x` already disposed at this point (§28.2)";

}  // namespace

// ============================================================================
// CONTROL — the straight-line spellings were ALREADY diagnosed before this
// fix and must stay exactly as they were.  These two anchor the asymmetry
// every negative test below measures.
// ============================================================================
BEZEL_TEST(loop_body_read_after_consume_3931,
           straight_line_read_after_move_rejected_28_3_control) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "Sink.Peek(x); "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
}

BEZEL_TEST(loop_body_read_after_consume_3931,
           straight_line_read_after_dispose_warns_28_2_control) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "x.Dispose(); "
        "Sink.Peek(x); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(
        diagWithSeverity(r, bezel::Severity::Warning, kUseAfterDispose));
}

// ============================================================================
// NEGATIVE — the SAME read, moved one line into a `while` body.
// FAILS WITHOUT THE FIX: compiles clean, zero diagnostics; the produced
// program heap-use-after-frees under ASan.
// ============================================================================
BEZEL_TEST(loop_body_read_after_consume_3931,
           while_body_read_after_move_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "var i := 0; "
        "while i < 1 do "
        "  Sink.Peek(x); "
        "  i := i + 1; "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(r, kMovedOut));
}

// The AOT oracle's half of the same fact: the program that used to compile
// and then heap-use-after-free must now not compile at all.
// FAILS WITHOUT THE FIX: `ran` is true and the process UAFs.
//
// `ran == false` alone would be ambiguous (issue 3892: it is also false for
// a compiler crash, a runtime signal, a timeout, and a forced-ASan report —
// the last of which is exactly what the pre-fix program produces under the
// gate's `--full` oracle, so a bare `!ran` row here would PASS without the
// fix for entirely the wrong reason).  Pin the REJECTION: a real
// `<path>:<line>:<col>:` location and the §28.3 text itself.
BEZEL_TEST(loop_body_read_after_consume_3931,
           while_body_read_after_move_not_compilable_aot_28_3) {
    JitResult jr = runAot(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "var i := 0; "
        "while i < 1 do "
        "  Sink.Peek(x); "
        "  i := i + 1; "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kMovedOut));
}

BEZEL_TEST(loop_body_read_after_consume_3931,
           for_range_body_read_after_move_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "for var i in 1..1 do "
        "  Sink.Peek(x); "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
}

// The read position is a `var` initializer — handleLocalDecl's checkRead
// call site, a different entry point into the same authority.
BEZEL_TEST(loop_body_read_after_consume_3931,
           while_body_var_initializer_read_after_move_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "var i := 0; "
        "while i < 1 do "
        "  var y := Sink.Peek(x); "
        "  i := i + y; "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
}

// The `const` initializer position — the #3864 call site.  #3931 is NOT a
// LocalConstDecl-vs-LocalDecl asymmetry (the `var` twin above fails the same
// way), but the const spelling must reach the diagnosis too.
BEZEL_TEST(loop_body_read_after_consume_3931,
           while_body_const_initializer_read_after_move_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "var i := 0; "
        "while i < 1 do "
        "  const y := Sink.Peek(x); "
        "  i := i + y; "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
}

// A member access on the moved-from local — checkRead's MemberAccessExpr arm.
BEZEL_TEST(loop_body_read_after_consume_3931,
           while_body_member_read_after_move_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "var i := 0; "
        "while i < 1 do "
        "  var y := x.tag; "
        "  i := i + y; "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
}

BEZEL_TEST(loop_body_read_after_consume_3931,
           nested_while_body_read_after_move_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "var i := 0; "
        "while i < 1 do "
        "  var j := 0; "
        "  while j < 1 do "
        "    Sink.Peek(x); "
        "    j := j + 1; "
        "  end; "
        "  i := i + 1; "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
}

BEZEL_TEST(loop_body_read_after_consume_3931,
           labeled_while_body_read_after_move_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "var i := 0; "
        "scan: while i < 1 do "
        "  Sink.Peek(x); "
        "  i := i + 1; "
        "end scan; "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
}

// A `continue` body still has a live back edge (`continue` jumps to the
// header), so it is in scope for the same defect.
BEZEL_TEST(loop_body_read_after_consume_3931,
           while_body_with_continue_read_after_move_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "var i := 0; "
        "while i < 1 do "
        "  i := i + 1; "
        "  Sink.Peek(x); "
        "  continue; "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
}

// A `try` inside the loop body — the CFG's exception edges did not rescue
// the fact either.
BEZEL_TEST(loop_body_read_after_consume_3931,
           try_inside_while_body_read_after_move_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "var i := 0; "
        "while i < 1 do "
        "  try "
        "    Sink.Peek(x); "
        "  catch "
        "    Exception => Sink.Noop(), "
        "  end try; "
        "  i := i + 1; "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
}

// The corrupted state ESCAPES the loop: pre-fix, a read placed entirely
// AFTER the loop — a plain straight-line use-after-move — was silenced too,
// because the loop's collapsed-to-Poisoned exit state flows on to the
// after-block.  Both reads must be diagnosed.
BEZEL_TEST(loop_body_read_after_consume_3931,
           read_after_the_loop_is_diagnosed_too_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "var i := 0; "
        "while i < 1 do "
        "  Sink.Peek(x); "
        "  i := i + 1; "
        "end; "
        "Sink.Peek(x); "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(kMovedOut) != std::string::npos)
            ++n;
    BEZEL_EXPECT_EQ(n, 2);
}

// ============================================================================
// NEGATIVE — the DISPOSE half.  §28.2 prescribes a WARNING (not an error)
// for a dominating explicit Dispose; the loop spelling must match the
// straight-line spelling's severity exactly, not over-reject.
// FAILS WITHOUT THE FIX: zero diagnostics at all; the program SEGVs.
// ============================================================================
BEZEL_TEST(loop_body_read_after_consume_3931,
           while_body_read_after_dispose_warns_28_2) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "x.Dispose(); "
        "var i := 0; "
        "while i < 1 do "
        "  Sink.Peek(x); "
        "  i := i + 1; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(
        diagWithSeverity(r, bezel::Severity::Warning, kUseAfterDispose));
    BEZEL_EXPECT_FALSE(
        diagWithSeverity(r, bezel::Severity::Error, kUseAfterDispose));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(r, kUseAfterDispose));
}

BEZEL_TEST(loop_body_read_after_consume_3931,
           for_range_body_read_after_dispose_warns_28_2) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "x.Dispose(); "
        "for var i in 1..1 do "
        "  Sink.Peek(x); "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(
        diagWithSeverity(r, bezel::Severity::Warning, kUseAfterDispose));
}

BEZEL_TEST(loop_body_read_after_consume_3931,
           while_body_member_read_after_dispose_warns_28_2) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "x.Dispose(); "
        "var i := 0; "
        "while i < 1 do "
        "  var y := x.tag; "
        "  i := i + y; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(
        diagWithSeverity(r, bezel::Severity::Warning, kUseAfterDispose));
}

// ============================================================================
// NEGATIVE — the SIBLING-ARM half of the same defect.  `checkRead` correctly
// declines to poison during the suppressed phase, and then a *consume*
// transfer function poisons the very same record in the very same statement,
// re-opening the identical back-edge collapse.  Two arms had live repros:
//
//   markDisposedByName's MovedFrom arm  — `Take(move x);` then `x.Dispose()`
//                                          inside the loop;
//   markMovedByName's   Disposed  arm  — `x.Dispose();` then `move x`
//                                          inside the loop.
//
// FAILS WITHOUT THE SIBLING GUARDS (measured on the checkRead-only commit
// 8d9d9e0): all three compile clean with ZERO diagnostics and SIGSEGV at
// runtime, while their straight-line twins are diagnosed.
//
// `kCannotDisposeMoved` / `kCannotMoveDisposed` are the two consume-side
// texts; both are §28.3 consume-table verdicts, distinct from the read-side
// texts the rows above pin.
// ============================================================================
BEZEL_TEST(loop_body_read_after_consume_3931,
           while_body_dispose_after_move_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "var i := 0; "
        "while i < 2 do "
        "  x.Dispose(); "
        "  i := i + 1; "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(r, kMovedOut));
}

// The straight-line twin, which was ALREADY rejected — the asymmetry this
// row and the one above measure between them.
BEZEL_TEST(loop_body_read_after_consume_3931,
           straight_line_dispose_after_move_rejected_28_3_control) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "x.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
}

BEZEL_TEST(loop_body_read_after_consume_3931,
           for_range_body_dispose_after_move_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "for var i in 1..2 do "
        "  x.Dispose(); "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
}

// ⚠ THE VERDICT ON THIS SHAPE MOVED, AND THE MOVE IS CORRECT.  When these
// rows were written this program drew the §28.3 consume ERROR "cannot move
// from disposed local `x`".  It now draws §28.2's use-after-dispose WARNING
// instead, because #3949 closed the assignment RHS as an unchecked read
// position: `checkRead` now reaches `x` first, emits §28.2's warning, and
// (per this file's own fix) poisons only on the emit pass, which
// cascade-suppresses `markMovedByName`'s error exactly as it always has for
// every OTHER read position.
//
// The warning is the SPECIFIED verdict, not a relaxation.  §28.2 verbatim:
// "The compiler additionally **must** emit a compile-time warning when a
// local variable is used (read, borrowed from, or consumed) after an
// explicit `Dispose()` call — or a discard of the binding — on the same
// variable that dominates the use site on every path".  A `move` is a
// consume and the `Dispose()` dominates, so a WARNING is what §28.2 asks
// for; its next sentence reserves the ERROR for the *conditional* kill ("A
// *conditional* kill is treated more strictly: at a join of an
// explicitly-disposed path with a still-owned path the variable is
// maybe-consumed and a later use is a compile error").  The old ERROR
// existed only because this one read position was never read-checked — it
// pinned a symptom of issue 3949, not a contract.
//
// What #3931 owns is not the severity but the CONSISTENCY: the loop
// spelling must agree with its straight-line twin.  That is what this row
// now measures, and it is a stronger claim than the old one — measured, it
// still fails if the sibling guards are removed, because the loop spelling
// then draws NOTHING AT ALL while the straight-line twin still warns.
//
// ⚠ v0.303 / #4450 wording note (post-ruling): `Sink.Take(move x)` is a
// top-level consuming form at the CallExpr argument position, and #4023's
// `isTopLevelConsumingForm` skip deliberately drops `checkRead`'s walk
// before it reaches the `x` identifier — so the §28.2 warning this row
// asserts is the CONSUME-SIDE wording (`kMoveAfterDisposeWarn`, produced
// by `markMovedByName`'s now-demoted Disposed arm), NOT the read-side
// `kUseAfterDispose` produced by `checkRead`.  Both rows of this
// loop-vs-straight-line twin therefore pin the consume-side wording.
BEZEL_TEST(loop_body_read_after_consume_3931,
           while_body_move_after_dispose_warns_28_2_like_straight_line) {
    const char* kBody =
        "x.Dispose(); "
        "var t := 0; "
        "var i := 0; ";
    auto loop = parseAndCheckOwnership(wrap(
        std::string("var x := new Res(1); ") + kBody +
        "while i < 2 do "
        "  t := t + Sink.Take(move x); "
        "  i := i + 1; "
        "end; "
        "return t;"));
    // The §28.2 consume-side warning fires, at a real location — the
    // post-ruling consume-side wording (produced by `markMovedByName`'s
    // demoted Disposed arm, the sole authority once #4023's skip has
    // taken `checkRead` out of the picture for this argument position).
    BEZEL_EXPECT_TRUE(
        diagWithSeverity(loop, bezel::Severity::Warning, kMoveAfterDisposeWarn));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(loop, kMoveAfterDisposeWarn));
    // ...and it is a WARNING, not an error: §28.2's dominating-kill regime.
    BEZEL_EXPECT_FALSE(loop.diagnostics.hasErrors());
    // The OLD §28.3 ERROR text must NOT fire — the ruling moved the
    // verdict off §28.3's consume-table for the dominating kill.
    BEZEL_EXPECT_FALSE(
        diagWithSeverity(loop, bezel::Severity::Error, kCannotMoveDisposed));

    // The straight-line twin — same consume, same read position, no loop —
    // must reach the identical verdict.  This equality IS the #3931
    // invariant; pinning it survives any future move of the severity.
    auto sl = parseAndCheckOwnership(wrap(
        std::string("var x := new Res(1); ") + kBody +
        "t := t + Sink.Take(move x); "
        "return t;"));
    BEZEL_EXPECT_TRUE(
        diagWithSeverity(sl, bezel::Severity::Warning, kMoveAfterDisposeWarn));
    BEZEL_EXPECT_FALSE(sl.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(
        diagWithSeverity(sl, bezel::Severity::Error, kCannotMoveDisposed));
}

// markMovedByName's MOVED-FROM arm (the third guarded sibling).  ⚠ THESE
// ROWS DO NOT DISCRIMINATE THAT GUARD — measured: they pass with the guard
// AND without it.  That arm still has NO known live repro, and this comment
// says so rather than letting the rows imply one.
//
// What they DO pin is that the verdict is now STABLE.  Un-guarded, this
// shape does not reach a fixpoint at all: the suppressed-phase poison makes
// the loop header alternate MovedFrom -> Poisoned -> (revived) MovedFrom ->
// … (the oscillation issue 2439's own comment traces in full), so the
// analysis burns all `maxPasses` iterations and its verdict is decided by
// the parity `maxPasses` happens to land on — `(post.size() + 1) * 8 + 4`,
// always even, which is the only reason the diagnostic appears today.  That
// is an accident to rest a soundness verdict on, and the guard removes it:
// with the state left alone, MovedFrom ⊓ MovedFrom = MovedFrom converges on
// pass two and the emit pass reads a genuinely stabilized fact.  (Derived
// from the code and corroborated by issue 2439's traced mechanism; the
// non-convergence itself is an analysis, not an instrumented measurement.)
BEZEL_TEST(loop_body_read_after_consume_3931,
           while_body_move_of_already_moved_from_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "var i := 0; "
        "while i < 2 do "
        "  t := t + Sink.Take(move x); "
        "  i := i + 1; "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(r, kMovedOut));
}

BEZEL_TEST(loop_body_read_after_consume_3931,
           for_range_body_move_of_already_moved_from_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "for var i in 1..2 do "
        "  t := t + Sink.Take(move x); "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
}

// Its straight-line twin, already rejected before either half of this fix.
BEZEL_TEST(loop_body_read_after_consume_3931,
           straight_line_move_of_already_moved_from_rejected_28_3_control) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "t := t + Sink.Take(move x); "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
}

// The AOT oracle's half for the two dispose/move consume shapes: what used
// to compile and SIGSEGV must now not compile.  Pinned via
// `failedAtCompileTimeWithRealLocation` + the §28.3 text rather than a bare
// `!ran`, which under the gate's forced-ASan oracle would pass WITHOUT the
// fix (a pre-fix ASan report also makes `ran` false).
BEZEL_TEST(loop_body_read_after_consume_3931,
           while_body_dispose_after_move_not_compilable_aot_28_3) {
    JitResult jr = runAot(wrap(
        "var x := new Res(1); "
        "var t := Sink.Take(move x); "
        "var i := 0; "
        "while i < 2 do "
        "  x.Dispose(); "
        "  i := i + 1; "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kMovedOut));
}

// The AOT half of the row above.  It can no longer assert non-compilation —
// §28.2 admits this program with a warning — so it asserts the thing that
// makes admitting it SOUND: the checked-disposal backstop §28.2 promises.
// "any instance member access on a logically-disposed instance ... raises
// `DisposedAccessException`"; the catch arm observes it and returns 1.
// That is a strictly stronger claim than "does not compile", and it is why
// "accepted with a warning" here is the specified behaviour rather than a
// hole.  (Measured on the batch tip: unchecked mode segfaults — §28.2's
// "undefined behavior" — and checked mode exits 1 through the arm.)
BEZEL_TEST(loop_body_read_after_consume_3931,
           while_body_move_after_dispose_raises_in_checked_mode_28_2) {
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

// ============================================================================
// NEGATIVE — the consume position where `checkRead` is DELIBERATELY skipped,
// so `markMovedByName`'s Disposed arm is the sole authority.  #3949's
// `isTopLevelConsumingForm` skips the read-check for a top-level `move`/`await`
// RHS precisely so the more specific consume diagnostic is not preempted —
// which makes this the row that pins the guarded arm directly.  The verdict
// is the §28.2 (v0.303) dominating-kill WARNING: under a Dispose() that
// dominates every path, a CONSUME on the disposed local draws §28.2's
// use-after-dispose warning, NOT §28.3's consume-table error (the ruling on
// #3975 option 1, closed by #4450).  The diagnostic text is the new
// `kMoveAfterDisposeWarn` — distinct from the read-side `kUseAfterDispose`
// so the consume shape stays visible at a glance.
//
// FAILS WITHOUT THE FIX: ACCEPTED with ZERO diagnostics, where the
// straight-line twin still warns; the unchanged guarded arm IS what makes
// this row reach the verdict at all (#3949 deliberately skips checkRead).
// ============================================================================
BEZEL_TEST(loop_body_read_after_consume_3931,
           while_body_top_level_move_after_dispose_warns_28_2_4450) {
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
    // §28.2 WARNING at a real location — the dominating-kill verdict.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        diagWithSeverity(r, bezel::Severity::Warning, kMoveAfterDisposeWarn));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(r, kMoveAfterDisposeWarn));
    // The §28.3 ERROR text must NOT fire — the ruling is the WHOLE POINT.
    BEZEL_EXPECT_FALSE(
        diagWithSeverity(r, bezel::Severity::Error, kCannotMoveDisposed));
}

// Its straight-line twin — same verdict, no loop.  The loop-vs-straight-line
// CONSISTENCY is the #3931 invariant these rows have always owned; pinning
// the equality here is what survives any future ruling flip.
BEZEL_TEST(loop_body_read_after_consume_3931,
           straight_line_top_level_move_after_dispose_warns_28_2_4450_control) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var y := new Res(2); "
        "x.Dispose(); "
        "y := move x; "
        "return y.tag;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        diagWithSeverity(r, bezel::Severity::Warning, kMoveAfterDisposeWarn));
}

// The AOT oracle's half of the row above.  The program now COMPILES (with a
// warning), so the pre-fix "rejection" oracle cannot stand.  The
// checked-disposal backstop §28.2 promises is the new soundness claim — a
// member access through the surviving borrow of a logically-disposed
// instance raises `DisposedAccessException` in checked mode.  This is the
// same backstop the row above (the move-after-dispose §28.2 warning)
// already invokes via `runAotCheckedDisposal`, mirrored here for the
// `y := move x` shape so the checked-mode claim covers both rows of the
// #3975 table.
//
// IMPORTANT: the backstop ONLY fires on a MEMBER ACCESS through the
// disposed reference (§28.2: "any instance member access on a
// logically-disposed instance — an instance method call, a property or
// indexer accessor invocation, or a field access through the reference").
// The natural `y := move x;` shape is a reference transfer with no
// member access, so the natural form runs to completion in checked mode
// (which is itself the §28.2 verdict — the program's static analysis
// admits it with a warning).  The row below uses the SHAPE that DOES
// include a member access through the now-disposed `y` (the `y.tag` read)
// so the runtime exception fires and the catch arm observes it; the
// static-analysis verdict (the §28.2 warning) is unchanged.
BEZEL_TEST(loop_body_read_after_consume_3931,
           while_body_top_level_move_after_dispose_raises_in_checked_mode_28_2_4450) {
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
// POSITIVE CONTROLS — the fix adds a rejection, so the shapes a loop makes
// LEGITIMATE must still compile AND produce the right VALUE.  Each of these
// carries a moved-from or disposed state around the same back edge; what
// makes them sound is that the state is re-established before any read.
// Asserted on the AOT oracle so the value is measured, not assumed.
// ============================================================================
BEZEL_TEST(loop_body_read_after_consume_3931,
           positive_body_local_produced_and_consumed_each_iteration) {
    JitResult jr = runAot(wrap(
        "var t := 0; "
        "var i := 0; "
        "while i < 3 do "
        "  var x := new Res(2); "
        "  t := t + Sink.Take(move x); "
        "  i := i + 1; "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 6);
}

// The outer local is MOVED-FROM at the bottom of every iteration and so
// arrives moved-from at the header — but is reassigned before the next
// read.  This is the shape a too-strict fix would wrongly reject, and it is
// the single most important false-reject axis for this change: the fix must
// not turn the loop-carried moved-from fact into a rejection of a local the
// program re-establishes.
//
// Asserted at the OWNERSHIP level, not by running it, because the shape was
// independently memory-unsafe when this branch was written: `var x := new
// Res(); Take(move x); x := new Res(); x.Dispose();` heap-use-after-freed
// under ASan with NO LOOP AT ALL (measured on b6ca3d1, the pre-fix
// compiler, and unchanged by this fix).  Running it here would have made
// this row fail under the gate's forced-ASan oracle for a reason with
// nothing to do with #3931.  What #3931 owns is the analysis verdict, and
// that is exactly what is measured: no "already moved out" rejection, and
// no error at all.
//
// The blocker is issue 3950, and it is FIXED — on branch r15-c1-l19-3950,
// which is DOWNSTREAM of this commit, so this branch alone still cannot run
// the row.  ⚠ PROMOTE THIS ROW TO A `runAot` VALUE ASSERTION once both have
// landed: that lane's `consume_then_reacquire_inside_a_while_body_28_7` is
// this exact shape and measures 19, clean under both the plain and the
// forced-ASan oracle.
//
// The cause recorded for posterity, because a wrong one sends the next
// reader to the wrong file: it was NOT a scope-end auto-drop left armed
// beside the explicit discharge.  It was the DISPLACED drop —
// `disposeDisplacedOwnedClassLocal` implemented only the *null* half of
// §28.7's liveness conjunction ("non-null AND not moved-from"), so
// reassigning a moved-from local dropped a value that had already been
// moved away.
BEZEL_TEST(loop_body_read_after_consume_3931,
           positive_outer_local_moved_then_reassigned_ownership_verdict_only) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(5); "
        "var t := 0; "
        "var i := 0; "
        "while i < 3 do "
        "  t := t + Sink.Take(move x); "
        "  x := new Res(5); "
        "  i := i + 1; "
        "end; "
        "x.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
}

BEZEL_TEST(loop_body_read_after_consume_3931,
           positive_read_before_consume_within_the_same_iteration) {
    JitResult jr = runAot(wrap(
        "var t := 0; "
        "var i := 0; "
        "while i < 2 do "
        "  var x := new Res(3); "
        "  Sink.Peek(x); "
        "  t := t + Sink.Take(move x); "
        "  i := i + 1; "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 6);
}

// The move happens in an INNER scope that ends before the back edge.
BEZEL_TEST(loop_body_read_after_consume_3931,
           positive_move_in_inner_scope_ending_before_the_back_edge) {
    JitResult jr = runAot(wrap(
        "var t := 0; "
        "var i := 0; "
        "while i < 2 do "
        "  if i >= 0 then "
        "    var x := new Res(4); "
        "    t := t + Sink.Take(move x); "
        "  end; "
        "  i := i + 1; "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 8);
}

BEZEL_TEST(loop_body_read_after_consume_3931,
           positive_for_range_body_local_fresh_each_iteration) {
    JitResult jr = runAot(wrap(
        "var t := 0; "
        "for var i in 1..3 do "
        "  var x := new Res(i); "
        "  t := t + Sink.Take(move x); "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 6);
}

// A plain borrow read of a live `using var`, repeated every iteration —
// the single most common loop shape in the language.  A fix that keyed on
// "read inside a loop" rather than on the tracked STATE would break this.
BEZEL_TEST(loop_body_read_after_consume_3931,
           positive_using_var_borrow_read_every_iteration) {
    JitResult jr = runAot(wrap(
        "using var x := new Res(7); "
        "var t := 0; "
        "var i := 0; "
        "while i < 3 do "
        "  t := t + Sink.Peek(x); "
        "  i := i + 1; "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 21);
}

BEZEL_TEST(loop_body_read_after_consume_3931,
           positive_read_in_loop_then_consume_after_the_loop) {
    JitResult jr = runAot(wrap(
        "var x := new Res(9); "
        "var t := 0; "
        "var i := 0; "
        "while i < 2 do "
        "  t := t + Sink.Peek(x); "
        "  i := i + 1; "
        "end; "
        "t := t + Sink.Take(move x); "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 27);
}

BEZEL_TEST(loop_body_read_after_consume_3931,
           positive_read_in_loop_then_dispose_after_the_loop) {
    JitResult jr = runAot(wrap(
        "var x := new Res(6); "
        "var t := 0; "
        "var i := 0; "
        "while i < 2 do "
        "  t := t + Sink.Peek(x); "
        "  i := i + 1; "
        "end; "
        "x.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 12);
}

BEZEL_TEST(loop_body_read_after_consume_3931,
           positive_nested_loops_with_a_fresh_inner_local) {
    JitResult jr = runAot(wrap(
        "var t := 0; "
        "var i := 0; "
        "while i < 2 do "
        "  var j := 0; "
        "  while j < 2 do "
        "    var x := new Res(1); "
        "    t := t + Sink.Take(move x); "
        "    j := j + 1; "
        "  end; "
        "  i := i + 1; "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 4);
}

// A consume reached only on the `break` path, plus a consume on the
// fall-through path — both discharge, neither may be rejected.
BEZEL_TEST(loop_body_read_after_consume_3931,
           positive_consume_on_a_break_path) {
    JitResult jr = runAot(wrap(
        "var t := 0; "
        "var i := 0; "
        "while i < 5 do "
        "  var x := new Res(2); "
        "  if i >= 2 then "
        "    t := t + Sink.Take(move x); "
        "    break; "
        "  end; "
        "  t := t + Sink.Take(move x); "
        "  i := i + 1; "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 6);
}

BEZEL_TEST(loop_body_read_after_consume_3931,
           positive_consume_on_a_continue_path) {
    JitResult jr = runAot(wrap(
        "var t := 0; "
        "var i := 0; "
        "while i < 4 do "
        "  i := i + 1; "
        "  var x := new Res(1); "
        "  if i = 2 then "
        "    t := t + Sink.Take(move x); "
        "    continue; "
        "  end; "
        "  t := t + Sink.Take(move x); "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 4);
}

BEZEL_TEST(loop_body_read_after_consume_3931,
           positive_try_inside_the_loop_with_a_fresh_local) {
    JitResult jr = runAot(wrap(
        "var t := 0; "
        "var i := 0; "
        "while i < 2 do "
        "  try "
        "    var x := new Res(3); "
        "    t := t + Sink.Take(move x); "
        "  catch "
        "    Exception => Sink.Noop(), "
        "  end try; "
        "  i := i + 1; "
        "end; "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 6);
}

// The DISPOSED state carried around the back edge, re-established by a
// reassignment before the next read — the dispose twin of the moved-from
// positive control above.
BEZEL_TEST(loop_body_read_after_consume_3931,
           positive_dispose_then_reassign_each_iteration) {
    JitResult jr = runAot(wrap(
        "var x := new Res(4); "
        "var t := 0; "
        "var i := 0; "
        "while i < 3 do "
        "  t := t + Sink.Peek(x); "
        "  x.Dispose(); "
        "  x := new Res(4); "
        "  i := i + 1; "
        "end; "
        "x.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 12);
}
