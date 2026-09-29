// Spec-derived regression for issue #4636 (spec §28.1 clause 1
// "Where the `owned` modifier can appear", §15.3 "Move-only value types" —
// "Parameter passing"), found while working #4628's §28.9 corpus cluster.
//
// §28.1's admissibility clauses for `owned`:
//   "1. Move-only value types (§15.3): the modifier is admitted-but-
//       redundant — the type's own move semantics already transfer the
//       obligation."
// §15.3's move-only local/parameter rule: an owning inline array
// (`[N owned]T`/`[N owned]T?`) is a move-only value type; assignment (and,
// for a parameter, passing) transfers ownership rather than copying.
//
// PRE-FIX: `MethodOwnership::recordMovesIn` (Ownership_Checks.cpp, the
// s28_3_0020 / #1125 fix) rejected `move <plain-parameter>` for ANY
// parameter whose modifier is not `owned`, on the reasoning "a plain
// parameter carries no obligation to transfer" — correct for a class/
// interface-typed plain parameter (a genuine borrow, the rule's original
// target), but wrong for a move-only VALUE type parameter, where clause 1
// above says the `owned` modifier is merely REDUNDANT: a plain move-only-
// array parameter's bare `move` is the NORMAL, and only meaningful, way to
// transfer it. The pre-fix compiler rejected even the FIRST, entirely legal
// move of such a parameter with the borrow-specific wording ("is a plain
// parameter and carries no obligation to transfer; write `var y := arr` (a
// plain borrow)") — itself nonsensical for a move-only type, since a bare
// read of one IS a move (§15.3), not a borrow.
//
// FIX: `recordMovesIn` now exempts a parameter whose resolved type is
// `common::isMoveOnlyValueType` (the single existing OwnershipFacts
// authority) from the borrow-marker rejection, falling through to the
// pre-existing `markMovedByName` machinery that already handles move-only
// parameter tracking correctly (confirmed unaffected:
// `move_only_array_param_move_1922_test.cpp`'s whole suite, including the
// double-move-is-rejected row, stays green).
//
// per §28.1, §15.3 (issue #4636)

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

std::string wrapTake(const std::string& sig, const std::string& body) {
    return std::string(
        "module M; "
        "class Cell "
        "  public constructor(v: int) end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "class C "
        "  public static method Take") +
        sig + " " + body + " end Take; end C;";
}

// True iff some ERROR diagnostic reports a §28.3 moved-out use of `name`
// (mirrors move_only_array_slot_read_moved_1925_test.cpp's helper).
bool hasMovedOutError(const OwnershipResult& r, const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("moved out") == std::string::npos) continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

}  // namespace

// =============================================================================
// ACCEPTS — the over-rejection this fix closes.
// =============================================================================

// §28.1 clause 1 / §15.3 (#4636): the FIRST, otherwise-legal `move` of a
// plain (non-`owned`) NULLABLE move-only-array parameter is admitted.
BEZEL_TEST(move_only_param_borrow_move_over_reject_4636,
           first_move_of_plain_nullable_array_param_accepted_spec_28_1) {
    auto r = parseAndCheckOwnership(wrapTake(
        "(arr: [1 owned]Cell?): int",
        "var c: [1 owned]Cell? := move arr; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.1 clause 1 / §15.3 (#4636): the NON-nullable owning-inline-array form
// (`[N owned]T`, restoration requires a fresh owned T rather than `null`,
// §15.1.1) is admitted too — the exemption is not nullable-slot-specific.
BEZEL_TEST(move_only_param_borrow_move_over_reject_4636,
           first_move_of_plain_non_nullable_array_param_accepted_spec_28_1) {
    auto r = parseAndCheckOwnership(wrapTake(
        "(arr: [1 owned]Cell): int",
        "var c: [1 owned]Cell := move arr; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// REJECTS — a genuine double-move still draws the RIGHT diagnostic (not the
// borrow-rejection wording this fix removes for move-only types).
// =============================================================================

// §15.3 (#4636, same shape as #1922's explicit_move_after_plain_binding_move):
// a bare read of a move-only-array parameter IS itself a move (§15.3
// "Parameter passing") — `var b := arr;` already consumes `arr`, so the
// following `move arr` is a SECOND move of an already-moved-from binding and
// must be rejected as "moved out", not as "not admitted on a borrow".
BEZEL_TEST(move_only_param_borrow_move_over_reject_4636,
           second_move_after_implicit_bare_read_move_rejected_moved_out) {
    auto r = parseAndCheckOwnership(wrapTake(
        "(arr: [1 owned]Cell?): int",
        "var b: [1 owned]Cell? := arr; "
        "var c: [1 owned]Cell? := move arr; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "arr"));
    // The bug's own symptom: the wrong (borrow-rejection) diagnostic must
    // NOT be the one that fires here.
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, "not admitted on a borrow"));
}

// =============================================================================
// CONTROL — the original s28_3_0020 / #1125 rule is UNCHANGED for a genuine
// borrow: a plain CLASS-typed parameter still rejects `move`.
// =============================================================================

BEZEL_TEST(move_only_param_borrow_move_over_reject_4636,
           plain_class_typed_parameter_move_still_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(wrapTake(
        "(w: Cell): int",
        "var b: Cell := move w; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "not admitted on a borrow"));
}
