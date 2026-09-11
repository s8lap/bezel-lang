// Ownership / codegen — a local whose DECLARATION is a plain borrow alias
// (`var y := x;`) but which is later REASSIGNED to a fresh owned value
// (`y := new Res(1);`) and then explicitly `.Dispose()`d does not actually
// free the owned value. Issue #4246.
//
// SPEC — settled, not underspecified. §28.3 "Local variables, ownership, and
// borrows" (v0.298) requires the explicit `.Dispose()` (the ownership
// checker's own dynamic/`states_` dataflow correctly demands and accepts it
// once `y` holds an owned value), and §28.6 "Who may call `Dispose()`"
// governs the explicit-dispose receiver. The OBJECT an explicit `.Dispose()`
// reclaims is owned by the receiver's slot at THAT program point — a fact
// the checker's own dataflow already proves per call site.
//
// WHY IT WAS MISSED. `explicitDisposeOwnedReceiverSlot`
// (LlvmBackend_Calls2.cpp) gated reclaim-eligibility on
// `unwindOwnedLocalClass(ld)`, which classifies PURELY from `ld`'s
// DECLARATION initializer form (`new`/call/move/bind/conditional-meet).
// `var y := x;`'s initializer is the bare identifier `x` — a borrow, none of
// those forms — so `unwindOwnedLocalClass` returned null FOREVER for `y`,
// even after `y := new Res(1);` made the ownership checker's OWN dataflow
// classify `y` Owned. The explicit `y.Dispose()` therefore only DISPATCHED
// the destructor (LlvmBackend_Disposal.cpp's `emitDynamicDispose`) and never
// RECLAIMED the heap block — a leak, not a crash.
//
// MEASURED PRE-FIX (base r18-c2 = 9057ec7, bezelc built clean):
//
//   var x := new Res(5); var y := x; y := new Res(1); var t := y.tag;
//   y.Dispose(); x.Dispose();                          // 300 iterations
//     --sanitize=address, ASAN_OPTIONS=detect_leaks=1
//     -> "Direct leak of 4800 byte(s) in 300 object(s)" — 16 B/object x 300,
//        exactly one leaked `Res(1)` per iteration, matching the issue.
//
// THE FIX — three parts, because a naive per-DECLARATION flag (mirroring
// `LocalDecl::becomesOwnedViaReassign`, which is deliberately restricted to
// NULL-initialized locals for exactly this reason) is UNSOUND for a
// borrow-initialized local: the SAME local can be legally `.Dispose()`d at a
// point where it is STILL a genuine borrow (§28.6's documented "recorded
// gap" — a plain local's borrow-dispose is unenforced), and reclaiming
// there would free a value another owner still holds.
//
//   1. A NEW per-OCCURRENCE AST flag, `IdentifierExpr::disposeReclaims-
//      ReassignedBorrow`, stamped by the ownership pass
//      (`markExplicitDisposeReclaimIfReassignedBorrow`, Ownership_Checks.cpp)
//      on the SPECIFIC `.Dispose()` receiver occurrence whose `states_`
//      classification is Owned AT THAT CALL — never on the LocalDecl, so an
//      earlier call on the same local while it is still a genuine borrow is
//      untouched.
//   2. A STICKY per-symbol bit, `LocalOwnership::explicitDisposeReclaimed-
//      Slot` (OwnershipState.hpp), so a REPEATED `.Dispose()` on the SAME
//      value-epoch (§28.2 idempotence) is also recognised as reclaim-
//      eligible — without it, the repeat call falls to the dispatch-only
//      fallback and reads through the pointer the first call's reclaim
//      already nulled (a SIGSEGV, measured while developing this fix). Reset
//      on every reassignment (`handleAssignment`) so the fact cannot bleed
//      into a later, unrelated value-epoch. `joinInto`'s same-state branch
//      ANDs (not ORs) this bit across a branch merge — conservative in the
//      double-free-avoiding direction — because a merge where only ONE arm
//      reclaimed must not treat the OTHER arm's still-live borrow as
//      reclaim-eligible.
//   3. A NULL-GUARD around the pre-existing unconditional dispatch-only
//      fallback (`emitDynamicDispose(recv)` in `emitExplicitDisposeChain`,
//      LlvmBackend_Calls2.cpp) — needed because the AND-merge above can
//      leave a post-branch-join dispose call classified "not reclaim
//      eligible" even on a runtime path where an EARLIER dispose already
//      reclaimed (and nulled) this exact slot; the guard is a no-op for
//      every PRE-#4246 shape reaching that fallback (none of them ever saw a
//      nulled slot there) and prevents a crash on the new one.
//
// §28.3 (local variables, ownership and borrows), §28.6 (who may call
// Dispose), §28.2 (the IDisposable contract, exactly-once / idempotence).
// v0.298, #4246.

#include <string>

#include "support/aot_support.hpp"  // runAot / runAotChecked — the AOT oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A checked AOT run is SKIPPED (not failed) when the ASan link toolchain is
// absent (mirrors the #3968 harness helper — lane worktrees in this project
// have the ASan runtime unavailable; the gate tree has it).
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// `class Res` is the disposable-shaped class from the issue. `body` is the
// Main method body. The destructor prints "D" so tests can count disposals
// from stdoutText without a counting static (see kCountingPrelude below for
// the exact-count rows, which need a monotone counter across calls).
const char* kPrelude =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor Console.WriteLine(\"D\"); end destructor; "
    "end Res; "
    "class C "
    "  public static method Main(): int ";

const char* kEpilogue =
    "  end Main; "
    "end C; ";

std::string wrap(const std::string& body) {
    return std::string(kPrelude) + body + kEpilogue;
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int count = 0;
    size_t pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.size();
    }
    return count;
}

// Counting prelude — `Counter.Dropped` increments once per destructor run,
// so tests can pin the EXACT number of disposals (necessary, not just
// sufficient, alongside the ASan/LSan leak oracle: an under-fire reads the
// same destructor count as a correct run, since the destructor always
// dispatches — only the RECLAIM differs).
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

std::string wrapCounting(const std::string& body) {
    return std::string(kCountingPrelude) + body + kEpilogue;
}

}  // namespace

// ============================================================================
// THE PRIMARY REGRESSION — the issue's exact reproducer in a 100-iter loop.
// ============================================================================

BEZEL_TEST(explicit_dispose_borrow_became_owned_leak_4246,
           issue_repro_borrow_declared_reassigned_owned_explicit_dispose_28_3) {
    JitResult jr = runAotChecked(wrap(
        "var i := 0; "
        "var acc := 0; "
        "while i < 100 do "
        "  var x := new Res(5); "
        "  var y := x; "
        "  y := new Res(1); "
        "  var t := y.tag; "
        "  y.Dispose(); "
        "  x.Dispose(); "
        "  acc := acc + t; "
        "  i := i + 1; "
        "end; "
        "return acc - 100;"));
    if (wasSkipped(jr)) return;  // ASan toolchain unavailable — skip
    // Pre-fix: LSan catches the leak (one `Res(1)` block per iteration, 100
    // of them) and `jr.ran` becomes false. Post-fix: the explicit Dispose
    // reclaims each block, LSan is silent, `jr.ran` stays true.
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
    // Two destructors per iteration — x's Res(5) and y's Res(1) — 200 total.
    BEZEL_EXPECT_EQ(countOccurrences(jr.stdoutText, "D"), 200);
}

// ============================================================================
// EXACT-COUNT / EXACTLY-ONCE — a counting destructor pins the number, so an
// over-firing fix (double-dispose) is caught as a wrong NUMBER too, not only
// as an ASan crash.
// ============================================================================

BEZEL_TEST(explicit_dispose_borrow_became_owned_leak_4246,
           reclaim_is_exactly_once_counting_28_2) {
    JitResult jr = runAot(wrapCounting(
        "var i := 0; "
        "var acc := 0; "
        "while i < 50 do "
        "  var x := new Res(5); "
        "  var y := x; "
        "  y := new Res(1); "
        "  y.Dispose(); "
        "  x.Dispose(); "
        "  acc := acc + 1; "
        "  i := i + 1; "
        "end; "
        "return Counter.Dropped + (acc - 50);"));
    BEZEL_EXPECT_TRUE(jr.ran);
    // 100 destructions exactly (2 per iteration x 50). MEASURED (fixed
    // compiler, probe harness): exit=100.
    BEZEL_EXPECT_EQ(jr.exitCode, 100);
}

// ============================================================================
// AXIS — reassignment to a CALL RESULT (not a `new` literal) leaks the same
// way pre-fix; the fix does not key on the reassignment's RHS shape.
// ============================================================================

BEZEL_TEST(explicit_dispose_borrow_became_owned_leak_4246,
           axis_reassign_to_owned_call_result_28_3) {
    JitResult jr = runAotChecked(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Make(): owned Res "
        "    return new Res(7); "
        "  end Make; "
        "  public static method OneIteration(): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    y := C.Make(); "
        "    var t := y.tag; "
        "    y.Dispose(); "
        "    x.Dispose(); "
        "    return t; "
        "  end OneIteration; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 100 do "
        "      acc := acc + C.OneIteration(); "
        "      i := i + 1; "
        "    end; "
        "    return acc - 700; "
        "  end Main; "
        "end C; ");
    if (wasSkipped(jr)) return;
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// ============================================================================
// AXIS — POSITIVE CONTROL: reassignment to another EXISTING BORROW (never
// becomes owned) needs no disposal at all and must stay leak-free /
// unaffected on both sides of the fix (the shape #4225's own tests used to
// deliberately AVOID tripping this issue).
// ============================================================================

BEZEL_TEST(explicit_dispose_borrow_became_owned_leak_4246,
           axis_reassign_to_another_borrow_stays_clean_28_3) {
    JitResult jr = runAotChecked(wrapCounting(
        "var x := new Res(5); "
        "var x2 := new Res(9); "
        "var y := x; "
        "y := x2; "
        "x.Dispose(); "
        "x2.Dispose(); "
        "return Counter.Dropped;"));
    if (wasSkipped(jr)) return;
    BEZEL_EXPECT_TRUE(jr.ran);
    // Only x and x2 are ever independently disposed; y is never itself
    // disposed on this row (MEASURED: exit=2).
    BEZEL_EXPECT_EQ(jr.exitCode, 2);
}

// ============================================================================
// AXIS — REJECT CONTROL: the checker's own missing-disposal diagnostic for
// this exact shape must be untouched by the codegen fix.
// ============================================================================

BEZEL_TEST(explicit_dispose_borrow_became_owned_leak_4246,
           axis_missing_dispose_after_reassignment_still_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        "var x := new Res(5); "
        "var y := x; "
        "y := new Res(1); "
        "var t := y.tag; "
        "x.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorMessage(r, "would be dropped here without disposal"));
}

// ============================================================================
// OWNERSHIP-PASS POSITIVE — the fix is a codegen gate, not a new rejection;
// the issue's own repro shape must still be accepted with zero diagnostics
// beyond it.
// ============================================================================

BEZEL_TEST(explicit_dispose_borrow_became_owned_leak_4246,
           ownership_check_stays_clean_28_3) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        "var x := new Res(5); "
        "var y := x; "
        "y := new Res(1); "
        "var t := y.tag; "
        "y.Dispose(); "
        "x.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ============================================================================
// §28.2 IDEMPOTENCE / REGRESSION GUARD — `y.Dispose(); y.Dispose();` after
// the borrow-to-owned reassignment. Before the sticky
// `explicitDisposeReclaimedSlot` bit existed, the SECOND call fell to the
// dispatch-only fallback and read through the pointer the FIRST call's
// reclaim had already nulled — a SIGSEGV, not a double-free — measured while
// developing this fix. The second call must instead be a clean no-op.
// ============================================================================

BEZEL_TEST(explicit_dispose_borrow_became_owned_leak_4246,
           double_dispose_after_reassignment_is_idempotent_not_a_crash_28_2) {
    JitResult jr = runAotChecked(wrapCounting(
        "var x := new Res(5); "
        "var y := x; "
        "y := new Res(1); "
        "y.Dispose(); "
        "y.Dispose(); "
        "x.Dispose(); "
        "return Counter.Dropped;"));
    if (wasSkipped(jr)) return;
    BEZEL_EXPECT_TRUE(jr.ran);
    // MEASURED: exit=2 (Res(1) once via the first Dispose(), Res(5) once via
    // x.Dispose(); the second y.Dispose() is a clean no-op).
    BEZEL_EXPECT_EQ(jr.exitCode, 2);
}

// ============================================================================
// §28.6 DOUBLE-FREE REGRESSION GUARD — dispose `y` while it is STILL a
// genuine, never-reassigned borrow of `x` (the §28.6 "recorded gap": a plain
// local's borrow-dispose is unenforced, so this compiles), THEN reassign to
// a fresh owned value and dispose again. A naive per-DECLARATION reclaim
// flag would make the FIRST (still-borrow) call ALSO reclaim, freeing `x`'s
// object out from under it — a double-free once `x.Dispose()` runs. The
// per-OCCURRENCE design must keep the first call dispatch-only.
// ============================================================================

BEZEL_TEST(explicit_dispose_borrow_became_owned_leak_4246,
           dispose_while_still_borrow_then_reassign_then_dispose_no_double_free_28_6) {
    JitResult jr = runAotChecked(wrapCounting(
        "var x := new Res(5); "
        "var y := x; "
        "y.Dispose(); "
        "y := new Res(1); "
        "y.Dispose(); "
        "x.Dispose(); "
        "return Counter.Dropped;"));
    if (wasSkipped(jr)) return;
    BEZEL_EXPECT_TRUE(jr.ran);
    // MEASURED: exit=2. The first y.Dispose() (while y aliases x) dispatches
    // x's destructor without reclaiming; the second y.Dispose() reclaims
    // Res(1); x.Dispose() re-dispatches x's destructor (a no-op — §28.2
    // idempotence, same physical object) and reclaims x's block. Two
    // DESTRUCTIONS, not three, and — the load-bearing part — no ASan error:
    // x's block is freed exactly once, by x.Dispose(), never by the
    // still-borrow y.Dispose().
    BEZEL_EXPECT_EQ(jr.exitCode, 2);
}

// ============================================================================
// STICKY-BIT RESET REGRESSION GUARD — reclaim via `y`, THEN reassign `y` to
// a DIFFERENT existing borrow, THEN dispose again. The sticky
// `explicitDisposeReclaimedSlot` bit must be cleared by the reassignment, or
// this second dispose would incorrectly reclaim `x2`'s still-live object —
// a double-free once `x2.Dispose()` runs.
// ============================================================================

BEZEL_TEST(explicit_dispose_borrow_became_owned_leak_4246,
           reclaim_then_reassign_to_borrow_then_dispose_no_double_free_28_3) {
    JitResult jr = runAotChecked(wrapCounting(
        "var x := new Res(5); "
        "var x2 := new Res(9); "
        "var y := x; "
        "y := new Res(1); "
        "y.Dispose(); "
        "y := x2; "
        "y.Dispose(); "
        "x.Dispose(); "
        "x2.Dispose(); "
        "return Counter.Dropped;"));
    if (wasSkipped(jr)) return;
    BEZEL_EXPECT_TRUE(jr.ran);
    // MEASURED: exit=3 (Res(1) reclaimed via the first y.Dispose(); x2's
    // destructor dispatched — not reclaimed — via the second y.Dispose();
    // x's Res(5) reclaimed via x.Dispose(); x2's block reclaimed via
    // x2.Dispose(), whose OWN destructor dispatch is a no-op re-run).
    BEZEL_EXPECT_EQ(jr.exitCode, 3);
}

// ============================================================================
// BRANCH-JOIN REGRESSION GUARD — one `if` arm reassigns-and-reclaims `y`,
// the other disposes it while still a genuine borrow; a THIRD dispose is
// reachable from both arms after the merge. `joinInto`'s conservative
// AND-merge makes the post-merge call "not reclaim eligible" on BOTH arms —
// which is what stops the borrow arm from double-freeing `x` — but that
// same conservatism means the RECLAIMING arm's runtime slot is ALREADY null
// by the time the post-merge call runs, so the fallback dispatch needs its
// own null-guard (this fix's third part) or it SIGSEGVs reading through the
// null receiver.
// ============================================================================

BEZEL_TEST(explicit_dispose_borrow_became_owned_leak_4246,
           branch_join_mixed_reclaim_no_crash_no_double_free_28_3) {
    JitResult jr = runAotChecked(
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
        "  public static method OneIteration(cond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    if cond then "
        "      y := new Res(1); "
        "      y.Dispose(); "
        "    else "
        "      y.Dispose(); "
        "    end; "
        "    y.Dispose(); "
        "    x.Dispose(); "
        "    return 1; "
        "  end OneIteration; "
        "  public static method Main(): int "
        "    var a := C.OneIteration(true); "
        "    var b := C.OneIteration(false); "
        "    return Counter.Dropped + a + b - 2; "
        "  end Main; "
        "end C; ");
    if (wasSkipped(jr)) return;
    BEZEL_EXPECT_TRUE(jr.ran);
    // MEASURED: exit=3 (true-branch call: Res(1) reclaimed [1] + x's Res(5)
    // reclaimed via x.Dispose() [1] = 2; false-branch call: x's destructor
    // dispatched once via the genuine-borrow y.Dispose() [1], the post-merge
    // y.Dispose() and x.Dispose()'s own dispatch are both idempotent no-ops
    // on the same physical object = 1; total 3). The load-bearing part is
    // `jr.ran` — pre-guard this SIGSEGVed on the true-branch iteration.
    BEZEL_EXPECT_EQ(jr.exitCode, 3);
}
