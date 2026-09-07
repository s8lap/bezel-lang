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

namespace {

constexpr char kDisposedMoveError[] =
    "cannot move from disposed local `x` (§28.3)";

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
// correctly rejected before this fix (it routes through `recordMovesIn`'s
// existing `UnaryExpr` arm, not the new `CollectionLiteralExpr` one) and
// must stay rejected with the IDENTICAL diagnostic text.
// =============================================================================
BEZEL_TEST(record_moves_in_collection_literal_4024,
           direct_move_of_disposed_local_rejected_control) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    using var y := move x; "
        "    return y.tag; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kDisposedMoveError));
}

// =============================================================================
// NEGATIVE (issue #4024's own repro) — `move x` as a bare COLLECTION-LITERAL
// element (`OwningArray<Res>` sequence-literal form).
// FAIL-WITHOUT: `r.diagnostics.hasErrors()` is FALSE (rc=0, zero
// diagnostics) — measured pre-fix.
// PASS-WITH: flips true, with the SAME diagnostic text the control case
// gets.
// =============================================================================
BEZEL_TEST(record_moves_in_collection_literal_4024,
           collection_literal_element_move_of_disposed_local_rejected) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    var arr: OwningArray<Res> := [move x]; "
        "    return arr[0].tag; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kDisposedMoveError));
}

// The same case through the STRONG AOT oracle (real `bezelc` + link + exec):
// proves not merely that a diagnostic fired, but that NO EXECUTABLE was ever
// produced (the pre-fix program compiled to a real, SEGV-on-run binary).
BEZEL_TEST(record_moves_in_collection_literal_4024,
           aot_collection_literal_element_move_of_disposed_local_rejected_no_program) {
    JitResult r = runAot(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    var arr: OwningArray<Res> := [move x]; "
        "    return arr[0].tag; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
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
