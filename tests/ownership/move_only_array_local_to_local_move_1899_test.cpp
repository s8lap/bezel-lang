// Spec-derived regression for issue #1899: a move-only inline-array
// (§15.1.1 `[N owned]T?`) local-to-local binding `var b := a;` MOVES the whole
// array, leaving the source `a` in moved-from state.
//
// §15.3 (Move-only value types), opening paragraph:
//   "Assignment transfers the value from source to destination, leaving the
//    source in moved-from state. Reading from a moved-from location is a
//    compile error."
// §15.3 bullet 1 "Local-to-local assignment" `var y := x;`:
//   "moves for a move-only value type. After the move, `x` is in moved-from
//    state and may not be read until reassigned."
//
// Before the fix the ownership pass tracked NO moved-from state for this plain-
// binding move (the source stayed live in the flow state), so the issue's repro
//   var a: [1 owned]Cell? := [new Cell(1)];
//   var b: [1 owned]Cell? := a;      // untracked move
//   var c: [1 owned]Cell? := a;      // reads moved-from `a` — MUST be rejected
// compiled clean: `a`, `b`, and `c` all shallow-aliased the one heap Cell and
// each was disposed at scope end — a double/triple free (ASan: heap-use-after-
// free in main). The fix marks the source moved-from at the binding, so the
// second read is the §15.3 compile error and the double-free is never emitted.
//
// The read-of-moved-from rejection is verified through three transfer shapes
// that all re-use `a` after the plain-binding move (`var c := a`, `move a`,
// `return a`); a SINGLE valid move is the positive control (compiles, and
// disposes the cell exactly once, ASan/LSan-clean) that guards against over-
// rejection.

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
// dispose, so `[1 owned]Cell?` is a genuine move-only owning inline array).
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

// True iff some ERROR diagnostic reports a §28.3 moved-out use of `name` — the
// read/move form ("owned value in `name` was already moved out ...") or the
// return/dispose form ("cannot dispose `name`: it was already moved out ...").
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
// REJECTS — a read of the moved-from move-only array after a plain-binding move.
// =============================================================================

// §15.3: the issue's EXACT repro — `var b := a; var c := a;`. The second read of
// the moved-from `a` is a compile error. Without the fix this compiled clean and
// double-freed the one heap Cell at scope end.
BEZEL_TEST(move_only_array_local_move_1899,
           second_read_of_moved_from_array_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [1 owned]Cell? := [new Cell(1)]; "
        "var b: [1 owned]Cell? := a; "
        "var c: [1 owned]Cell? := a; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "a"));
}

// §15.3: the same untracked move admitted `move a` after the plain-binding move;
// the explicit move of the already-moved-from `a` is rejected too.
BEZEL_TEST(move_only_array_local_move_1899,
           explicit_move_after_plain_binding_move_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [1 owned]Cell? := [new Cell(1)]; "
        "var b: [1 owned]Cell? := a; "
        "var c: [1 owned]Cell? := move a; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "a"));
}

// §15.3 / §28.3: returning the moved-from `a` (a whole-array transfer out) after
// the plain-binding move is rejected — `a` was already moved into `b`.
BEZEL_TEST(move_only_array_local_move_1899,
           return_of_moved_from_array_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell "
        "  public constructor(v: int) end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "class C public static method Make(): [1 owned]Cell? "
        "  var a: [1 owned]Cell? := [new Cell(1)]; "
        "  var b: [1 owned]Cell? := a; "
        "  return a; "
        "end Make; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "a"));
}

// =============================================================================
// ACCEPTS — a single well-formed move (no over-rejection).
// =============================================================================

// §15.3: a SINGLE plain-binding move is well-formed — the source `a` is moved-
// from and never read again, and the live target `b` is read. No error (guards
// against over-rejection now that the binding marks the source moved-from).
BEZEL_TEST(move_only_array_local_move_1899,
           single_plain_binding_move_is_accepted_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [1 owned]Cell? := [new Cell(1)]; "
        "var b: [1 owned]Cell? := a; "
        "return b[0]!.V();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.3: a borrow of the array (an index read `a[0]`) is NOT a move — it may
// precede the whole-array move, which only then leaves `a` moved-from. Accepts.
BEZEL_TEST(move_only_array_local_move_1899,
           borrow_before_move_is_accepted_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [1 owned]Cell? := [new Cell(1)]; "
        "var n: int := a[0]!.V(); "
        "var b: [1 owned]Cell? := a; "
        "return n;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.3: a whole-value reassignment restores `a` to a live value; a move after
// the restore is legal again. Accepts.
BEZEL_TEST(move_only_array_local_move_1899,
           move_after_reassignment_restore_is_accepted_spec_15_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [1 owned]Cell? := [new Cell(1)]; "
        "var b: [1 owned]Cell? := a; "
        "a := [new Cell(2)]; "
        "var c: [1 owned]Cell? := a; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// RUNTIME — the valid single move disposes the one heap cell EXACTLY once.
// =============================================================================

// §15.3 / §28: after `var b := a;` the source `a` is transferred (not disposed)
// and the live `b` disposes the one heap Cell exactly once at scope end. A
// double-free would print "D1" twice (and ASan/LSan would flip r.ran to false).
BEZEL_TEST(move_only_array_local_move_1899,
           single_move_disposes_cell_exactly_once_spec_15_3) {
    const char* kSingleMoveRuntime =
        "module M; "
        "class Cell "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  destructor Console.WriteLine(\"D\" + V.ToString()); end destructor; "
        "end Cell; "
        "class C public static method Main(): int "
        "  var a: [1 owned]Cell? := [new Cell(1)]; "
        "  var b: [1 owned]Cell? := a; "
        "  Console.WriteLine(\"body\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(kSingleMoveRuntime);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    // Scope-end disposal follows the body.
    const std::size_t body = r.stdoutText.find("body");
    const std::size_t disp = r.stdoutText.find("D1");
    BEZEL_EXPECT_TRUE(body != std::string::npos);
    BEZEL_EXPECT_TRUE(disp != std::string::npos);
    BEZEL_EXPECT_TRUE(body < disp);
}
