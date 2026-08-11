// Regression for #1634 / §22.4 / §28.8.
//
// An `out owned` parameter slots a not-yet-transferred owned value across the
// callee's body — §22.4 "the callee writes an owned value through the
// parameter; the caller receives ownership at method exit". A mid-body read
// of that parameter (after the assignment, before the return) is a BORROW of
// the slot value, not a use-after-dispose: the value is still sitting in the
// slot, and only transfers to the caller at method exit.
//
// Pre-fix: handleAssignment marked the parameter Disposed immediately on
// `r := new Cell(42)`, so the next read `if r <> null then …;` fired the
// "use after Dispose" warning (false diagnostic). Post-fix: the parameter
// stays Owned across the body (the out-transfer releases the callee-side
// obligation at every exit edge), so mid-body reads are legal borrows.
// endState's fall-through discharge error skips `out owned` parameters too,
// so the caller's compile-time adoption stays clean.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// Wrap a method body in a class with a disposable Cell helper. The
// `r <> null` read after the assignment is the #1634 trigger.
std::string wrap(const std::string& method) {
    return std::string("module M; "
        "class Cell public V: int := 0; "
        "  public constructor(x: int) V := x; end constructor; "
        "end Cell; "
        "class C ") + method + " end C;";
}

} // namespace

// #1634 / §22.4 / §28.8: mid-body read after assignment — the original repro.
BEZEL_TEST(out_owned_mid_body_read_1634, take_then_read_is_borrow) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Take(out owned r: Cell?): bool "
        "  r := new Cell(42); "
        "  if r <> null then return true; end; "
        "  return false; "
        "end Take;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #1634 / §22.4 / §28.8: mid-body read of a class-typed Cell (non-nullable
// declared variant) after assignment.
BEZEL_TEST(out_owned_mid_body_read_1634, take_non_nullable_mid_body_read) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Take(out owned r: Cell): bool "
        "  r := new Cell(42); "
        "  if r.V = 42 then return true; end; "
        "  return false; "
        "end Take;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #1634 / §22.4 / §28.8: a mid-body read between two assignments.
BEZEL_TEST(out_owned_mid_body_read_1634, second_assign_after_first_read) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Take(out owned r: Cell?): bool "
        "  r := new Cell(42); "
        "  var first := r; "
        "  return first <> null; "
        "end Take;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #1634 / §22.4 / §28.8: NEGATIVE — a plain `owned` parameter MUST still be
// flagged as undischarged (it is NOT transferring to a caller — the callee
// owns the obligation). The endState skip is scoped to `out owned` only;
// plain `owned` keeps the existing discharge error.
BEZEL_TEST(out_owned_mid_body_read_1634, plain_owned_still_rejected) {
    auto r = parseAndCheckOwnership(
        std::string("module M; "
        "class Cell public V: int := 0; "
        "  public constructor(x: int) V := x; end constructor; "
        "end Cell; "
        "class C "
        "  public static method Take(owned r: Cell): bool "
        "    r := new Cell(42); "
        "    return true; "
        "  end Take; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "already moved out") ||
        hasErrorMessage(r, "dropped here without disposal") ||
        hasErrorMessage(r, "consumes"));
}
