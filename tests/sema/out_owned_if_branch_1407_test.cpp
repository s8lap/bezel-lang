// fix #1407 / §28.3: if-condition out-owned-var discharged in the then-branch.
//
// An `out owned var item` declared in an if-condition and discharged via
// `item?.Dispose()` INSIDE the then-branch (fall-through case) was spuriously
// warned/flagged by the ownership pass. The false edge of the Branch block
// carried the post-condition state (item = Owned) rather than the correct
// pre-condition state (item not written → PolyNull). The join then saw
// Disposed ⊓ Owned = Owned (obligation retained), firing the warning.
//
// Fix (§28.3, #1407): for a non-loop if/elif Branch block that declares
// `out owned var` locals, foldPred demotes those locals to PolyNull on the
// false edge (succs[1]) before the meet. The correct meet is then
// Disposed ⊓ PolyNull = Disposed — the obligation is satisfied.
//
// Key guardrails:
//   POSITIVE: dispose inside then-branch (fall-through) → ACCEPT / no warning
//   POSITIVE: dispose inside then-branch with real code after if → ACCEPT
//   NEGATIVE: item NOT disposed in either branch → still auto-drop warned
//   NEGATIVE: then-branch does other work, no disposal → still warned
//
// // per §28.3, #1407

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;
using bezel::test::hasAutoDropWarning;

namespace {

// Wrap in a method body with a pre-pushed OwningStack<Cell>.
std::string withMain(const std::string& body) {
    return std::string(
        "module M; "
        "class Cell "
        "  public static _n: int := 0; "
        "  public constructor() Cell._n := Cell._n + 1; end constructor; "
        "  destructor Cell._n := Cell._n - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Main(): int "
        "    do ") +
        body +
        "    end; "
        "    return Cell._n; "
        "  end Main; "
        "end C;";
}

} // namespace

// §28.3 / #1407 (PRIMARY POSITIVE): `item?.Dispose()` in the then-branch of
// an if whose condition declares the item via `out owned var`. The false edge
// carries PolyNull (TryPop returned false → item not written); the then-branch
// exits Disposed; Disposed ⊓ PolyNull = Disposed → obligation satisfied.
// Must ACCEPT with no errors and no auto-drop warning.
BEZEL_TEST(out_owned_if_branch_1407, dispose_in_then_branch_accepted) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new OwningStack<Cell>(); "
        "s.Push(move (new Cell())); "
        "if s.TryPop(out owned var item) then "
        "  item?.Dispose(); "
        "end; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "item"));
}

// §28.3 / #1407 (FALL-THROUGH + CODE AFTER): same as above but with additional
// code after the if-statement (another Push to verify the stack is usable).
// The join state after the if must be Disposed (not Owned), so there is no
// lingering obligation past the if — must ACCEPT.
BEZEL_TEST(out_owned_if_branch_1407, dispose_in_then_branch_code_after_accepted) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new OwningStack<Cell>(); "
        "s.Push(move (new Cell())); "
        "if s.TryPop(out owned var item) then "
        "  item?.Dispose(); "
        "end; "
        "s.Push(move (new Cell())); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "item"));
}

// §28.3 / #1407 (NEGATIVE GUARDRAIL — no disposal): item received in the
// then-branch is NOT disposed. The then-branch exits Owned, false-edge is
// PolyNull; meet = Owned (PolyNull absorbs into Owned, keeping Owned).
// The obligation surfaces: auto-drop warning expected.
// Guards that the fix does NOT over-relax genuine leaks.
BEZEL_TEST(out_owned_if_branch_1407, no_dispose_still_warned) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new OwningStack<Cell>(); "
        "s.Push(move (new Cell())); "
        "if s.TryPop(out owned var item) then "
        "end; "));
    // The affine auto-drop rule (#1387) issues a must-use WARNING (not an
    // error) and registers scope-end disposal.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "item"));
}

// §28.3 / #1407 (NEGATIVE GUARDRAIL — multi-element then-body, no disposal):
// the then-branch does other work (pushes another element) but never discharges
// item. Must still warn — the obligation carries through regardless.
BEZEL_TEST(out_owned_if_branch_1407, other_work_in_then_no_dispose_still_warned) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new OwningStack<Cell>(); "
        "s.Push(move (new Cell())); "
        "if s.TryPop(out owned var item) then "
        "  s.Push(move (new Cell())); "
        "end; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "item"));
}
