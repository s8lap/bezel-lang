// Spec-derived regression tests for issue #4247 — a plain-identifier borrow
// holder (`var y := x;`) REBOUND to a fresh owned value and explicitly
// `.Dispose()`d on ONE branch of an `if`-WITHOUT-`else` (or any other
// construct whose join is not a straight line — `if`/`else` with only one
// disposing arm, a `case` arm, a loop back-edge, a nested `if`), then
// touched again after the join, compiled with ZERO ownership diagnostics.
//
// `OwnershipState.hpp`'s own header comment documents the lattice as
// "intentionally NON-COMPLETE — the branch-join operation rejects mismatched
// merges instead of widening to the weaker state" — a Borrowed/Disposed join
// is exactly such a mismatch, yet `MethodOwnership::joinInto`
// (Ownership_Checks.cpp) had no arm for it: it fell through every named
// case (same-state, PolyNull, loop-Uninitialized, `ownedBorrowedClash`,
// `ownedDischargedSplit` — all of which require at least one side to be
// `Owned`) to the generic "other mismatches" catch-all, which sets
// `OwnershipState::Poisoned` with NO diagnostic. `Poisoned`'s documented
// contract is "a prior error referenced this local; suppress cascades" —
// here nothing was ever reported, so poisoning silently disabled every
// later check on the local (`checkRead`, `checkDisposeReceiverOwnership`'s
// `disposeAliasSource` alias-dispose rejection, `checkConsumeFreeze`) for
// the rest of the method.
//
// MEASURED ROOT CAUSE, NOT MERE HYPOTHESIS (this lane, C3-L15, base
// `dafe9be` = r18-c3 cycle-3 tip, `git log origin/main --grep="#4247"`
// showed only the ORIGINAL filing commit `b68ec16` (fix #4225) mentioning
// the issue, not a fix — confirmed via `git show -s --format=%B` on the full
// hash; nothing on `origin/main` resolves it):
//
//   module M;
//   class Res
//     public tag: int;
//     public constructor(t: int) this.tag := t; end constructor;
//     destructor end destructor;
//   end Res;
//   class C
//     public static method Main(cond: bool): int
//       var x := new Res(5);
//       var y := x;
//       if cond then
//         y := new Res(1);
//         y.Dispose();
//       end;
//       x := new Res(9);
//       y.Dispose();
//       x.Dispose();
//       return 0;
//     end Main;
//   end C;
//
// compiles rc=0, ZERO diagnostics, on the unfixed tree (`build/bezelc -c`).
//
// WHICH IT ACTUALLY IS — leak, double-dispose, or something else — DETERMINED
// BY MEASUREMENT (the issue's own title says "leak, possible double-dispose"
// and explicitly declines to investigate further):
//
//   * The DOUBLE `.Dispose()` half (`y.Dispose()` inside the `if`, then
//     `y.Dispose()` again after the join, unconditionally) is measured
//     HARMLESS at runtime, even at 200x loop amplification
//     (`scripts/agent_check.sh --probe --asan`, exit=0, no ASan/LSan report):
//     `.Dispose()` is idempotent at runtime via the checked-mode disposed-flag
//     backstop (§28.2), so a repeat call on the SAME already-disposed pointer
//     is a safe no-op. The issue's "possible double-dispose" framing does not
//     hold up under measurement — it is not a crash on the disposed arm.
//   * The single-shot (unamplified) READ variant (`var t := y.tag;` in place
//     of the second `.Dispose()`) is the issue's OWN "Measured runtime
//     behaviour (single-shot...)" repro, which its filing reported as a
//     "16 byte(s) leaked in 1 object(s)" LSan report and nothing else.
//     RE-MEASURED (send-back review of this commit, then independently
//     re-measured a second time by the C3-4247-fixback lane): the issue's
//     verbatim single-shot repro run 20 times, isolated, against the
//     PARENT commit's rebuilt binary — **20 of 20 deterministic SEGV**, not
//     one leak:
//       AddressSanitizer: SEGV on unknown address 0x00000000000c
//       ==...==The signal is caused by a READ memory access.
//       #0 ... in main
//     Amplifying the SAME shape to 200 loop iterations
//     (`repro2b_leak_amplified.bzl`, this lane's scratch) reproduces the
//     identical signature (same near-null address, same READ access, same
//     frame). So the single-shot repro is itself a deterministic crash on
//     this tree/hardware, not merely a leak, and NOT an allocator-timing
//     coincidence revealed only by amplification — amplification was never
//     needed to see it here. The near-null address and total determinism
//     point at a read through an explicitly nulled disposed-flag slot,
//     which is consistent with a crash on every run rather than reuse of a
//     freed block on only *some* runs. The issue's own one-shot "leaked"
//     report is a plausible outcome elsewhere (LSan's conservative stack
//     scan is real, §30 of this run's LANE_RULES), but it is not what this
//     lane's or this send-back's repeated measurement produced — the two
//     disagree, and that disagreement is itself worth recording rather than
//     papering over with an "allocator timing" story neither measurement
//     actually observed. Either way, the correct characterization is: **a
//     real use-after-dispose read crash** — not a double-dispose, and not
//     reliably "just" a leak.
//
// THE FIX, and why REJECTION (not "make it dispose correctly on the
// not-taken path") is the right response: a program that never touches the
// merged local again after the join (§15 accept-side control) is completely
// sound — the disposal on the taken arm discharged a value `y` itself came
// to own via the rebind, and the not-taken arm's borrow of `x` remains `x`'s
// own obligation, not `y`'s — so REJECTING AT THE JOIN unconditionally (like
// `ownedBorrowedClash` does for Owned-vs-Borrowed) would false-reject that
// legitimate program. Mirroring `ownedDischargedSplit`'s existing Owned ⊓
// Disposed/MovedFrom treatment one field over: `joinInto` gets a new
// `borrowedDischargedSplit` arm that KEEPS the Borrowed side's record (so its
// `source`/`disposeAliasSource` survive the join) and sets
// `disposedOnSomePath` — mirrored on the SAME same-state branch #4236 already
// widened for `disposeAliasSource` (an OR-merge was missing for
// `disposedOnSomePath` there too, needed for the nested-if / loop-header
// axis cells below to propagate the flag correctly). `checkRead`'s existing
// `disposedOnSomePath` consumer (previously gated to `state == Owned` only,
// because that was the only state it was ever populated on) is widened to
// also fire on `state == Borrowed` — the flag's MEANING ("a read here may
// observe freed memory on some incoming path") is unchanged, only the set of
// states it is populated on. A repeat `.Dispose()` needs no new handling: it
// is independently caught by the PRE-EXISTING `disposeAliasSource`
// alias-of-`x` rejection (`checkDisposeReceiverOwnership`, fix #4236) now
// that the join stops discarding that record.
//
// THE MERGE OPERATOR, stated as the lattice argument the brief asked for:
// the new/widened `disposedOnSomePath` fact is OR-merged, both where
// `borrowedDischargedSplit` first produces it (an incoming Disposed arm sets
// it regardless of what the OTHER arm's Borrowed record already carried) and
// on the same-state branch (`st.disposedOnSomePath = st.disposedOnSomePath ||
// o.disposedOnSomePath`, mirroring `disposeAliasSource`'s existing OR).
// The unsafe direction is "disposed on AT LEAST ONE reachable path, and the
// merge silently drops that fact" — a READ after the join can land on the
// disposed-branch path at runtime on ANY execution where that branch was the
// one taken, so if EITHER incoming edge carries the danger, the merged local
// must carry it forward too; losing it on any path (which an AND-merge, or
// "keep only one side", would do) lets a read through that may observe freed
// memory. This is the SAME direction `disposeAliasSource`'s own OR-merge
// already uses (#4236) and the OPPOSITE of `explicitDisposeReclaimedSlot`'s
// AND-merge (#4246) — that flag's unsafe direction is "reclaim-eligible on
// some but not every path", the exact opposite question.
//
// §28.3 "Local variables, ownership, and borrows" (the branch-join /
// conditional-discharge rules), §28.2 (`.Dispose()` idempotence), §28.6
// ("Who may call Dispose()"). v0.298. #4247.
//
// Files touched: src/ownership/OwnershipState.hpp (`disposedOnSomePath`
// field comment), src/ownership/Ownership_Checks.cpp (`joinInto`'s
// same-state branch 8606-8625 and its new `borrowedDischargedSplit` arm
// 8812-8889 in the fixed tree; `checkRead`'s `disposedOnSomePath` consumer
// 2582-2607). Re-measured directly against the fixed tree by the
// C3-4247-fixback send-back lane — the original commit's `~8590-8610` /
// `~8797-8876` citations were off by ~15-35 lines (the `~8797-8876` range
// ended BEFORE the `const bool borrowedDischargedSplit = ...` declaration
// it was meant to cover); do not re-copy these without re-measuring, since
// any further edit to this file shifts everything below it again.

#include <string>

#include "support/aot_support.hpp"  // runAot / runAotChecked — the AOT oracle
#include "support/aot_result_asserts.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// The join-mismatch diagnostic text this fix's `checkRead` widening
// produces — the stable substring every reject row below asserts.
const char* kJoinMismatchNeedle =
    "may have already been disposed on another branch and may not be read "
    "here";

// The pre-existing #4236 alias-dispose diagnostic, which now ALSO fires on
// the repeat-`.Dispose()` variant once the join stops discarding
// `disposeAliasSource` for the Borrowed side.
const char* kAliasDisposeNeedle = "it is a borrow (an alias) of";

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
    "class C "
    "  public static method Main(): int ";

const char* kEpilogue =
    "  end Main; "
    "end C; ";

std::string wrapCounting(const std::string& body) {
    return std::string(kCountingPrelude) + body + kEpilogue;
}

}  // namespace

// ============================================================================
// PRIMARY REGRESSION — the issue's own two repro shapes (double-dispose and
// single-read), each in a `bool`-parameterized `Main` (matching the issue's
// literal text) checked via the ownership pass directly (no execution
// needed — both are compile-time rejections).
// ============================================================================

BEZEL_TEST(borrowed_disposed_branch_join_4247,
           issue_repro_double_dispose_variant_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(cond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    if cond then "
        "      y := new Res(1); "
        "      y.Dispose(); "
        "    end; "
        "    x := new Res(9); "
        "    y.Dispose(); "
        "    x.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // Both the join-mismatch read check (the repeat Dispose() IS a read of
    // the receiver first) and the pre-existing #4236 alias-dispose rejection
    // fire; either alone proves the fix, so assert both are present.
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kJoinMismatchNeedle));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kAliasDisposeNeedle));
}

BEZEL_TEST(borrowed_disposed_branch_join_4247,
           issue_repro_read_after_join_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(cond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    if cond then "
        "      y := new Res(1); "
        "      y.Dispose(); "
        "    end; "
        "    x := new Res(9); "
        "    var t := y.tag; "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kJoinMismatchNeedle));
}

// ============================================================================
// AXIS GRID (§ per the brief) — which merge shapes reach the mismatched
// Borrowed/Disposed join. Each row is a minimal isolate: `x.Dispose()`
// instead of a reassignment, so ONLY the join-mismatch diagnostic this fix
// adds is exercised (not the separate, pre-existing #4225 consume-freeze
// diagnostic a reassignment of `x` also triggers — see the primary rows
// above for that combined shape).
// ============================================================================

// if-WITHOUT-else — the issue's own core shape, isolated.
BEZEL_TEST(borrowed_disposed_branch_join_4247, axis_if_without_else_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(cond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    if cond then "
        "      y := new Res(1); "
        "      y.Dispose(); "
        "    end; "
        "    var t := y.tag; "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kJoinMismatchNeedle));
}

// if/else where only ONE arm disposes (the `else` arm is a no-op, not an
// absent `else` — structurally the same join, spelled differently).
BEZEL_TEST(borrowed_disposed_branch_join_4247,
           axis_if_else_only_one_arm_disposes_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(cond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    if cond then "
        "      y := new Res(1); "
        "      y.Dispose(); "
        "    else "
        "      var noop := 0; "
        "    end; "
        "    var t := y.tag; "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kJoinMismatchNeedle));
}

// A `case` arm join — §19/§28.8's arm-to-arm merge routes through the same
// `joinInto` machinery (confirmed by issue #4116's own fix, this file's
// sibling `case_stmt_maybe_consumed_join_4116_test.cpp`).
BEZEL_TEST(borrowed_disposed_branch_join_4247, axis_case_arm_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(cond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    case cond of "
        "      true => do "
        "        y := new Res(1); "
        "        y.Dispose(); "
        "      end, "
        "      _ => () "
        "    end case; "
        "    var t := y.tag; "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kJoinMismatchNeedle));
}

// A loop-carried (back-edge, `unionKeys` in `joinInto`) merge — the rebind-
// and-dispose is conditional INSIDE the loop body, so the loop header's own
// entry/exit meet is the mismatched join, not an `if`-join directly.
BEZEL_TEST(borrowed_disposed_branch_join_4247,
           axis_loop_header_back_edge_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    var i := 0; "
        "    while i < 3 do "
        "      if i = 1 then "
        "        y := new Res(1); "
        "        y.Dispose(); "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    var t := y.tag; "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kJoinMismatchNeedle));
}

// A nested `if` — the OUTER join sees Borrowed (outer not-taken arm) merged
// against Borrowed+disposedOnSomePath (outer taken arm, produced by the
// INNER if's own borrowedDischargedSplit). This is the row that pins the
// same-state OR-merge addition: without it, the outer join's early
// `st.state == o.state` branch would silently drop the flag one level up.
BEZEL_TEST(borrowed_disposed_branch_join_4247, axis_nested_if_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(outerCond: bool, innerCond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    if outerCond then "
        "      if innerCond then "
        "        y := new Res(1); "
        "        y.Dispose(); "
        "      end; "
        "    end; "
        "    var t := y.tag; "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kJoinMismatchNeedle));
}

// ============================================================================
// ACCEPT-SIDE CONTROLS (§15) — the expensive direction. Every row below must
// keep compiling AND (where it executes) run ASan/LSan-clean, because this
// fix widens a rejection.
// ============================================================================

// A1 — if-without-else, disposes on the taken path, never touches the
// holder again. The ONLY diagnostic admissible is the pre-existing,
// unrelated §28.3 auto-drop WARNING for `x` itself never being disposed —
// nothing about `y`.
BEZEL_TEST(borrowed_disposed_branch_join_4247,
           accept_if_without_else_no_further_touch_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(cond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    if cond then "
        "      y := new Res(1); "
        "      y.Dispose(); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A2 — same shape, but `x` IS explicitly disposed afterward (still never
// touching `y`). Executes under the strict AOT oracle, loop-amplified 100x,
// alternating `cond` every iteration so both arms run ~50 times each.
BEZEL_TEST(borrowed_disposed_branch_join_4247,
           accept_dispose_x_after_join_no_y_touch_asan_100x_28_3) {
    JitResult jr = runAotChecked(wrapCounting(
        "var i := 0; "
        "while i < 100 do "
        "  var cond := (i mod 2 = 0); "
        "  var x := new Res(5); "
        "  var y := x; "
        "  if cond then "
        "    y := new Res(1); "
        "    y.Dispose(); "
        "  end; "
        "  x.Dispose(); "
        "  i := i + 1; "
        "end; "
        "return Counter.Dropped;"));
    if (wasSkipped(jr)) return;
    BEZEL_EXPECT_TRUE(jr.ran);
    // 100 iterations: 50 taken-arm iterations dispose 2 objects each
    // (Res(1) + Res(5)), 50 not-taken-arm iterations dispose 1 (Res(5) via
    // `x.Dispose()` only) = 50*2 + 50*1 = 150.
    BEZEL_EXPECT_EQ(jr.exitCode, 150);
}

// A3 — both arms rebind-and-dispose (same-state Disposed⊓Disposed join —
// does not reach `borrowedDischargedSplit` at all; a control that this fix
// leaves the ordinary same-state path alone).
BEZEL_TEST(borrowed_disposed_branch_join_4247,
           accept_both_arms_dispose_28_3) {
    JitResult jr = runAotChecked(wrapCounting(
        "var cond := true; "
        "var x := new Res(5); "
        "var y := x; "
        "if cond then "
        "  y := new Res(1); "
        "  y.Dispose(); "
        "else "
        "  y := new Res(2); "
        "  y.Dispose(); "
        "end; "
        "x.Dispose(); "
        "return Counter.Dropped;"));
    if (wasSkipped(jr)) return;
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 2);  // Res(1) [taken arm] + Res(5) [x]
}

// A4 — rebind to ANOTHER BORROW, no dispose in either arm at all. Stays
// Borrowed/Borrowed (same-state), never a Borrowed/Disposed split.
BEZEL_TEST(borrowed_disposed_branch_join_4247,
           accept_rebind_to_another_borrow_no_dispose_28_3) {
    JitResult jr = runAotChecked(wrapCounting(
        "var cond := true; "
        "var x := new Res(5); "
        "var z := new Res(7); "
        "var y := x; "
        "if cond then "
        "  y := z; "
        "end; "
        "x.Dispose(); "
        "z.Dispose(); "
        "return Counter.Dropped;"));
    if (wasSkipped(jr)) return;
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 2);  // x and z, independently
}

// A5 — NEITHER arm disposes; `y` is read as a plain borrow both inside the
// `if` and after the join. No Disposed state anywhere in this program.
BEZEL_TEST(borrowed_disposed_branch_join_4247, accept_neither_disposes_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(cond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    if cond then "
        "      var t1 := y.tag; "
        "    end; "
        "    var t2 := y.tag; "
        "    x.Dispose(); "
        "    return t2; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A6 — nested if, disposes on the innermost taken path, never touched
// again. Pins that the same-state OR-merge addition does not itself
// introduce a spurious rejection when there is genuinely no later use.
BEZEL_TEST(borrowed_disposed_branch_join_4247,
           accept_nested_if_no_further_touch_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(outerCond: bool, innerCond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    if outerCond then "
        "      if innerCond then "
        "        y := new Res(1); "
        "        y.Dispose(); "
        "      end; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A7 — loop body, disposes conditionally inside the loop, never touched
// again after the loop. Executes under the strict AOT oracle, ASan/LSan.
BEZEL_TEST(borrowed_disposed_branch_join_4247,
           accept_loop_no_touch_after_asan_28_3) {
    JitResult jr = runAotChecked(wrapCounting(
        "var x := new Res(5); "
        "var y := x; "
        "var i := 0; "
        "while i < 3 do "
        "  if i = 1 then "
        "    y := new Res(1); "
        "    y.Dispose(); "
        "  end; "
        "  i := i + 1; "
        "end; "
        "x.Dispose(); "
        "return Counter.Dropped;"));
    if (wasSkipped(jr)) return;
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 2);  // Res(1) [i=1 iteration] + Res(5) [x]
}

// ============================================================================
// RUNTIME SEVERITY REGRESSION GUARD — the pre-fix defect's own measured
// crash, loop-amplified. If this fix ever regresses back to silent
// acceptance, this row demonstrates the shape SEGVs (not merely leaks) under
// repetition, matching this lane's own measured finding recorded in the
// file header. Executes via `runAot` (not `runAotChecked`): the STRICT
// oracle already treats a SIGSEGV as `ran == false`; this row asserts the
// FIX (compile-time rejection) rather than re-deriving the crash, since the
// crash itself was already measured directly via
// `scripts/agent_check.sh --probe --asan` (see file header) and re-deriving
// it here would require reverting the fix, which is out of place in a
// regression test. This row instead pins that the exact 200x-loop shape
// that measured SEGV now fails to COMPILE at all.
// ============================================================================

BEZEL_TEST(borrowed_disposed_branch_join_4247,
           axis_loop_amplified_read_variant_rejected_not_merely_a_leak_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var sum := 0; "
        "    while i < 200 do "
        "      var cond := true; "
        "      var x := new Res(5); "
        "      var y := x; "
        "      if cond then "
        "        y := new Res(1); "
        "        y.Dispose(); "
        "      end; "
        "      x := new Res(9); "
        "      var t := y.tag; "
        "      x.Dispose(); "
        "      sum := sum + t; "
        "      i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kJoinMismatchNeedle));
}
