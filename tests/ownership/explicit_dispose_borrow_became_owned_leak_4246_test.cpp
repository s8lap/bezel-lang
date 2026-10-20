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
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 9 checked rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics, and then RETURNED from the row on
// it. That is an ABSORBING site by
// scripts/check_toolchain_skip_surface.py's own definition, and it is a silent
// vacuous pass: on a box whose clang cannot link the ASan binary, all
// 9 rows returned WITHOUT asserting and the suite reported a clean PASS
// having compiled and run nothing. MEASURED on the pre-change tree with the
// harness's own knob (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`): every row green.
//
// Every one of those guards becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position: the
// skip is still recognised as an environment condition, but it now MARKS THE
// ROW FAILED instead of hiding it. The lenient legs' assertions additionally
// now RUN UNCONDITIONALLY rather than being skipped, which is the other half of
// the strengthening. Every substantive assertion in every row (r.ran and the
// exitCode pins) stays exactly where it was.

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
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(jr));
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
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(jr));
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
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(jr));
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
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(jr));
    BEZEL_EXPECT_TRUE(jr.ran);
    // MEASURED: exit=2 (Res(1) once via the first Dispose(), Res(5) once via
    // x.Dispose(); the second y.Dispose() is a clean no-op).
    BEZEL_EXPECT_EQ(jr.exitCode, 2);
}

// ============================================================================
// §28.6 DOUBLE-FREE REGRESSION GUARD — UPDATED post-`fix #4236` (§28.3/§28.6,
// v0.298). ORIGINALLY this row disposed `y` while it was STILL a genuine,
// never-reassigned borrow of `x` (the §28.6 "recorded gap" this file's own
// header describes: "a plain local's borrow-dispose is unenforced, so this
// compiles"), THEN reassigned to a fresh owned value and disposed again — a
// naive per-DECLARATION reclaim flag would make the FIRST (still-borrow)
// call ALSO reclaim, double-freeing `x`'s object once `x.Dispose()` ran; the
// per-OCCURRENCE design kept that first call dispatch-only.
//
// `fix #4236` closed exactly that "recorded gap" — an explicit `.Dispose()`
// through a bare-identifier alias of a still-owned local is now a compile
// error (`cannot call Dispose() on y: it is a borrow (an alias) of x`),
// correctly and per §28.6/§28.3 as written. The ORIGINAL program above no
// longer compiles: `jr.ran` is false with that diagnostic, not the ASan
// oracle this row exists to run. The still-borrow `y.Dispose()` line is
// DROPPED (LANE_RULES-directed restructuring, R18 cycle-2 gate arbitration)
// — there is no way to construct a program where a still-live borrow is
// explicitly disposed, EVER, now that #4236 rejects it universally, so the
// specific per-occurrence PROPERTY this row verified (one dispatch-only
// occurrence + one reclaiming occurrence on the SAME symbol, distinguished
// by states_ classification at each call) is no longer expressible — it
// would need a program shape the ownership checker now statically refuses
// to accept. Said plainly rather than silently replaced with a weaker
// claim: this row is retained as a plain reassign-then-dispose reclaim
// check (redundant with `reclaim_is_exactly_once_counting_28_2` above, kept
// separately for its own historical identity/number); the genuine
// per-occurrence dispatch-vs-reclaim distinction is still exercised by
// `reclaim_then_reassign_to_borrow_then_dispose_no_double_free_28_3` and
// `branch_join_mixed_reclaim_no_crash_no_double_free_28_3` below, both
// similarly updated. // per §28.3, §28.6, v0.298, #4236, #4246
BEZEL_TEST(explicit_dispose_borrow_became_owned_leak_4246,
           dispose_while_still_borrow_then_reassign_then_dispose_no_double_free_28_6) {
    JitResult jr = runAotChecked(wrapCounting(
        "var x := new Res(5); "
        "var y := x; "
        "y := new Res(1); "
        "y.Dispose(); "
        "x.Dispose(); "
        "return Counter.Dropped;"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(jr));
    BEZEL_EXPECT_TRUE(jr.ran);
    // MEASURED: exit=2 (Res(1) reclaimed via y.Dispose(); Res(5) reclaimed
    // via x.Dispose()).
    BEZEL_EXPECT_EQ(jr.exitCode, 2);
}

// ============================================================================
// STICKY-BIT RESET REGRESSION GUARD — UPDATED post-`fix #4236` (§28.3/§28.6,
// v0.298). ORIGINALLY: reclaim via `y`, THEN reassign `y` to a DIFFERENT
// existing borrow (`x2`), THEN dispose `y` AGAIN while it was that borrow.
// The sticky `explicitDisposeReclaimedSlot` bit had to be cleared by the
// reassignment, or that second dispose would have incorrectly reclaimed
// `x2`'s still-live object — a double-free once `x2.Dispose()` ran.
//
// That second `y.Dispose()` (disposing `y` while it is a borrow of `x2`) is
// now `fix #4236`'s own rejection — a compile error, unconditionally,
// regardless of the sticky bit — so the ORIGINAL program no longer compiles
// (`jr.ran` false). Said plainly: the exact hazard this row measured (the
// sticky bit silently overriding a correct "do not reclaim" verdict for a
// borrow-state occurrence) can no longer be reached, because that verdict
// is no longer computed dynamically at all for this shape — #4236 refuses
// the call BEFORE codegen's reclaim/dispatch decision ever runs.
// Restructured (LANE_RULES-directed, R18 cycle-2 gate arbitration) to DROP
// only the now-illegal second `y.Dispose()` call — the reassignment to the
// borrow `x2` is KEPT (that part was always legal) so the row still checks
// that a local which already reclaimed once via explicit dispose can be
// safely rebound to an existing borrow afterward with no crash and no
// double free, `x2` being disposed independently and exactly once via its
// own `x2.Dispose()`. This is closely related to
// `axis_reassign_to_another_borrow_stays_clean_28_3` above (same "reassign
// to a borrow stays clean" property) with an extra reclaim epoch on `y`
// ahead of it. // per §28.3, §28.6, v0.298, #4236, #4246
BEZEL_TEST(explicit_dispose_borrow_became_owned_leak_4246,
           reclaim_then_reassign_to_borrow_then_dispose_no_double_free_28_3) {
    JitResult jr = runAotChecked(wrapCounting(
        "var x := new Res(5); "
        "var x2 := new Res(9); "
        "var y := x; "
        "y := new Res(1); "
        "y.Dispose(); "
        "y := x2; "
        "x.Dispose(); "
        "x2.Dispose(); "
        "return Counter.Dropped;"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(jr));
    BEZEL_EXPECT_TRUE(jr.ran);
    // MEASURED: exit=3 (Res(1) reclaimed via y.Dispose(); x's Res(5)
    // reclaimed via x.Dispose(); x2's Res(9) reclaimed via x2.Dispose()).
    BEZEL_EXPECT_EQ(jr.exitCode, 3);
}

// ============================================================================
// BRANCH-JOIN REGRESSION GUARD — UPDATED post-`fix #4236` (§28.3/§28.6,
// v0.298). ORIGINALLY: one `if` arm reassigned-and-reclaimed `y`, the OTHER
// disposed it while still a genuine, never-reassigned borrow of `x`; a THIRD
// dispose was reachable from both arms after the merge. `joinInto`'s
// conservative AND-merge made that post-merge call "not reclaim eligible" on
// BOTH arms — which is what stopped the borrow arm from double-freeing `x`
// — but that same conservatism meant the RECLAIMING arm's runtime slot was
// ALREADY null by the time the post-merge call ran, so the fallback dispatch
// needed its own null-guard (this fix's third part) or it SIGSEGVs reading
// through the null receiver.
//
// The `else` arm's `y.Dispose()` (disposing `y` while it is still a genuine,
// untouched borrow of `x`) is now `fix #4236`'s own rejection — a compile
// error on EVERY branch unconditionally, not just the ones actually taken at
// runtime (the ownership checker rejects a statically-reachable borrow
// dispose regardless of which arm executes) — BUT ONLY on the
// bare-IDENTIFIER-alias axis `disposeAliasSource` closes (`y := x;` then a
// later dispose). There is no way to construct EITHER arm of an `if` where
// one side disposes a still-live BARE-IDENTIFIER borrow — #4236 forecloses
// that shape identically in every branch — so the exact "mixed
// borrow-dispatch vs owned-reclaim" split this row ORIGINALLY exercised via
// that specific shape is unreachable on that axis. ⚠ CORRECTED (send-back
// #4272): an earlier revision of this comment claimed the split was
// "unreachable by construction" full stop — that overstated the fix. It is
// unreachable on the bare-identifier-alias axis only; see the NEW row below
// this one, which restores the coverage this restructuring dropped via the
// axis that is still open.
//
// TRIED AND REJECTED (worth recording so it is not retried): making the
// `else` arm reassign `y` to a fresh owned value WITHOUT disposing it
// in-branch, relying on §28.3's Owned/Disposed join to admit the post-merge
// dispose. MEASURED: the ownership checker rejects that shape outright —
// "local `y` may have already been disposed on another branch and may not
// be read here (conditional-discharge join, §28.3 / §28.2)" — a `maybe-
// disposed` local may not be TOUCHED again at all, not even by another
// `.Dispose()` (unlike a DEFINITELY-disposed local, which idempotence
// admits). So a post-merge dispose is only ever legal when EVERY arm
// definitely discharges `y` first — and on the bare-identifier-alias axis,
// the only way such a LOCAL alias's `.Dispose()` can be legal at all is for
// it to be genuinely Owned at that call (any bare-identifier-borrow alias is
// rejected), which always sets the sticky reclaim bit — so on THAT axis the
// AND-merge is always true whenever the program compiles. ⚠ CORRECTED
// (send-back #4272): this was previously written as "unreachable by
// construction" with no axis qualifier. `disposeAliasSource` is set ONLY
// from a bare `IdentifierExpr` RHS (Ownership.cpp:6717-6731/9368-9382) — a
// reassignment through a MEMBER-ACCESS read (`y := h.item;`, a genuine live
// borrow) is NOT caught by it at all and falls into the same documented
// "plain var, not rejected" gap (Ownership.cpp:8025-8028) rows above already
// rely on. That axis still reaches the exact mixed-AND-merge classification
// (`true` from an owned-reassign-and-dispose arm, `false` from a
// member-access-borrow-and-dispose arm) the null-guard exists to guard, and
// the null-guard is still the only thing preventing a SIGSEGV there — see
// `branch_join_mixed_reclaim_via_member_access_borrow_no_crash_28_3_4272`
// below, which restores that coverage.
//
// Restructured (LANE_RULES-directed, R18 cycle-2 gate arbitration) to keep
// a real, MEASURED assertion — both arms now reassign-and-dispose with
// DIFFERENT fresh values, still exercising the sticky-bit AND-merge itself
// (both arms set the bit, so the post-merge dispose is a correctly-skipped
// no-op on both arms, unlike the straight-line
// `reclaim_then_reassign_to_borrow_then_dispose_no_double_free_28_3` row
// above, which never branches) — rather than silently keeping the old
// "null-guard" framing over a program that no longer demonstrates it. This
// row is now a control for the ALWAYS-true (bare-identifier axis) case; the
// new row below is the regression guard for the genuinely mixed
// (member-access axis) case. // per §28.3, §28.6, v0.298, #4236, #4246
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
        "      y := new Res(2); "
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
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(jr));
    BEZEL_EXPECT_TRUE(jr.ran);
    // MEASURED: exit=4 (each call: its own arm's Res(1)/Res(2) reclaimed [1]
    // + post-merge y.Dispose() no-op, sticky bit AND-merge correctly true on
    // both arms [0] + x's Res(5) reclaimed via x.Dispose() [1] = 2 per call;
    // 2 calls = 4). The load-bearing part is `jr.ran` — a broken AND-merge
    // (an OR, or a dropped reset) would double-free here instead.
    BEZEL_EXPECT_EQ(jr.exitCode, 4);
}

// ============================================================================
// BRANCH-JOIN NULL-GUARD REGRESSION GUARD — MEMBER-ACCESS AXIS (issue #4272,
// send-back on `97db4ac`). Restores the coverage the row above's
// restructuring dropped: `fix #4246`'s third part (a null-guard around the
// dispatch-only fallback `emitDynamicDispose(recv)`) is still LIVE and still
// the only thing preventing a SIGSEGV, via the axis `fix #4236` does not
// close.
//
// `disposeAliasSource` (`fix #4236`) is set ONLY from a bare `IdentifierExpr`
// RHS (Ownership.cpp:6717-6731/9368-9382). A reassignment through a
// MEMBER-ACCESS read (`y := h.item;`, `h.item` a genuine, still-live borrow
// of `x`) is NOT recognized by that check at all — it falls into the same
// documented "plain var, not rejected" recorded gap
// (Ownership.cpp:8025-8028) the file header cites, so `y.Dispose()`
// immediately after is dispatch-only (unenforced), not a compile error.
//
// One arm reassigns `y` to a FRESH owned value and disposes it (reclaim-
// eligible = true, sets the sticky `explicitDisposeReclaimedSlot` bit, nulls
// the runtime slot). The OTHER arm reassigns `y` to `h.item` (a member-
// access borrow of `x`, still live) and disposes it (reclaim-eligible =
// false — correctly dispatch-only, since reclaiming here would double-free
// `x`; the sticky bit stays false). `markDisposedByName` runs regardless of
// eligibility, so BOTH arms leave `y`'s dataflow `state` at definitely-
// Disposed (the join is unconditional, not maybe-disposed, so the post-merge
// touch is legal — a §28.2 idempotent-redundant-dispose lint fires, not an
// error). The sticky bit's own AND-merge, though, is `true ∧ false = false`
// — the exact "mixed" classification `fix #4246`'s null-guard exists to
// guard: on the runtime path where the OWNED arm ran, the post-merge
// dispatch-only fallback reads a slot ALREADY nulled by that arm's own
// reclaim; on the path where the BORROW arm ran, the slot is still live and
// the same fallback just re-dispatches (a redundant but safe teardown call
// on `x`'s still-owned object). MEASURED (this lane,
// `scripts/agent_check.sh --probe --asan`, 300 iterations, condition
// alternating every iteration so BOTH arms run ~150 times each): compiles
// clean (one `§28.2` idempotent-redundant-dispose lint at the post-merge
// call, not an error) and runs ASan/LSan-clean, exit 0 — confirming the
// null-guard still does real work on the arm that reclaimed, and the
// dispatch-only fallback is still safe (no double-free) on the arm that
// didn't. Without the null-guard this SIGSEGVs on every odd-indexed
// iteration (the reclaiming arm). // per §28.3, §28.6, v0.298, #4236,
// #4246, #4272
// ============================================================================

BEZEL_TEST(explicit_dispose_borrow_became_owned_leak_4246,
           branch_join_mixed_reclaim_via_member_access_borrow_no_crash_28_3_4272) {
    JitResult jr = runAotChecked(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Holder "
        "  public item: Res; "
        "  public constructor(r: Res) this.item := r; end constructor; "
        "end Holder; "
        "class C "
        "  public static method OneIteration(cond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    var h := new Holder(x); "
        "    if cond then "
        "      y := new Res(1); "
        "      y.Dispose(); "
        "    else "
        "      y := h.item; "
        "      y.Dispose(); "
        "    end; "
        "    y.Dispose(); "
        "    x.Dispose(); "
        "    return 1; "
        "  end OneIteration; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 300 do "
        "      acc := acc + C.OneIteration(i mod 2 = 0); "
        "      i := i + 1; "
        "    end; "
        "    return acc - 300; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(jr));
    // The load-bearing assertion: `jr.ran` false here means either a SIGSEGV
    // (the null-guard regressed) or a leak/double-free (the AND-merge or the
    // dispatch-only fallback regressed). MEASURED: exit=0 (each
    // OneIteration returns 1, 300 calls, minus 300 — 150 reclaiming-arm
    // calls and 150 borrow-arm calls, alternating, never crash).
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// ============================================================================
// BRANCH-JOIN NULL-GUARD REGRESSION GUARD — CALL-RESULT AXIS (issue #4272,
// follow-up to the member-access row above). C3-L9's axis sweep for #4272
// enumerated every non-`IdentifierExpr` RHS shape the issue's own text names
// or suggests (member access, call result, cast, ternary, `await`, `move`)
// against the SAME branch-join shape as the member-access row above, to
// bound what `fix #4246`'s null-guard is actually protecting. Table
// (MEASURED, `scripts/agent_check.sh --probe [--asan]`, this lane):
//
//   member access (`y := h.item;`)         reaches the null-guard, clean — landed above
//   call result   (`y := h.Get();`)        reaches the null-guard, clean — THIS row
//   cast          (`y := (h.item as Res) ?? h.item;`)   reaches it, clean (measured, no new row)
//   ternary       (`y := (if c then h.item else h.item);`) reaches it, clean (measured, no new row)
//   `move` of a non-owned field (`y := move h.item;`)   DIFFERENT bug: a
//     straight-line (no branch, no merge, no null-guard involvement at all)
//     `move` of a plain (non-`owned`) field is wrongly ACCEPTED — spec says
//     "`move` is also an error on ... a borrow" — and codegen treats the
//     moved-from read as a fresh owned value, so `y.Dispose()` reclaims the
//     SAME object `x` still owns; `x.Dispose()` then reads/frees it again —
//     a measured heap-use-after-free with ZERO branching. Filed separately
//     (issue 4284) — not this issue's null-guard at all.
//   `await` of a Pattern-A class-typed task result (`using var t := ...;
//     y := await t;`)                       DIFFERENT bug: also reproduces
//     straight-line (no branch) as a heap-use-after-free inside
//     `bz$elemDispose` / `bz_task_free` (bezel_runtime_concurrency.cpp:1156)
//     when the `using var` task local's own scope ends — the task frame's
//     result-element disposal does not know the borrowed result was already
//     independently discharged by its real owner. Filed separately (issue
//     4285) — a `Task`/`OwningTask` frame-teardown gap, not codegen's
//     dispatch-only fallback or the null-guard.
//
// So 4 of 6 candidate axes reach the EXACT null-guard path (mixed
// true/false `explicitDisposeReclaimedSlot` AND-merge, one arm's slot
// already nulled by its own reclaim); the other 2 hit unrelated,
// independently-filed bugs before ever reaching branch-join territory. This
// row adds the CALL-RESULT axis as a second, AST-distinct (`CallExpr`, not
// `MemberAccessExpr`) discriminator alongside the member-access row, so the
// suite does not depend on a single expression-node shape to prove the
// guard live. Structure identical to the member-access row: one arm
// reassigns-and-disposes a FRESH owned value (reclaim-eligible, nulls the
// slot); the other reassigns via `h.Get()` (a plain, non-`owned`-announced
// instance method returning `this.item` — a borrow, same as the field read)
// and disposes it (dispatch-only, correctly, since reclaiming would
// double-free `x`); the shared post-merge dispose hits the mixed AND-merge.
//
// MECHANICALLY VERIFIED (this lane, C3-L9, not merely argued): with the
// null-guard temporarily removed from `emitExplicitDisposeChain`'s
// dispatch-only fallback (LlvmBackend_Calls2.cpp, the
// `builder_.CreateCondBr(nonNull, dnn, dcont)` guard replaced with an
// unconditional `emitDynamicDispose(recv)`), BOTH this row and the
// member-access row above SIGSEGV under a plain (non-ASan) rebuild — the
// pre-existing member-access row is not just "untested", the underlying
// mechanism is exercised by both axes identically. Restored on the
// original guard, both rows pass again, clean under ASan. // per §28.3,
// §28.6, v0.298, #4236, #4246, #4272
// ============================================================================

BEZEL_TEST(explicit_dispose_borrow_became_owned_leak_4246,
           branch_join_mixed_reclaim_via_call_result_borrow_no_crash_28_3_4272) {
    JitResult jr = runAotChecked(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Holder "
        "  public item: Res; "
        "  public constructor(r: Res) this.item := r; end constructor; "
        "  public method Get(): Res "
        "    return this.item; "
        "  end Get; "
        "end Holder; "
        "class C "
        "  public static method OneIteration(cond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    var h := new Holder(x); "
        "    if cond then "
        "      y := new Res(1); "
        "      y.Dispose(); "
        "    else "
        "      y := h.Get(); "
        "      y.Dispose(); "
        "    end; "
        "    y.Dispose(); "
        "    x.Dispose(); "
        "    return 1; "
        "  end OneIteration; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 300 do "
        "      acc := acc + C.OneIteration(i mod 2 = 0); "
        "      i := i + 1; "
        "    end; "
        "    return acc - 300; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(jr));
    // MEASURED: exit=0 (each OneIteration returns 1, 300 calls, minus 300;
    // 150 reclaiming-arm + 150 borrow-arm calls, alternating, never crash).
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}
