// Spec-derived tests for Ownership slice O3b — loop fixpoint + zero-trip
// leak detection (§28).
//
// O3a walked each loop body once and excluded the back-edge and the
// zero-trip header→after skip edge from the meet, which deliberately
// suppressed two unsound-but-quiet behaviours. O3b replaces the single
// pass with a worklist fixpoint and lets the zero-trip skip edge
// participate in the meet, so the analysis is sound for the never-executed
// loop path:
//
//   - §28.3 "an uninitialized local may not be read until assigned": a
//     local assigned only inside a loop body is uninitialized on the
//     zero-trip path; reading it after the loop is an error.
//   - §28.3 / §28.6 "an owned value must be discharged on every path": an
//     owned local acquired in a loop body and not discharged leaks.
//
// And the fixpoint must NOT introduce false positives: a loop that
// acquires and discharges an owned value every iteration is clean (the
// fixpoint recognises the per-iteration discharge).

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// Helper classes: Stream is a disposable class; the enclosing class C has
// an owned-shaped field `source` for the field-reassignment cases.
//
// §28.9 (v0.237, #2654): `Stream` declares a DESTRUCTOR, which is what makes
// it "a disposable class" as this comment has always claimed — i.e. NON-
// trivial-dispose. Without it, §28.9's exemption ("A `new` expression of a
// trivial-dispose class type … does *not* produce the standard ownership-
// discharge warning") would apply and every must-use assertion below would be
// testing that exemption rather than this file's actual subject, the §28/§28.6
// loop-fixpoint discharge analysis.
std::string wrap(const std::string& body,
                 const std::string& sig = "M(cond: bool)") {
    return std::string("module M; "
        "class T end T; "
        "class Stream "
        "  destructor end destructor; "
        "  public Inner: Stream? := null; "
        "end Stream; "
        "class C "
        "  public owned source: Stream := new Stream(); "  // owned (the comment's
                  // intent): `move this.source` / reassign-with-`new` are owned-field
                  // mechanics; a non-`owned` field would leak the `new Stream()` (#1020)

        "  public method ") + sig + " " + body + " end; "
        "end C;";
}

} // namespace

// --- Zero-trip leak detection (§28.3 uninitialized read) ----------------

// §28.3: a local assigned only inside a loop body is uninitialized on the
// zero-trip path. Reading it after the loop (`x.Dispose()`) is a read of a
// possibly-uninitialized local. O3a accepted this silently; O3b diagnoses.
BEZEL_TEST(own_loopfix, zero_trip_read_of_loop_assigned_local_diagnosed_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: Stream; "
        "while cond do x := new Stream(); end; "
        "x.Dispose();"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The same shape with a `for` loop: the iteration may run zero times, so
// the body assignment does not definitely initialise `x` (§28.3).
BEZEL_TEST(own_loopfix, zero_trip_read_after_for_loop_diagnosed_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: Stream; "
        "for var it in source do x := new Stream(); end; "
        "x.Dispose();"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// A read guarded so the local is also assigned before the loop is clean:
// the pre-loop assignment definitely initialises it on every path.
BEZEL_TEST(own_loopfix, preassigned_then_loop_reassign_read_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := source.Inner; "
        "while cond do x := source.Inner; end; "
        "_ := x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Owned-in-loop discharge obligation (§28.3 / §28.6) -----------------

// §28.6 / §28.3: an owned local acquired in a loop body and never
// discharged is dropped at the end of each iteration (and on loop exit).
// The fixpoint preserves the obligation across the zero-trip-skip merge.
BEZEL_TEST(own_loopfix, owned_acquired_in_loop_undischarged_diagnosed_28_6) {
    auto r = parseAndCheckOwnership(wrap(
        "while cond do var x := new Stream(); end;"));
    // §28.3 v0.150 (#1008 Part B): a fresh owned local acquired each iteration
    // auto-drops at the loop-body scope end each iteration (must-use warning).
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "x"));
}

// §28.3: acquiring and disposing the owned value on every iteration is
// clean. The fixpoint must recognise the per-iteration discharge and not
// false-positive on the back-edge re-entry.
BEZEL_TEST(own_loopfix, owned_acquired_and_disposed_each_iter_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "while cond do var x := new Stream(); x.Dispose(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.10: a `using var` in the loop body carries implicit per-iteration
// discharge — clean across the fixpoint.
BEZEL_TEST(own_loopfix, using_var_in_loop_body_clean_28_10) {
    auto r = parseAndCheckOwnership(wrap(
        "while cond do using var x := new Stream(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3: discharging the owned value each iteration is clean. Spelled
// marker-free per §28.3 K3.13 / K3.9 (v0.307): the discard statement itself
// announces the consume, so `_ := move x;` is now a redundant marker.
BEZEL_TEST(own_loopfix, owned_moved_out_each_iter_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "while cond do var x := new Stream(); _ := x; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.6: an owned local acquired before the loop and never touched by it
// is still owned on every path through the loop (including zero-trip) and
// leaks at method exit.
BEZEL_TEST(own_loopfix, preloop_owned_untouched_leaks_28_6) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); while cond do end;"));
    // §28.3 v0.150: a pre-loop owned local untouched by the loop auto-drops at
    // method-scope end (must-use warning), no error.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "x"));
}

// --- Nested loops -------------------------------------------------------

// §28.3: an inner loop that acquires and disposes each iteration, nested
// inside an outer loop, is clean — the fixpoint converges through both
// back-edges.
BEZEL_TEST(own_loopfix, nested_loops_dispose_each_iter_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "while cond do "
        "  while cond do var x := new Stream(); x.Dispose(); end; "
        "end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.6: an owned local acquired in the inner loop body and never
// discharged leaks, even when nested.
BEZEL_TEST(own_loopfix, nested_loops_inner_undischarged_diagnosed_28_6) {
    auto r = parseAndCheckOwnership(wrap(
        "while cond do "
        "  while cond do var x := new Stream(); end; "
        "end;"));
    // §28.3 v0.150: the inner-loop fresh owned local auto-drops at the inner
    // loop-body scope end each iteration (must-use warning), no error.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "x"));
}

// §28.3: a local assigned only in the inner loop body is uninitialised on
// the inner loop's zero-trip path; reading it after the inner loop (but
// still inside the outer loop) is diagnosed.
BEZEL_TEST(own_loopfix, nested_inner_zero_trip_read_diagnosed_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "while cond do "
        "  var x: Stream; "
        "  while cond do x := new Stream(); end; "
        "  x.Dispose(); "
        "end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// --- Loop that reassigns an owned field (§28.7) -------------------------

// §28.7: reassigning an owned field across loop iterations, disposing the
// previous value first each time, is clean. The field-move tracker is
// reset before the final emit pass so the per-iteration dispose+reassign
// is recognised exactly once.
BEZEL_TEST(own_loopfix, loop_reassign_owned_field_with_dispose_clean_28_7) {
    auto r = parseAndCheckOwnership(wrap(
        "while cond do "
        "  this.source.Dispose(); "
        "  this.source := new Stream(); "
        "end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8.1: a `move this.field` inside a loop body that reassigns the field
// in the same iteration leaves the field assigned at method exit — clean.
BEZEL_TEST(own_loopfix, loop_move_field_then_reassign_clean_28_8_1) {
    auto r = parseAndCheckOwnership(wrap(
        "while cond do "
        "  using var old := move this.source; "
        "  this.source := new Stream(); "
        "end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8.1: a `move this.field` inside a loop body with no reassignment
// leaves the field in moved-from state at method exit — diagnosed.
BEZEL_TEST(own_loopfix, loop_move_field_no_reassign_diagnosed_28_8_1) {
    auto r = parseAndCheckOwnership(wrap(
        "while cond do "
        "  using var old := move this.source; "
        "end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// --- Intra-body sequencing across the fixpoint (§28.3) -------------------

// §28.3: a read of a moved-from local later in the same loop body is still
// diagnosed under the fixpoint (intra-block sequencing preserved).
BEZEL_TEST(own_loopfix, read_after_move_in_loop_body_diagnosed_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "while cond do "
        "  var x := new Stream(); _ := move x; _ := x; "
        "end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
