// Ownership / codegen — reassigning a MOVED-FROM owned class LOCAL ran §28.7's
// drop-on-overwrite pass over a pointer whose obligation had already
// transferred out, disposing a value the new owner had already freed: a
// heap-use-after-free with NO loop, NO diagnostic, and (for two of the shapes)
// a deterministic SIGSEGV even without a sanitizer.  Issue #3950.
//
// SPEC — settled, not underspecified.  §28.7 "Reassigning owned fields and
// locals — drop-on-overwrite" (v0.294), verbatim:
//
//   "**Assignment to an owned field or owned local disposes the displaced
//    value (derives: K3.11).** The right-hand side is evaluated first; then,
//    if the previous value is live (non-null and not moved-from — the
//    per-slot tracking §28.11's indexer-set rule already defines), it is
//    disposed; then the new value is stored. ... A moved-from or null
//    location therefore stores without disposal — which is precisely how the
//    field-restoration rule's `move`-then-reassign idiom (§28.8.1) stays
//    single-disposal"
//
// and §28.3 K1, item 3 (v0.294), verbatim:
//
//   "**moved-from** — the obligation has transferred out; the binding or slot
//    is unreadable until reassigned, and discharge machinery skips it"
//
// The liveness test §28.7 states is a CONJUNCTION of two negations —
// "non-null AND not moved-from" — so BOTH have to hold before the displaced
// value may be disposed.  Codegen's local pass (`disposeDisplacedOwnedClassLocal`,
// LlvmBackend_Disposal.cpp) implemented only the NULL half: it loads the old
// slot value and null-guards it.  That is sufficient for a `using var`, an
// owned iteration variable and an `out owned` recipient — the `move` lowering
// nulls those slots (LlvmBackend_Exprs2.cpp) — but NOT for a plain `var`
// local, whose slot the `move` lowering deliberately leaves populated so the
// `new T(move n, n)` idiom can still read the second operand out of it (the
// shape three `json_550_test` rows depend on).  The MOVED-FROM half was
// therefore never supplied to codegen at all.
//
// MEASURED PRE-FIX (parent 8d9d9e0 = origin/main 2760e31 + the #3864 and
// #3931 fixes; `bezelc` built from each side, probes compiled with
// `--sanitize=address` and run with `detect_leaks=1`):
//
//   var x := new Res(5); var t := Sink.Take(move x);
//   x := new Res(7); x.Dispose();
//     -> compiles with ZERO ownership diagnostics; ASan:
//        "ERROR: AddressSanitizer: heap-use-after-free ... READ of size 8"
//        in `main`, "freed by thread T0 here: ... bz$M.Sink$Take".
//   var x := new Res(5); var y := move x; x := new Res(7); ...
//     -> SIGSEGV (exit 139) on the PLAIN build, 5 runs out of 5.
//   var x := new Res(5); Sink.Take(move x); x := owned y; ...
//     -> SIGSEGV (exit 139) on the PLAIN build, 5 runs out of 5.
//
// Seven of eight negative shapes heap-use-after-freed (straight-line, in a
// `while`, `move`-into-a-local, marked `x := owned y`, a two-cycle
// consume/re-acquire, and both with and without a trailing explicit
// `Dispose()`).  The eighth — `_ := x;` (the §17.13 discard) then reassign —
// was clean, because a discard leaves the local *explicitly-disposed*, not
// moved-from.  Every positive shape (plain reassign, dispose-then-reassign,
// reassign in a loop) was clean on both sides.
//
// THE FIX feeds the ONE authority that already computes the missing fact —
// the ownership dataflow's own converged state for the target at that
// statement — into a new `AssignmentStmt::lhsMovedFromAtStore`, the exact
// class-local sibling of `wholeValueLhsMovedFrom`, which #3263 added for this
// same §28.7 clause on move-only inline arrays.  Codegen skips the pass when
// it is set.  The flag is written only on the ownership pass's FINAL emit
// walk (`!suppressDiag_`), so it records the CONVERGED state and never an
// intermediate one — the discipline #3931 established for `checkRead`.
//
// LEAK DIRECTION.  Gating a drop can leak as easily as it can stop a
// double-drop, so this file asserts the drop still RUNS wherever it must, by
// counting destructor invocations in a static (`Counter.Dropped`) rather than
// by trusting an exit status: `reassign_still_drops_the_displaced_value`
// and its 50-iteration amplification would read 1 instead of 51 if the gate
// over-fired.  Externally the same claim was measured with LSan on the
// generated programs at 500 iterations, against a proven oracle: the same
// source compiled with the gate FORCE-SUPPRESSED reported "8000 byte(s)
// leaked in 500 allocation(s)", and clean with the real gate.
//
// NOT COVERED, filed separately: the CONDITIONAL consume (`if c then
// Sink.Take(move x); end; x := new Res();`).  That join is K1.4
// *maybe-consumed*, not moved-from — the value is live on one path and freed
// on the other — so no static gate can be right for both; it needs a per-path
// runtime bit.  It still heap-use-after-frees, unchanged by this fix.
//
// per §28.7, §28.3 K1.3, v0.294, #3950

#include <string>

#include "support/aot_support.hpp"  // runAot — the AOT oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// `Sink.Take(owned r)` never discharges its parameter, so §28.3's auto-drop
// disposes the moved-in value at the end of `Take`: the moved-FROM local back
// in `Main` is provably already freed once `Take` returns.  That is what makes
// the displaced drop a use-after-free rather than a merely redundant one.
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
    "end Sink; ";

// The counting prelude: `Res`'s destructor bumps a process-global counter, so
// a test can assert the EXACT number of disposals rather than "it did not
// crash".  The mutable primitive static draws §22.12 rule 2's declaration-site
// warning; a warning does not fail the AOT compile.
const char* kCountingPrelude =
    "module M; "
    "class Counter "
    "  public static Dropped: int := 0; "
    "end Counter; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor Counter.Dropped := Counter.Dropped + 1; end destructor; "
    "end Res; "
    "class Sink "
    "  public static method Take(owned r: Res): int return r.tag; end Take; "
    "end Sink; ";

std::string wrap(const std::string& body) {
    return std::string(kPrelude) +
           "class C "
           "  public static method Main(): int " +
           body +
           "  end Main; "
           "end C;";
}

std::string wrapCounting(const std::string& body) {
    return std::string(kCountingPrelude) +
           "class C "
           "  public static method Main(): int " +
           body +
           "  end Main; "
           "end C;";
}

}  // namespace

// ============================================================================
// NEGATIVE — the two shapes that SIGSEGV before the fix on the PLAIN build.
// These are the rows that fail without the fix on BOTH gate oracles.
// ============================================================================

// `var y := move x;` transfers to `y`; the following `x := new Res(7)` then
// ran the displaced drop over the pointer `y` now owns.  Pre-fix: exit 139
// (SIGSEGV), five runs out of five, with no sanitizer involved.
BEZEL_TEST(reassign_moved_from_local_displaced_drop_3950,
           move_into_a_local_then_reassign_the_source_28_7) {
    JitResult jr = runAot(wrap(
        "var x := new Res(5); "
        "var y := move x; "
        "x := new Res(7); "
        "var v := x.tag + y.tag; "
        "x.Dispose(); "
        "y.Dispose(); "
        "return v;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 12);  // 7 + 5
}

// The MARKED assignment transfer `x := owned y` (§28.3 K3.4 / v0.180's
// assignment-position marker) reaches the same codegen path through
// `handleMarkedOwnedAssignment`, whose own comment already stated the rule
// ("a poly-null / moved-from / disposed target holds nothing to drop") — it
// simply never told codegen.  Pre-fix: exit 139, five runs out of five.
BEZEL_TEST(reassign_moved_from_local_displaced_drop_3950,
           marked_owned_transfer_into_a_moved_from_local_28_7) {
    JitResult jr = runAot(wrap(
        "var x := new Res(5); "
        "var y := new Res(9); "
        "var t := Sink.Take(move x); "
        "x := owned y; "
        "var v := t + x.tag; "
        "x.Dispose(); "
        "return v;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 14);  // 5 + 9
}

// ============================================================================
// NEGATIVE — the shapes that compiled and ran to the RIGHT answer on the
// plain build while heap-use-after-freeing underneath.  These fail without
// the fix under the gate's forced-ASan oracle (`--full`).
// ============================================================================

// The idiomatic "consume then re-acquire in place".  This is the exact
// program #3950 was filed with, and the positive control the #3931 lane had
// to assert at the ownership level instead of running.
BEZEL_TEST(reassign_moved_from_local_displaced_drop_3950,
           consume_then_reacquire_in_place_28_7) {
    JitResult jr = runAot(wrap(
        "var x := new Res(5); "
        "var t := 0; "
        "t := t + Sink.Take(move x); "
        "x := new Res(7); "
        "x.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 5);
}

// Same, with the re-acquired value left to §28.3's scope-end auto-drop
// instead of an explicit `Dispose()` — the auto-drop is admitted here by
// `reassignedDropCovered_` (#1465), so this exercises the drop-on-overwrite
// and the auto-drop together.
BEZEL_TEST(reassign_moved_from_local_displaced_drop_3950,
           consume_then_reacquire_with_scope_end_auto_drop_28_3) {
    JitResult jr = runAot(wrap(
        "var x := new Res(5); "
        "var t := 0; "
        "t := t + Sink.Take(move x); "
        "x := new Res(7); "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 5);
}

// The loop spelling: every iteration consumes and re-acquires, so the
// moved-from state is re-established across the back edge each time.
BEZEL_TEST(reassign_moved_from_local_displaced_drop_3950,
           consume_then_reacquire_inside_a_while_body_28_7) {
    JitResult jr = runAot(wrap(
        "var x := new Res(5); "
        "var t := 0; "
        "var i := 0; "
        "while i < 3 do "
        "  t := t + Sink.Take(move x); "
        "  x := new Res(7); "
        "  i := i + 1; "
        "end; "
        "x.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 19);  // 5 + 7 + 7
}

// Two consume/re-acquire cycles in straight line — the second reassignment
// must get the gate too (the flag is per-AssignmentStmt, not per-local).
BEZEL_TEST(reassign_moved_from_local_displaced_drop_3950,
           two_consume_reacquire_cycles_in_straight_line_28_7) {
    JitResult jr = runAot(wrap(
        "var x := new Res(5); "
        "var t := 0; "
        "t := t + Sink.Take(move x); "
        "x := new Res(7); "
        "t := t + Sink.Take(move x); "
        "x := new Res(8); "
        "x.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 12);  // 5 + 7
}

// The program must still be ACCEPTED — the fix is a codegen gate, not a new
// rejection.  Pinned at the ownership level so a future over-rejection of the
// consume-then-re-acquire idiom shows up here as well as in the AOT rows.
BEZEL_TEST(reassign_moved_from_local_displaced_drop_3950,
           consume_then_reacquire_stays_accepted_28_3) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        "var x := new Res(5); "
        "var t := 0; "
        "t := t + Sink.Take(move x); "
        "x := new Res(7); "
        "x.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ============================================================================
// LEAK DIRECTION — the drop must still RUN wherever the displaced value is
// genuinely live.  These count destructor invocations, so an over-firing gate
// is caught as a wrong NUMBER, not as a missing crash.
// ============================================================================

// `x := new Res(5); x := new Res(7);` — nothing was moved, so §28.7's pass
// MUST dispose the displaced Res(5).  Two objects, two disposals.  An
// over-firing gate reads 1.
BEZEL_TEST(reassign_moved_from_local_displaced_drop_3950,
           reassign_still_drops_the_displaced_value_28_7) {
    JitResult jr = runAot(wrapCounting(
        "var x := new Res(5); "
        "x := new Res(7); "
        "var v := x.tag; "
        "x.Dispose(); "
        "return Counter.Dropped * 10 + v;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 27);  // Dropped == 2, tag == 7
}

// The same claim amplified: 50 displaced values plus the final one.  A gate
// that over-fired inside the loop body would read 1 instead of 51 — the
// in-gtest analogue of the 500-iteration LSan sweep quoted in the header.
BEZEL_TEST(reassign_moved_from_local_displaced_drop_3950,
           fifty_reassignments_drop_fifty_displaced_values_28_7) {
    JitResult jr = runAot(wrapCounting(
        "var x := new Res(1); "
        "var i := 0; "
        "while i < 50 do "
        "  x := new Res(2); "
        "  i := i + 1; "
        "end; "
        "x.Dispose(); "
        "return Counter.Dropped;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 51);  // 50 displaced + the final value
}

// The moved-from twin of the row above: 50 consume/re-acquire cycles.  Each
// value is disposed exactly once — by `Take`'s auto-drop — plus the final
// explicit `Dispose()`.  If the gate FAILED to fire, the displaced drop would
// re-dispose an already-freed object (the #3950 defect); if it over-fired at
// the wrong site, the count would fall short.
BEZEL_TEST(reassign_moved_from_local_displaced_drop_3950,
           fifty_consume_reacquire_cycles_dispose_exactly_once_each_28_7) {
    JitResult jr = runAot(wrapCounting(
        "var x := new Res(1); "
        "var t := 0; "
        "var i := 0; "
        "while i < 50 do "
        "  t := t + Sink.Take(move x); "
        "  x := new Res(1); "
        "  i := i + 1; "
        "end; "
        "x.Dispose(); "
        "return Counter.Dropped;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 51);  // 50 by Take's auto-drop + 1 explicit
}

// ============================================================================
// POSITIVE CONTROLS — shapes that were already correct before the fix and
// must be byte-for-byte unchanged by it.
// ============================================================================

BEZEL_TEST(reassign_moved_from_local_displaced_drop_3950,
           positive_plain_reassign_28_7) {
    JitResult jr = runAot(wrap(
        "var x := new Res(5); "
        "x := new Res(7); "
        "var v := x.tag; "
        "x.Dispose(); "
        "return v;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 7);
}

// The dispose twin: an EXPLICITLY-DISPOSED (not moved-from) local reassigned.
// §28.2 makes the further disposal a no-op, and the pass correctly runs.
BEZEL_TEST(reassign_moved_from_local_displaced_drop_3950,
           positive_dispose_then_reassign_28_2) {
    JitResult jr = runAot(wrap(
        "var x := new Res(5); "
        "x.Dispose(); "
        "x := new Res(7); "
        "var v := x.tag; "
        "x.Dispose(); "
        "return v;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 7);
}

// The §17.13 discard leaves the local explicitly-disposed, NOT moved-from, so
// the gate must NOT fire for it.  Measured clean on both sides.
BEZEL_TEST(reassign_moved_from_local_displaced_drop_3950,
           positive_discard_then_reassign_17_13) {
    JitResult jr = runAot(wrap(
        "var x := new Res(5); "
        "_ := x; "
        "x := new Res(7); "
        "var v := x.tag; "
        "x.Dispose(); "
        "return v;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 7);
}

BEZEL_TEST(reassign_moved_from_local_displaced_drop_3950,
           positive_reassign_in_a_loop_28_7) {
    JitResult jr = runAot(wrap(
        "var x := new Res(5); "
        "var i := 0; "
        "var t := 0; "
        "while i < 3 do "
        "  x := new Res(7); "
        "  t := t + x.tag; "
        "  i := i + 1; "
        "end; "
        "x.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 21);
}

// A borrow-initialized local is reassigned an owned value: the pass excludes
// borrow-init locals (disposing the displaced BORROW would be a double-free),
// and the moved-from gate must not disturb that exclusion.
BEZEL_TEST(reassign_moved_from_local_displaced_drop_3950,
           positive_borrow_read_of_a_reacquired_local_28_3) {
    JitResult jr = runAot(wrap(
        "var x := new Res(5); "
        "var t := Sink.Take(move x); "
        "x := new Res(7); "
        "t := t + Sink.Peek(x); "
        "x.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 12);  // 5 + 7
}
