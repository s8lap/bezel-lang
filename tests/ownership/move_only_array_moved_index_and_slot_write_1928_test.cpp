// Spec-derived regression for issue #1928: two residual §15.3 under-rejections
// in the move-only inline-array tracking, both surfaced during the pre-gate
// review of #1925 (a67fc8b) and both PRE-EXISTING (a purely missing diagnostic,
// not unsoundness):
//
//   (a) A moved-from local used AS an index. `a[<expr reading a moved-from
//       local>]` — e.g. `a[s.V()]` after `move s`, or `a[src[0]!.V()]` after a
//       whole move of the move-only array `src` — was not diagnosed. #1925's new
//       `IndexExpr` case in `checkRead` recursed into the index TARGET (the array
//       `a`) but never into `ix->indices` (the index sub-expressions), so a
//       moved-from local surfaced through the index went unchecked for EVERY
//       array kind (copyable value array, ordinary collection, move-only array).
//
//   (b) A slot WRITE after a whole move. `var b := a; a[0] := new Cell();` —
//       writing to a slot of a whole-moved move-only inline array — was accepted.
//       This is a separate index-assignment-TARGET path (`handleAssignment`), not
//       `checkRead`, and #1925 left it unchanged.
//
// §15.3 (Move-only value types), opening paragraph:
//   "Assignment transfers the value from source to destination, leaving the
//    source in moved-from state. Reading from a moved-from location is a
//    compile error."
// §15.3 bullet 1 "Local-to-local assignment" `var y := x;`:
//   "moves for a move-only value type. After the move, `x` is in moved-from
//    state and may not be read until reassigned."
//
// #1899 (491c8c8) marks a move-only-array LOCAL moved-from on a whole move
// `var b := a;`, and #1922 (b617d41) extends the same marking to a PARAMETER
// source; #1925 (a67fc8b) rejects a slot READ `a[i]` of such a moved-from array.
// This fix closes the two remaining gaps:
//   (a) `checkRead`'s IndexExpr case now recurses into each index sub-expression
//       (an index is always a read position), so the existing identifier /
//       member / call / postfix-bang recursion surfaces any moved-from local read
//       inside the index — the §28.3 use-after-move §15.3 mandates.
//   (b) `handleAssignment` now read-checks the index TARGET of a move-only
//       inline-array SLOT WRITE, mirroring #1925's read gate, so a write into a
//       whole-moved array is the same §28.3 use-after-move.
//
// Both are TIGHT: (b) is gated to a move-only inline-array target (a live array
// or one with only an element moved `move a[i]` — which leaves the ARRAY live —
// is untouched); (a) is safe unconditionally because `checkRead` reports ONLY for
// a local already in an error state, so a live index never triggers it. The
// positive controls below guard against over-rejection: a live index, a slot
// write on a non-moved array, an element move `move a[i]` followed by a write and
// a read of another slot, a reassigned (restored) array, and a copyable value
// array with a live index. A runtime check confirms the valid slot write on a
// live array still disposes each heap cell exactly once (ASan/LSan-clean).

#include "support/aot_support.hpp"       // runAotChecked — AOT + ASan/LSan oracle
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

// A checked AOT run is SKIPPED (not failed) when the ASan link toolchain is
// absent (mirrors the #1899/#1925 harness helper).
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
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
// REJECTS — half (a): a moved-from local read inside the INDEX sub-expression.
// =============================================================================

// §15.3 (#1928 half a): a moved-from CLASS local read as (part of) an index —
// `a[s.V()]` after `move s` — is a §15.3 read-of-moved-from. Without the fix the
// index sub-expression was never read-checked (checkRead's IndexExpr case
// recursed only into the array target), so `s` was silently read after the move.
BEZEL_TEST(move_only_array_moved_index_and_slot_write_1928,
           moved_class_local_as_index_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "var s: Cell := new Cell(0); "
        "using var t: Cell := move s; "
        "var x: int := a[s.V()]!.V(); "
        "return x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "s"));
}

// §15.3 (#1928 half a): a whole-moved MOVE-ONLY ARRAY read as (part of) an index
// — `a[src[0]!.V()]` after `var b := src;` — is a §15.3 read-of-moved-from. The
// moved-from `src` surfaces through the index's postfix-bang / index / identifier
// chain now that the index sub-expression is read-checked.
BEZEL_TEST(move_only_array_moved_index_and_slot_write_1928,
           moved_move_only_array_as_index_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [1 owned]Cell? := [new Cell(1)]; "
        "var src: [1 owned]Cell? := [new Cell(7)]; "
        "var b: [1 owned]Cell? := src; "
        "var x: int := a[src[0]!.V()]!.V(); "
        "return x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "src"));
}

// §15.3 (#1928 half a): the fix is general, not gated to a move-only-array target
// — a moved-from local read as the index of a COPYABLE value array `[N]int` is
// rejected too (the index is a read position regardless of what is indexed).
BEZEL_TEST(move_only_array_moved_index_and_slot_write_1928,
           moved_local_as_value_array_index_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var arr: [3]int := [10, 20, 30]; "
        "var s: Cell := new Cell(1); "
        "using var t: Cell := move s; "
        "var x: int := arr[s.V()]; "
        "return x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "s"));
}

// =============================================================================
// REJECTS — half (b): a slot WRITE into a whole-moved move-only array.
// =============================================================================

// §15.3 (#1928 half b): the issue's repro — `var b := a; a[0] := new Cell();`.
// The slot write into the moved-from `a` is a compile error (§15.3: the source
// "may not be read until reassigned"; a slot store is such a use). Without the
// fix the index-assignment-target path never consulted `a`'s moved-from state.
BEZEL_TEST(move_only_array_moved_index_and_slot_write_1928,
           slot_write_after_whole_move_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [1 owned]Cell? := [new Cell(1)]; "
        "var b: [1 owned]Cell? := a; "
        "a[0] := new Cell(2); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "a"));
}

// =============================================================================
// ACCEPTS — no over-rejection.
// =============================================================================

// §15.3 (#1928 half a): a LIVE local read as an index is well-formed — the fix
// only fires for a local already moved-from. Accepts.
BEZEL_TEST(move_only_array_moved_index_and_slot_write_1928,
           live_local_as_index_is_accepted_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "var s: Cell := new Cell(0); "
        "var x: int := a[s.V()]!.V(); "
        "_ := s; "
        "return x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.3 (#1928 half b): a slot write into a NON-moved (live) array is well-formed.
// Accepts (guards the gate: the write check fires only after a WHOLE move).
BEZEL_TEST(move_only_array_moved_index_and_slot_write_1928,
           slot_write_on_live_array_is_accepted_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [1 owned]Cell? := [new Cell(1)]; "
        "a[0] := new Cell(2); "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.3 (#1928 half b): an ELEMENT move `move a[0]` moves only that slot and
// leaves the ARRAY live (Owned/Borrowed, NOT MovedFrom) — so writing the emptied
// slot AND reading a different slot are both well-formed. Accepts (guards the
// gate: the write check fires only after a WHOLE move, never an element move).
BEZEL_TEST(move_only_array_moved_index_and_slot_write_1928,
           slot_write_and_read_after_element_move_is_accepted_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "using var m: Cell? := move a[0]; "
        "a[0] := new Cell(3); "
        "var x: int := a[1]!.V(); "
        "return x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.3 (#1928 half b): after a whole move, REASSIGNING the array restores it to
// live — a subsequent slot write is well-formed (the moved-from mark is cleared
// on reassignment). Accepts.
BEZEL_TEST(move_only_array_moved_index_and_slot_write_1928,
           slot_write_after_reassign_is_accepted_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [1 owned]Cell? := [new Cell(1)]; "
        "var b: [1 owned]Cell? := a; "
        "a := [new Cell(9)]; "
        "a[0] := new Cell(2); "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.3 (#1928 half a): a LIVE local read as the index of a copyable value array
// is well-formed. Accepts (the fix does not over-reject an ordinary index).
BEZEL_TEST(move_only_array_moved_index_and_slot_write_1928,
           live_local_as_value_array_index_is_accepted_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var arr: [3]int := [10, 20, 30]; "
        "var s: Cell := new Cell(1); "
        "var x: int := arr[s.V()]; "
        "_ := s; "
        "return x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// RUNTIME — the accepted element-move-then-slot-write path disposes each once.
// =============================================================================

// §15.3 / §28 (#1928 half b): the accepted `move a[0]; a[0] := new Cell(3);`
// shape — an element move empties the slot (its value taken by `m`), then a fresh
// value is stored into the emptied slot — runs and disposes each heap Cell
// exactly once: `m` disposes the moved-out Cell 1, and the scope-end walk
// disposes the stored Cell 3 (a[0]) and the untouched Cell 2 (a[1]). Confirms the
// fix (a pure diagnostic; codegen is unchanged) does not disturb a valid slot
// store: a double-free would print a "D" line twice (and ASan/LSan would flip
// r.ran to false). (An overwrite of an OCCUPIED slot leaks the displaced value —
// a separate, pre-existing owning-slot-overwrite bug this test deliberately does
// not exercise, filed alongside #1928.)
BEZEL_TEST(move_only_array_moved_index_and_slot_write_1928,
           valid_element_move_then_slot_write_disposes_once_spec_15_4) {
    const char* kRuntime =
        "module M; "
        "class Cell "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  destructor Console.WriteLine(\"D\" + V.ToString()); end destructor; "
        "end Cell; "
        "class C public static method Main(): int "
        "  var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "  using var m: Cell? := move a[0]; "
        "  a[0] := new Cell(3); "
        "  var x: int := a[1]!.V; "
        "  Console.WriteLine(\"m\" + x.ToString()); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(kRuntime);
    if (wasSkipped(r)) return;  // ASan toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Each of the three live cells is disposed exactly once — no double-free,
    // no leak (ASan/LSan-clean).
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D3"), 1);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("m2") != std::string::npos);
}
