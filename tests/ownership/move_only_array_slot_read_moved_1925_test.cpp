// Spec-derived regression for issue #1925: a SLOT read `a[i]` of a move-only
// inline array (§15.1.1 `[N owned]T?`) that has been WHOLE-moved out of its
// location is a §15.3 read-of-moved-from error — for both a LOCAL and a
// PARAMETER source.
//
// §15.3 (Move-only value types), opening paragraph:
//   "Assignment transfers the value from source to destination, leaving the
//    source in moved-from state. Reading from a moved-from location is a
//    compile error."
// §15.3 bullet 1 "Local-to-local assignment" `var y := x;`:
//   "moves for a move-only value type. After the move, `x` is in moved-from
//    state and may not be read until reassigned."
//
// #1899 (commit 6b849e1) marks a move-only-array LOCAL moved-from on a whole
// move `var b := a;`, and #1922 extends the same marking to a PARAMETER source.
// Both catch only WHOLE re-reads (`var c := x`, `move x`, `return x`) via
// `checkRead` on the bare identifier. A SLOT read `x[i]` went through the
// index-read path, which had no case in `checkRead` at all, so it never
// consulted the moved-from state of the index target:
//   var a: [1 owned]Cell? := [new Cell(1)];
//   var b: [1 owned]Cell? := a;   // whole-move: `a` is moved-from
//   return a[0]!.V();             // slot read of moved-from `a` — NOT diagnosed
// compiled clean for a local AND for a parameter. Memory-SAFE (the transfer
// target keeps the storage alive, so the cell is disposed exactly once — no
// double-free), a purely missing §15.3 diagnostic. The fix teaches `checkRead`
// to recurse a move-only inline-array index (and the null-forgiving `a[i]!`)
// into its target, so the target's moved-from state is consulted and the slot
// read is the §28.3 use-after-move error §15.3 mandates.
//
// The rejection is verified for a local and a parameter, in both a binding and a
// return position, with and without the `!` unwrap, and after a conditional
// (maybe-moved) move. The positive controls guard against over-rejection: a
// slot read WITHOUT a prior move; a slot read of the still-live transfer TARGET;
// an element move `move a[i]` (which leaves the ARRAY live and marks only that
// slot, so the other slots stay readable); and a copyable value-element array
// `[2]int` whose `var b := a` is a COPY, not a move. A runtime check confirms
// the valid move + live-target slot read disposes the one heap cell exactly once
// (ASan/LSan-clean).

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"        // runAotChecked — AOT + ASan/LSan oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// `class Cell` is a disposable-shaped class (a destructor makes it non-trivial-
// dispose, so `[N owned]Cell?` is a genuine move-only owning inline array).
// `body` is the Main method body.
std::string wrap(const std::string& body) {
    return std::string(
        "module M; "
        "class Cell "
        "  public constructor(v: int) end constructor; "
        "  public method V(): int return 0; end V; "
        "  destructor end destructor; "
        "end Cell; "
        "class C public static method Main(): int ") +
        body + " end Main; end C;";
}

// A `Take` method (params + `: ReturnType` via `sig`, body via `body`) plus a
// `Main` that supplies a fresh array argument so the program is whole.
std::string wrapTake(const std::string& sig, const std::string& body,
                     const std::string& mainBody) {
    return std::string(
        "module M; "
        "class Cell "
        "  public constructor(v: int) end constructor; "
        "  public method V(): int return 0; end V; "
        "  destructor end destructor; "
        "end Cell; "
        "class C "
        "  public static method Take") +
        sig + " " + body + " end Take; " +
        "  public static method Main(): int " + mainBody + " end Main; end C;";
}

// True iff some ERROR diagnostic reports a §28.3 moved-out use of `name`.
bool hasMovedOutError(const OwnershipResult& r, const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("moved out") == std::string::npos) continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

}  // namespace

// =============================================================================
// REJECTS — a SLOT read of the moved-from move-only array (local and param).
// =============================================================================

// §15.3 (#1925): the issue's LOCAL repro — `var b := a; ... a[0]!.V();`. The slot
// read of the moved-from `a` is a compile error. Without the fix it compiled
// clean (the index-read path never consulted `a`'s moved-from state).
BEZEL_TEST(move_only_array_slot_read_moved_1925,
           local_slot_read_after_whole_move_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [1 owned]Cell? := [new Cell(1)]; "
        "var b: [1 owned]Cell? := a; "
        "var x: int := a[0]!.V(); "
        "return x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "a"));
}

// §15.3 (#1925): the same slot read in a RETURN position — `return a[0]!.V();`
// after the whole move — is rejected too (the read reaches the moved-from `a`
// through the `!.V()` call/member/postfix-bang chain).
BEZEL_TEST(move_only_array_slot_read_moved_1925,
           local_slot_read_in_return_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [1 owned]Cell? := [new Cell(1)]; "
        "var b: [1 owned]Cell? := a; "
        "return a[0]!.V();"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "a"));
}

// §15.3 (#1925): a BARE slot read (no `!` unwrap) — `var y: Cell? := a[0];` —
// after the whole move is rejected too; the fix does not depend on the postfix
// bang being present.
BEZEL_TEST(move_only_array_slot_read_moved_1925,
           local_bare_slot_read_after_whole_move_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [1 owned]Cell? := [new Cell(1)]; "
        "var b: [1 owned]Cell? := a; "
        "var y: Cell? := a[0]; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "a"));
}

// §15.3 (#1925): the issue's PARAMETER repro — `Take(arr) { var b := arr; return
// arr[0]!.V(); }`. The slot read of the moved-from parameter `arr` is rejected
// (the #1922 lazy seeding leaves `arr` moved-from; the slot read now consults it).
BEZEL_TEST(move_only_array_slot_read_moved_1925,
           param_slot_read_after_whole_move_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(wrapTake(
        "(arr: [1 owned]Cell?): int",
        "var b: [1 owned]Cell? := arr; "
        "return arr[0]!.V();",
        "return Take([new Cell(1)]);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "arr"));
}

// §15.3 / §28.3 (#1925): the parameter is whole-moved on ONE branch, so at the
// merge it is maybe-moved; a slot read after the join is rejected (on the moved
// path it reads a moved-from location). The moved-from state propagates through
// the branch join to the index-read site.
BEZEL_TEST(move_only_array_slot_read_moved_1925,
           param_slot_read_after_conditional_move_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(wrapTake(
        "(cond: bool, arr: [1 owned]Cell?): int",
        "if cond then "
        "  var b: [1 owned]Cell? := arr; "
        "end; "
        "return arr[0]!.V();",
        "return Take(true, [new Cell(1)]);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "arr"));
}

// =============================================================================
// ACCEPTS — no over-rejection.
// =============================================================================

// §15.3 (#1925): a slot read WITHOUT any prior move is well-formed — `a` is live,
// so `a[0]` reads a live location. Accepts (guards against over-rejection now
// that a move-only-array index consults the target's moved-from state).
BEZEL_TEST(move_only_array_slot_read_moved_1925,
           slot_read_without_prior_move_is_accepted_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [1 owned]Cell? := [new Cell(1)]; "
        "var x: int := a[0]!.V(); "
        "return x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.3 (#1925): after `var b := a;` the transfer TARGET `b` is live — a slot
// read of `b` is well-formed. Only the moved-from SOURCE `a` is rejected. Accepts.
BEZEL_TEST(move_only_array_slot_read_moved_1925,
           slot_read_of_live_transfer_target_is_accepted_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [1 owned]Cell? := [new Cell(1)]; "
        "var b: [1 owned]Cell? := a; "
        "return b[0]!.V();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.3 (#1925): an ELEMENT move `move a[0]` moves only that slot and leaves the
// ARRAY live (Owned/Borrowed, NOT MovedFrom) — the other slots stay readable. A
// slot read of a different slot `a[1]` is accepted (guards the gate: the fix
// fires only after a WHOLE move, never after a partial slot move).
BEZEL_TEST(move_only_array_slot_read_moved_1925,
           slot_read_after_element_move_is_accepted_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "using var m: Cell? := move a[0]; "
        "var x: int := a[1]!.V(); "
        "return x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.3 (#1925): a COPYABLE value-element inline array `[2]int` is NOT move-only —
// `var b := a` COPIES it (both stay live), so a slot read of `a` afterward is
// well-formed. Accepts (the fix's gate is move-only-inline-array-only, so it
// does not touch a copyable value array).
BEZEL_TEST(move_only_array_slot_read_moved_1925,
           slot_read_of_copyable_value_array_is_accepted_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [2]int := [10, 20]; "
        "var b: [2]int := a; "
        "var x: int := a[0]; "
        "return x + b[1];"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// RUNTIME — the valid move + live-target slot read disposes the cell once.
// =============================================================================

// §15.3 / §28 (#1925): after `var b := a;` the source `a` is transferred (not
// disposed) and the live `b` — read via a valid slot read `b[0]` — disposes the
// one heap Cell exactly once at scope end. A double-free would print "D1" twice
// (and ASan/LSan would flip r.ran to false). Confirms the fix rejects the bad
// slot read of the SOURCE without breaking a valid slot read of the TARGET.
BEZEL_TEST(move_only_array_slot_read_moved_1925,
           valid_move_then_target_slot_read_disposes_once_spec_15_3) {
    const char* kRuntime =
        "module M; "
        "class Cell "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public method Get(): int return V; end Get; "
        "  destructor Console.WriteLine(\"D\" + V.ToString()); end destructor; "
        "end Cell; "
        "class C public static method Main(): int "
        "  var a: [1 owned]Cell? := [new Cell(1)]; "
        "  var b: [1 owned]Cell? := a; "
        "  Console.WriteLine(\"n\" + b[0]!.Get().ToString()); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(kRuntime);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    // The live-target slot read ran and saw the moved value.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("n1") != std::string::npos);
    // Scope-end disposal follows the body read.
    const std::size_t body = r.stdoutText.find("n1");
    const std::size_t disp = r.stdoutText.find("D1");
    BEZEL_EXPECT_TRUE(body != std::string::npos);
    BEZEL_EXPECT_TRUE(disp != std::string::npos);
    BEZEL_EXPECT_TRUE(body < disp);
}
