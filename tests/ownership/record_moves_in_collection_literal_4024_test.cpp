// Ownership — `MethodOwnership::recordMovesIn` (Ownership_Checks.cpp) had NO
// `CollectionLiteralExpr` arm at all: the SAME missing-arm gap #4003/#4028
// fixed for seven structural-wrapper node kinds, one node kind further. A
// `move` operand embedded directly in a collection literal element (or, for
// the dict form, a key) used as a `LocalDecl`/`LocalConstDecl` initializer
// or assignment RHS was never validated or marked — the same double-free
// family as #4003/#4028, for a node kind #4003's own landing deliberately
// declined to touch (see that file's audit comment) because it sits next to
// ~18 unaudited pre-existing `CollectionLiteralExpr` call sites elsewhere in
// Ownership.cpp/Ownership_Checks.cpp.
//
// Spec, v0.294:
//   §15.4 "Collection literals" — "each element expression of non-trivial-
//     dispose class type must be a borrow [or] a fresh producer"; a `move`
//     of a NAMED owned binding is likewise an admitted element source and is
//     subject to the ordinary §28.3 move-validation rules.
//   §28.3 "Local variables, ownership, and borrows" — the move/disposal
//     state machine `markMovedByName` enforces ("a `Disposed` local may not
//     be moved from").
//
// THE BUG (measured pre-fix on r15-c3-batch base, carrying #4003 fix 7b37af7
// and #4028 fix 4d40c6f):
//
//   using var h := new Holder(move x);        // x already .Dispose()d
//     -> REJECTED: "cannot move from disposed local `x` (§28.3)".
//   var arr: OwningArray<Res> := [move x];    // byte-identical move, as a
//                                             // collection-literal element
//     -> ACCEPTED, ZERO diagnostics. Compiled and run under
//        `--sanitize=address`: SEGV (null READ) in `main`, reading
//        `arr[0].tag` off the disposed-then-moved `x`'s freed storage.
//        Measured (this lane):
//        `/var/tmp/bezel-scratch/r15/4024/logs/l6-4024-probe-pre.log`.
//
// ROOT CAUSE: `recordMovesIn` is the ONE authority that walks a
// LocalDecl-initializer / assignment-RHS expression looking for a `move` to
// validate (via `markMovedByName`) and mark MovedFrom. Its dynamic_cast
// chain had no `CollectionLiteralExpr` arm, so for `[move x]` the chain fell
// off the end doing nothing and the `move` embedded in the literal's
// `elements` was never visited by this pass at all — invisible to
// `markMovedByName`, so a Disposed/MovedFrom source was silently accepted.
// (`checkRead`, the READ-side twin, already HAS a `CollectionLiteralExpr` arm
// — fix #3971 — but its `readOperand` helper deliberately DECLINES a
// top-level `move`/`await` element via `isTopLevelConsumingForm`, on the
// documented understanding that `recordMovesIn` is the authority that
// actually validates and marks it. It never was, for this node kind.)
//
// THE FIX: `recordMovesIn` gained a `CollectionLiteralExpr` arm recursing
// into `keys` (empty for the non-dict sequence form) and `elements` —
// structurally identical to `checkRead`'s own arm, mirroring the read side
// exactly as every #4003/#4028 arm mirrors its `checkRead` counterpart.
//
// CALL-SITE AUDIT (per the issue's own decline-with-reasons, and LANE_RULES
// §"For #4024, the ~18 unaudited CollectionLiteralExpr call sites are a real
// hazard"): every OTHER `CollectionLiteralExpr` call site in
// Ownership.cpp/Ownership_Checks.cpp was inspected before adding this arm.
// None of them calls `recordMovesIn` on these elements, nor is called BY
// `recordMovesIn`, so this new arm cannot double-process or conflict with
// any of them:
//   - `scanReassignExpr`, `moveOnlyArrayLeakScanExpr`,
//     `forEachBorrowStoreCallInExpr`, `collectBodyLambdasExpr`
//     (Ownership.cpp) — closure-reassignment / move-only-array-leak /
//     borrow-store-call / lambda-collection bookkeeping, each its own
//     separate walk.
//   - `scanCapturedOwnedConsumeExpr`, `scopedPreScanExpr`,
//     `scopedCaptureWalkExpr`, `scopedCalleeWalkExpr`, `callBoundedSourceOf`,
//     `callBoundedWalkExpr` (Ownership_Checks.cpp) — lambda-capture and
//     §28.14 call-bounded-callback scans, unrelated to move validation.
//   - `findThrowable28_8_1`, `collectReadIdentifiers` — unrelated leaf
//     inventories (throwable-expression finder; identifier collector).
//   - the §15.4 fresh-producer-in-borrowing-literal checks
//     (`sec154StoreArgIsClassTyped`'s `CollectionLiteralExpr` "fresh" arm,
//     `checkBorrowingTaskArrayLiteral`) — classify element FRESHNESS for a
//     different rule (borrowing-collection-store / `Task.WhenAll` literal),
//     never call `recordMovesIn`.
//   - `borrowingArrayLiteralLocalFieldStoreSource` — a LEAF classifier of
//     the LOCAL's own declaration shape (is this local's initializer a
//     non-dict `Array<T>` literal?), not a recursive element walk; does not
//     touch elements at all.
//   - `checkRead`'s own `CollectionLiteralExpr` arm (fix #3971) — the READ
//     side twin this arm mirrors; declines top-level consuming forms via
//     `readOperand`/`isTopLevelConsumingForm`, so the two walks partition
//     the work (checkRead validates plain reads of non-consuming elements;
//     recordMovesIn validates and marks `move` elements) with no overlap.
//
// per §15.4, §28.2, §28.3, v0.294, #4003, #4028, #4024

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot — the strong AOT oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotCheckedDisposal;

namespace {

constexpr char kDisposedMoveError[] =
    "cannot move from disposed local `x` (§28.3)";
// The post-ruling wording (`markMovedByName`'s Disposed arm is now a
// §28.2 WARNING per v0.303 / #4450 — the OLD §28.3 ERROR above is the
// pre-ruling wording, kept here only because the negative rows below have
// always used it for the controls whose scenario reaches the SAME arm via
// a different surrounding node kind.  The new wording is what rows 4-6 of
// the #3975 table now draw, and what the new
// `move_after_dominating_dispose_warns_4450_test.cpp` regression file
// pins for every consume position.
constexpr char kDisposedMoveWarn[] =
    "move from a local `x` already disposed at this point (§28.2)";

// A plain disposable class — no ctor consuming an owned argument is needed
// for this bug: the `move` is validated at the LITERAL element position
// itself, not inside a callee.
constexpr char kRes[] =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; "
    "end Res; ";

std::string program(const std::string& classesAndMain) {
    return kRes + classesAndMain;
}

}  // namespace

// =============================================================================
// CONTROL — the pre-existing non-literal `move` spelling was already
// correctly handled before this fix (it routes through `recordMovesIn`'s
// existing `UnaryExpr` arm, not the new `CollectionLiteralExpr` one) and
// must stay handled with the SAME diagnostic text.  Under v0.303 / #4450
// the text is the §28.2 consume-side WARNING, NOT the pre-ruling §28.3
// ERROR this row originally pinned — the wording is what changed (and what
// the new consume-side text `kDisposedMoveWarn` names), and the
// consistency between the two arms of `recordMovesIn` (the original
// `UnaryExpr` arm and the new `CollectionLiteralExpr` arm) is exactly the
// invariant this row was originally written to prove.
// =============================================================================
BEZEL_TEST(record_moves_in_collection_literal_4024,
           direct_move_of_disposed_local_warns_28_2_4450_control) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    using var y := move x; "
        "    return y.tag; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    bool hasWarn = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Warning &&
            d.message.find(kDisposedMoveWarn) != std::string::npos) {
            hasWarn = true; break;
        }
    }
    BEZEL_EXPECT_TRUE(hasWarn);
}

// =============================================================================
// NEGATIVE (issue #4024's own repro) — `move x` as a bare COLLECTION-LITERAL
// element (`OwningArray<Res>` sequence-literal form).
// FAIL-WITHOUT THE COLLECTION-LITERAL ARM: zero diagnostics — measured
// pre-fix.  After the arm was added (the fix #4024 itself, not #4450) the
// diagnostic text became the §28.3 "cannot move from disposed local"
// ERROR.  After #4450 / v0.303 the text is now the §28.2 consume-side
// WARNING — the same warning the control case above now draws.  The
// pre-/post-ruling consistency the control case proves is also the
// pre-/post-arm consistency this row proves: the new `CollectionLiteralExpr`
// arm draws the SAME diagnostic as the old `UnaryExpr` arm, and they reach
// it through the SAME code path (`markMovedByName`'s Disposed arm).
// =============================================================================
BEZEL_TEST(record_moves_in_collection_literal_4024,
           collection_literal_element_move_of_disposed_local_warns_28_2_4450) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    var arr: OwningArray<Res> := [move x]; "
        "    return arr[0].tag; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    bool hasWarn = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Warning &&
            d.message.find(kDisposedMoveWarn) != std::string::npos) {
            hasWarn = true; break;
        }
    }
    BEZEL_EXPECT_TRUE(hasWarn);
}

// The same case through the STRONG AOT oracle (real `bezelc` + link + exec):
// the program now COMPILES (with a warning), so the pre-fix "rejection"
// oracle cannot stand.  The checked-disposal backstop §28.2 promises is
// the new soundness claim — a member access through the surviving borrow
// of a logically-disposed instance raises `DisposedAccessException` in
// checked mode.  Same shape the regression file for #4450 uses for the
// other consume positions; this row is the collection-literal analogue so
// every #4003/#4028/#4024 wrapper kind has its own checked-mode row.
//
// IMPORTANT: the backstop ONLY fires on a MEMBER ACCESS through the
// disposed reference (§28.2).  The natural `[move x]` shape transfers
// ownership into the OwningArray element with no read of `x`'s fields,
// so the natural form runs to completion in checked mode (which is itself
// the §28.2 verdict).  The row below uses the SHAPE that DOES include a
// member access through the now-disposed element (`arr[0].tag`), so the
// runtime exception fires and the catch arm observes it; the
// static-analysis verdict (the §28.2 warning) is unchanged.
BEZEL_TEST(
    record_moves_in_collection_literal_4024,
    aot_collection_literal_element_move_of_disposed_local_raises_in_checked_mode_4450) {
    JitResult r = runAotCheckedDisposal(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    try "
        "      var arr: OwningArray<Res> := [move x]; "
        "      var t := arr[0].tag; "
        "      return 0; "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// A second, independently-shaped repro: an already-MOVED-FROM (not merely
// Disposed) local as a collection-literal element — exercises
// `markMovedByName`'s OTHER arm (`MovedFrom`), proving the new arm reaches
// `recordMovesInArgs`... no — reaches `markMovedByName` for both tracked
// error states, not just the one the issue happened to measure.
BEZEL_TEST(record_moves_in_collection_literal_4024,
           collection_literal_element_move_of_already_moved_local_rejected) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); "
        "    var first := move x; "
        "    var arr: OwningArray<Res> := [move x]; "
        "    return arr[0].tag + first.tag; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "owned value in `x` was already moved out at this point (§28.3)"));
}

// =============================================================================
// ACCEPT-SIDE SWEEP (LANE_RULES §15) — the new arm adds a REJECTION (a
// `move` element that used to be invisible to `recordMovesIn` is now
// validated), so the FAIL-WITHOUT/PASS-WITH pair above proves only that this
// negative row moved. These rows pin the adjacent legitimate shapes: a
// FRESH-PRODUCER element (no move at all), a LEGAL move (a live, not
// disposed/moved-from, source), a multi-element literal mixing both, and the
// dict-literal form (`keys` + `elements` both populated) — all of which must
// KEEP COMPILING.
// =============================================================================

BEZEL_TEST(record_moves_in_collection_literal_4024,
           accept_collection_literal_fresh_producer_element_compiles_clean) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var arr: OwningArray<Res> := [new Res(1)]; "
        "    return arr[0].tag; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(record_moves_in_collection_literal_4024,
           accept_collection_literal_legal_move_of_live_local_compiles_clean) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(2); "
        "    var arr: OwningArray<Res> := [move x]; "
        "    return arr[0].tag; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A multi-element literal mixing a legal `move` with a fresh producer —
// proves the new arm's per-element recursion does not cross-contaminate
// sibling elements (each is independently walked and marked).
BEZEL_TEST(
    record_moves_in_collection_literal_4024,
    accept_collection_literal_mixed_legal_move_and_fresh_producer_compiles_clean) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(3); "
        "    var arr: OwningArray<Res> := [move x, new Res(4)]; "
        "    return arr[0].tag + arr[1].tag; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The DICT-literal form (`keys` populated in parallel with `elements`) with
// a legal `move` value — proves the arm's `keys` recursion (needed for a
// `move`-as-key shape too) does not disturb the ordinary value-side move.
BEZEL_TEST(record_moves_in_collection_literal_4024,
           accept_dict_literal_legal_move_value_compiles_clean) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var d: OwningDictionary<int, Res> := [1: move x]; "
        "    return d[1].tag; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
