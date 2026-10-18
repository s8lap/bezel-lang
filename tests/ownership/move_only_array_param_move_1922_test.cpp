// Spec-derived regression for issue #1922: a move-only inline-array
// (§15.1.1 `[N owned]T?`) PARAMETER bound local-to-local — `var b := arr;` —
// MOVES the whole array, leaving the source parameter `arr` in moved-from state,
// exactly as #1899 established for a move-only-array LOCAL.
//
// §15.3 (Move-only value types), opening paragraph:
//   "Assignment transfers the value from source to destination, leaving the
//    source in moved-from state. Reading from a moved-from location is a
//    compile error."
// §15.3 bullet 1 "Local-to-local assignment" `var y := x;`:
//   "moves for a move-only value type. After the move, `x` is in moved-from
//    state and may not be read until reassigned."
// §15.3 bullet "Parameter passing":
//   "a parameter declared `(x: V)` ... for a move-only V, it is by value-move
//    (transfer ownership)." — a parameter IS a move-only lvalue, so the local-
//    to-local move rule of bullet 1 applies to it.
//
// #1899 (commit 6b849e1) tracks this move for a LOCAL but its guard returned
// early for a non-local source: a PLAIN move-only-array parameter is not seeded
// into the ownership flow state (a move-only value carries no `owned` discharge
// obligation), so `noteMoveOnlyArrayWholeMoveRead`'s `lookupState` missed it and
// no moved-from state was recorded. The issue's repro
//   Take(arr: [1 owned]Cell?): int
//     var b: [1 owned]Cell? := arr;   // untracked move of the parameter
//     var c: [1 owned]Cell? := arr;   // reads moved-from `arr` — MUST be rejected
// compiled clean (memory-SAFE — the transfer target `b` keeps the storage alive,
// so the cell is disposed exactly once, no double-free — but the §15.3 read-of-
// moved-from diagnostic was missing). The fix seeds the parameter into the flow
// state lazily at the move site, so the second read is the §15.3 compile error.
//
// The read-of-moved-from rejection is verified through three transfer shapes that
// all re-use `arr` after the plain-binding move (`var c := arr`, `move arr`,
// `return arr`) plus a conditional-move-then-read (maybe-moved after the join).
// The positive controls (parameter never moved, single valid move with the
// transfer target used, a copyable value-element array parameter, an element move
// that does NOT mark the whole array, a move on every branch) guard against over-
// rejection; a runtime check confirms the valid single move disposes exactly once
// (ASan/LSan-clean).
//
// NOTE (scope): the issue's literal repro re-reads via `arr[0]` (an index / slot
// read). A slot read of a moved-from move-only array is NOT rejected — for a
// LOCAL either (#1899 catches WHOLE reads, not slot reads); that pre-existing
// slot-read gap is out of scope here. This regression pins the whole-read shapes,
// which is exactly the parameter analogue of the #1899 fix.

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
// `sig` is the `Take` method signature tail (params + `: ReturnType`); `body` is
// its body. A `Main` that supplies a fresh array argument keeps the program whole.
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
// REJECTS — a whole read of the moved-from move-only-array PARAMETER after the
// plain-binding move.
// =============================================================================

// §15.3 (#1922): the issue's repro shape — `var b := arr; var c := arr;`. The
// second whole read of the moved-from parameter `arr` is a compile error. Without
// the fix the plain parameter was untracked and this compiled clean.
BEZEL_TEST(move_only_array_param_move_1922,
           second_whole_read_of_moved_from_param_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(wrapTake(
        "(arr: [1 owned]Cell?): int",
        "var b: [1 owned]Cell? := arr; "
        "var c: [1 owned]Cell? := arr; "
        "return 0;",
        "return Take([new Cell(1)]);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "arr"));
}

// §15.3 (#1922): the same untracked move admitted `move arr` after the plain-
// binding move; the explicit move of the already-moved-from parameter is rejected.
BEZEL_TEST(move_only_array_param_move_1922,
           explicit_move_after_plain_binding_move_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(wrapTake(
        "(arr: [1 owned]Cell?): int",
        "var b: [1 owned]Cell? := arr; "
        "var c: [1 owned]Cell? := move arr; "
        "return 0;",
        "return Take([new Cell(1)]);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "arr"));
}

// §15.3 / §28.3 (#1922): returning the moved-from parameter (a whole-array
// transfer out) after the plain-binding move is rejected — `arr` was already
// moved into `b`.
BEZEL_TEST(move_only_array_param_move_1922,
           return_of_moved_from_param_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(wrapTake(
        "(arr: [1 owned]Cell?): [1 owned]Cell?",
        "var b: [1 owned]Cell? := arr; "
        "return arr;",
        "using var t: [1 owned]Cell? := Take([new Cell(1)]); return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "arr"));
}

// §15.3 / §28.3 (#1922): the parameter is moved on ONE branch, so at the merge it
// is maybe-moved; a whole read after the join is rejected (reading a moved-from —
// on the moved path — location). The lazily-seeded moved-from state propagates
// through the branch join exactly as it does for a local.
BEZEL_TEST(move_only_array_param_move_1922,
           whole_read_after_conditional_move_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(wrapTake(
        "(cond: bool, arr: [1 owned]Cell?): int",
        "if cond then "
        "  var b: [1 owned]Cell? := arr; "
        "end; "
        "var c: [1 owned]Cell? := arr; "
        "return 0;",
        "return Take(true, [new Cell(1)]);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "arr"));
}

// =============================================================================
// ACCEPTS — no over-rejection (the parameter is only marked at a genuine whole
// move; every legal use still compiles).
// =============================================================================

// §15.3 (#1922): a parameter that is NEVER whole-moved is untouched — a slot read
// through it compiles. (Guards against seeding/marking the parameter spuriously.)
BEZEL_TEST(move_only_array_param_move_1922,
           param_never_moved_is_accepted_spec_15_3) {
    auto r = parseAndCheckOwnership(wrapTake(
        "(arr: [1 owned]Cell?): int",
        "return arr[0]!.V();",
        "return Take([new Cell(1)]);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.3 (#1922): a SINGLE plain-binding move of the parameter is well-formed — the
// source `arr` is moved-from and never read again, and the live transfer target
// `b` is read. No error (guards against over-rejection now that the binding marks
// the parameter moved-from).
BEZEL_TEST(move_only_array_param_move_1922,
           single_move_with_target_used_is_accepted_spec_15_3) {
    auto r = parseAndCheckOwnership(wrapTake(
        "(arr: [1 owned]Cell?): int",
        "var b: [1 owned]Cell? := arr; "
        "return b[0]!.V();",
        "return Take([new Cell(1)]);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.3 (#1922): a value-element inline-array parameter (`[2]int`, no `owned`
// marker) is a COPYABLE value type, not move-only — `var b := arr;` copies and a
// second read of `arr` is legal. The fix must not treat this as a move.
BEZEL_TEST(move_only_array_param_move_1922,
           value_element_array_param_is_not_a_move_spec_15_3) {
    auto r = parseAndCheckOwnership(wrapTake(
        "(arr: [2]int): int",
        "var b: [2]int := arr; "
        "var c: [2]int := arr; "
        "return b[0] + c[0];",
        "return Take([1, 2]);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.3 (#1922): a per-slot move (`move arr[i]`) is NOT a whole-array move — it
// leaves the *slot* moved-from, not the whole parameter. Restoring the slot and
// reading through the parameter compiles: the fix must mark only bare-identifier
// WHOLE reads, never an index/slot read.
BEZEL_TEST(move_only_array_param_move_1922,
           element_move_does_not_mark_whole_param_spec_15_3) {
    auto r = parseAndCheckOwnership(wrapTake(
        "(arr: [1 owned]Cell?): int",
        "using var x: Cell? := move arr[0]; "
        "arr[0] := new Cell(9); "
        "return arr[0]!.V();",
        "return Take([new Cell(1)]);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.3 (#1922): moving the parameter on EVERY branch (and never reading it after
// the join) is well-formed — the parameter is moved-from on all paths and no
// subsequent read observes it. No over-rejection at the merge.
BEZEL_TEST(move_only_array_param_move_1922,
           move_on_every_branch_is_accepted_spec_15_3) {
    auto r = parseAndCheckOwnership(wrapTake(
        "(cond: bool, arr: [1 owned]Cell?): int",
        "if cond then "
        "  var b: [1 owned]Cell? := arr; "
        "else "
        "  var b2: [1 owned]Cell? := arr; "
        "end; "
        "return 0;",
        "return Take(true, [new Cell(1)]);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// RUNTIME — the valid single move disposes the one heap cell EXACTLY once.
// =============================================================================

// §15.3 / §28 (#1922): after `var b := arr;` the parameter `arr` is transferred
// (not disposed) and the live `b` disposes the one heap Cell exactly once at scope
// end. A double-free would print "D1" twice (and ASan/LSan would flip r.ran to
// false) — this is the memory-safety invariant the missing diagnostic never broke.
BEZEL_TEST(move_only_array_param_move_1922,
           single_move_disposes_cell_exactly_once_spec_15_3) {
    const char* kSingleMoveRuntime =
        "module M; "
        "class Cell "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  destructor Console.WriteLine(\"D\" + V.ToString()); end destructor; "
        "end Cell; "
        "class C "
        "  public static method Take(arr: [1 owned]Cell?): int "
        "    var b: [1 owned]Cell? := arr; "
        "    Console.WriteLine(\"body\"); "
        "    return 0; "
        "  end Take; "
        "  public static method Main(): int "
        "    return Take([new Cell(1)]); "
        "  end Main; end C;";
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
