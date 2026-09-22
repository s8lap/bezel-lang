// Spec-derived regression for issue #1932: a moved-from local read inside the
// INDEX of an ASSIGNMENT TARGET — `a[s.V()] := x` after `move s` — was not
// diagnosed. Extends the #1928(a) family (ee2effc): a purely missing §15.3
// diagnostic (memory-safe — the moved-from value is only READ as an index, not
// reused unsafely), not unsoundness.
//
// Why it was missed:
//   - #1928 half (a) added index-sub-expression `checkRead` recursion only in
//     `checkRead`'s IndexExpr case — i.e. for index expressions in READ
//     positions (`x := a[s.V()]`).
//   - #1928 half (b) added a write-target moved-from check in `handleAssignment`,
//     but it `checkRead`s only the array TARGET (`ix->target`), not the index
//     sub-expressions (`ix->indices`).
//   So a moved-from local in the index of an assignment TARGET fell through both.
//
// §15.3 (Move-only value types), opening paragraph:
//   "Assignment transfers the value from source to destination, leaving the
//    source in moved-from state. Reading from a moved-from location is a
//    compile error."
// §15.3 bullet 1 "Local-to-local assignment" `var y := x;`:
//   "moves for a move-only value type. After the move, `x` is in moved-from
//    state and may not be read until reassigned."
//
// The fix: `handleAssignment`'s IndexExpr-assignment-target path now recurses
// `checkRead` into each index sub-expression, mirroring #1928 half (a)'s
// read-position recursion — an index is a read position regardless of whether it
// appears in a read or a write-target position. Unconditional (not gated to a
// move-only-array target) and TIGHT: `checkRead` reports ONLY for a local
// already in an error state, so a LIVE index never triggers it. The positive
// controls below guard against over-rejection: a live local as an assign-target
// index, a pure arithmetic index, and a live local index of a move-only array
// slot write.

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
// absent (mirrors the #1899/#1925/#1928 harness helper).
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
// REJECTS — a moved-from local read inside an assignment-target index.
// =============================================================================

// §15.3 (#1932): a moved-from CLASS local read as (part of) an assignment-target
// index — `arr[s.V()] := 99` after `move s`, on a copyable value array — is a
// §15.3 read-of-moved-from. Without the fix `handleAssignment`'s index-target
// path never read-checked the index sub-expressions, so `s` was silently read
// after the move. This is the exact read-position shape #1928 half (a) already
// rejects (`x := arr[s.V()]`), now closed for the write-target position too.
BEZEL_TEST(assign_target_index_moved_from_1932,
           moved_class_local_as_value_array_assign_index_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var arr: [3]int := [10, 20, 30]; "
        "var s: Cell := new Cell(1); "
        "using var t: Cell := move s; "
        "arr[s.V()] := 99; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "s"));
}

// §15.3 (#1932): the same read-of-moved-from through an assignment-target index
// on a MOVE-ONLY inline array slot write — `a[s.V()] := new Cell(2)` after
// `move s`. Here half (b)'s array-target read-check does NOT fire (the array `a`
// is live), so the diagnostic comes solely from the new index-sub-expression
// recursion.
BEZEL_TEST(assign_target_index_moved_from_1932,
           moved_class_local_as_move_only_array_slot_index_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "var s: Cell := new Cell(0); "
        "using var t: Cell := move s; "
        "a[s.V()] := new Cell(9); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "s"));
}

// §15.3 (#1932): a whole-moved MOVE-ONLY ARRAY read as (part of) an assignment-
// target index — `arr[src[0]!.V()] := 99` after `var b := src;` — surfaces the
// moved-from `src` through the index's postfix-bang / index / identifier chain,
// mirroring #1928 half (a)'s read-position case in the write-target position.
BEZEL_TEST(assign_target_index_moved_from_1932,
           moved_move_only_array_as_assign_index_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var arr: [3]int := [10, 20, 30]; "
        "var src: [1 owned]Cell? := [new Cell(7)]; "
        "var b: [1 owned]Cell? := src; "
        "arr[src[0]!.V()] := 99; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "src"));
}

// =============================================================================
// ACCEPTS — no over-rejection.
// =============================================================================

// §15.3 (#1932): a LIVE local read as an assignment-target index is well-formed —
// the fix only fires for a local already moved-from. Accepts.
BEZEL_TEST(assign_target_index_moved_from_1932,
           live_local_as_assign_index_is_accepted_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var arr: [3]int := [10, 20, 30]; "
        "var s: Cell := new Cell(1); "
        "arr[s.V()] := 99; "
        "_ := s; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.3 (#1932): a pure arithmetic assignment-target index `arr[i + 1] := x`
// reads no moved-from location — accepts (the fix does not over-reject an
// ordinary index).
BEZEL_TEST(assign_target_index_moved_from_1932,
           arithmetic_assign_index_is_accepted_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var arr: [3]int := [10, 20, 30]; "
        "var i: int := 1; "
        "arr[i + 1] := 99; "
        "return arr[2];"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.3 (#1932): a LIVE local read as the index of a MOVE-ONLY inline array slot
// write is well-formed (guards the gate: the new recursion fires only for a
// moved-from index, never a live one, and never disturbs half (b)'s array-target
// check on a live array). Accepts.
BEZEL_TEST(assign_target_index_moved_from_1932,
           live_local_as_move_only_array_slot_index_is_accepted_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "using var m: Cell? := move a[0]; "
        "var s: Cell := new Cell(0); "
        "a[s.V()] := new Cell(9); "
        "_ := s; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// RUNTIME — the accepted live-index assign-target path disposes each cell once.
// =============================================================================

// §15.3 / §28 (#1932): the accepted `move a[0]; a[s.Idx()] := new Cell(3);`
// shape — an element move empties slot 0 (its value taken by `m`), then a fresh
// value is stored into the emptied slot via a LIVE-local index `s.Idx()` — runs
// and disposes each heap Cell exactly once. Confirms the fix (a pure diagnostic;
// codegen is unchanged) does not disturb a valid slot store: a double-free would
// print a "D" line twice (and ASan/LSan would flip r.ran to false).
BEZEL_TEST(assign_target_index_moved_from_1932,
           valid_live_index_slot_write_disposes_once_spec_15_4) {
    const char* kRuntime =
        "module M; "
        "class Cell "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public method Idx(): int return 0; end Idx; "
        "  destructor Console.WriteLine(\"D\" + V.ToString()); end destructor; "
        "end Cell; "
        "class C public static method Main(): int "
        "  var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "  using var m: Cell? := move a[0]; "
        "  using var s: Cell := new Cell(5); "
        "  a[s.Idx()] := new Cell(3); "
        "  var x: int := a[1]!.V; "
        "  Console.WriteLine(\"m\" + x.ToString()); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(kRuntime);
    if (wasSkipped(r)) return;  // ASan toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Each live cell is disposed exactly once — no double-free, no leak.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D3"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D5"), 1);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("m2") != std::string::npos);
}
