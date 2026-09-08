// Spec-derived regression for issue #4065: `checkRead`'s `IndexExpr` arm never
// read-checks an ORDINARY indexer target — only a move-only inline-array target
// (`isMoveOnlyInlineArrayType`, gated for #1925) reached the moved-from /
// disposed state lookup, leaving every ordinary indexer READ of a moved-from or
// disposed collection receiver (an `OwningArray<T>` local after a whole move,
// or any collection after a dominating `Dispose()`) silently accepted —
// measured in the issue to compile clean with zero diagnostics and to
// heap-use-after-free under ASan.
//
// §15.3 (Move-only value types), opening paragraph:
//   "Assignment transfers the value from source to destination, leaving the
//    source in moved-from state. Reading from a moved-from location is a
//    compile error."
// §28.2 v0.131 (use-after-dispose):
//   "used (read, borrowed from, or consumed) after an explicit `Dispose()`
//    call … that dominates the use site on every path."
// §28.3 K1.3:
//   "the binding or slot is unreadable until reassigned"
//
// #1925 (a67fc8b) closed the gap for the MOVE-ONLY INLINE-ARRAY case by gating
// `checkRead`'s `IndexExpr` arm's target recursion on `isMoveOnlyInlineArrayType`
// and consulting the moved-from state from there. That gate was the right shape
// for #1925's slot-read repro (`var b := a; return a[0]!.V();`) but never fired
// for an ORDINARY indexer (a class-typed collection like `OwningArray<T>` or a
// user-defined indexer). The receiver of an index expression is a READ
// position exactly as the receiver of a member expression is — every sibling
// arm in `checkRead` (`MemberAccessExpr`, `PostfixBangExpr`, `UnaryExpr`)
// recurses into its operand unconditionally; only `IndexExpr` was gated, and
// only to a single element-type family. This fix widens the gate to read-check
// the target UNCONDITIONALLY (a live Owned/Borrowed local silently passes;
// MovedFrom/Disposed/maybe-consumed/Uninitialized each fire the diagnostic the
// IdentifierExpr arm already emits) — matching the sibling arms.
//
// The widening is sound for the prior gate's positive controls (a slot read
// WITHOUT any prior move, a slot read of the live transfer target, a slot
// read of a different slot after `move a[i]`, a copyable value-element array)
// — every one of those still passes through Owned state and out silently —
// which are reasserted as ACCEPTS below. Two NEW REJECTS cover the new
// behaviour: a slot read off a disposed ordinary indexer, and a slot read
// after a whole-move-style destruction of the receiver (a using-var scope
// exit acts as a dispose-and-drop; the slot read off the dead binding
// exercises the unreadable-until-reassigned clause in its K1.3 form). A
// runtime check confirms the live `OwningArray<T>` slot read still disposes
// each heap cell exactly once.

#include "support/aot_support.hpp"       // runAotChecked — AOT + ASan/LSan oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// A `class Cell` (disposable, so an `[N owned]Cell?` is a real move-only
// inline array AND an `OwningArray<Cell>` is a real class-typed indexer
// collection). `body` is the Main method body.
std::string wrap(const std::string& body) {
    return std::string(
        "module M; "
        "class Cell "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "class C public static method Main(): int ") +
        body + " end Main; end C;";
}

// A `Reader` setup with `body` inserted into a Main that disposes `arr` and
// passes `arr[0]` as an argument. Used for the slot-read-after-Dispose repro
// that exercises the WIDENED `checkRead`'s `IndexExpr` arm via an argument
// walk — the slot read happens BEFORE the call, so the argument's own
// `IndexExpr` recurses into `arr` (the disposed local) and must fire. //
// per §28.2, v0.294, #4065
std::string wrapOwning(const std::string& body) {
    return std::string(
        "module M; "
        "class Cell "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "class Reader "
        "  public static method Peek(c: Cell): int "
        "    return c.V; "
        "  end Peek; "
        "end Reader; "
        "class C public static method Main(): int ") +
        body + " end Main; end C;";
}

// True iff some WARNING diagnostic names `name` after a dominating Dispose
// (the §28.2 use-after-dispose message text). Scans Warning-severity directly
// because `hasErrorMessage` deliberately filters warnings.
bool hasDisposeWarningFor(const OwnershipResult& r, const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("after a dominating explicit Dispose()") ==
                std::string::npos) continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
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
// REJECTS — a slot read of a moved-from / disposed ordinary indexer receiver.
// =============================================================================

// §28.2 (#4065): a slot read off a DOMINATED-DISPOSED `OwningArray<T>` is a
// use-after-dispose. Before the fix this compiled clean — `checkRead`'s
// `IndexExpr` arm only consulted the moved-from state of a move-only inline
// array, never an ordinary class-typed collection, so the disposed `arr`
// was invisible to the indexer read. Widen the gate so the index TARGET is
// read-checked unconditionally; the IdentifierExpr arm fires the §28.2
// use-after-dispose warning with a Note pointing to the explicit Dispose()
// call site. // per §28.2, v0.294, #4065
BEZEL_TEST(check_read_index_expr_target_4065,
           slot_read_of_disposed_owning_array_rejected_spec_28_2) {
    auto r = parseAndCheckOwnership(wrap(
        "using var arr: OwningArray<Cell> := [new Cell(1)]; "
        "arr.Dispose(); "
        "var x: int := arr[0]!.V; "
        "return x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors() ||
                      hasDisposeWarningFor(r, "arr"));
    // The diagnostic must name the receiver, not just the slot — this is what
    // the gate was failing to consult.
    BEZEL_EXPECT_TRUE(hasDisposeWarningFor(r, "arr"));
}

// §28.2 (#4065): the issue's own second example — `Reader.Peek(arr[0])` off a
// disposed `arr`. The slot read `arr[0]` happens INSIDE the call argument, so
// the argument-walk `checkRead` reaches the `IndexExpr` arm and (post-fix)
// recurses into `arr`. Before the fix the argument's IndexExpr-arm gate was
// the only place a moved-from/disposed indexer receiver would be visible to
// the read-check, so this shape — the most direct restatement of the issue —
// silently accepted a use-after-dispose. // per §28.2, v0.294, #4065
BEZEL_TEST(check_read_index_expr_target_4065,
           slot_read_of_disposed_owning_array_as_arg_rejected_spec_28_2) {
    auto r = parseAndCheckOwnership(wrapOwning(
        "using var arr: OwningArray<Cell> := [new Cell(1)]; "
        "arr.Dispose(); "
        "return Reader.Peek(arr[0]!);"));
    BEZEL_EXPECT_TRUE(hasDisposeWarningFor(r, "arr"));
}

// §28.3 K1.3 (#4065): an UNREADABLE local (one whose whole value has been
// transferred out by a move-only inline array's whole-move path) reached
// through its indexer. This is the moved-out-side twin of the dispose row
// above — the indexer's receiver is the binding's slot just as much as a
// member-access receiver is, so reading it surfaces the §28.3 moved-out
// diagnostic the IdentifierExpr arm emits. (The original #1925 covered
// the SAME move-only-array shape; this regression re-covers it through the
// ORDINARY indexer path, which the gate previously skipped.) // per §28.3
// K1.3, v0.294, #1925, #4065
BEZEL_TEST(check_read_index_expr_target_4065,
           slot_read_after_whole_move_local_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [1 owned]Cell? := [new Cell(1)]; "
        "var b: [1 owned]Cell? := a; "
        "var x: int := a[0]!.V; "
        "return x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "a"));
}

// §28.3 K1.3 (#4065): the moved-from twin of the disposed-`arr` row, but
// for an ORDINARY (class-typed) indexer collection rather than a move-only
// inline array — `arr` is whole-moved into `stolen` by a partial-move
// receiver (a method call that takes `owned arr`); afterwards a slot read of
// `arr` is a §28.3 read-of-moved-from, surfacing through the widened
// `IndexExpr` arm. Before the fix the IndexExpr arm's gate skipped every
// non-move-only-array receiver, so this case was silently accepted.
// Constructs `stolen` as a non-disposed Owned receiver (no Dispose) so the
// only diagnostic should be the moved-from one on `arr`, confirming the
// warning names the indexer's receiver — not the call site. // per §28.3
// K1.3, v0.294, #4065
BEZEL_TEST(check_read_index_expr_target_4065,
           slot_read_after_whole_move_owning_array_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var arr: OwningArray<Cell> := [new Cell(1)]; "
        "var stolen: OwningArray<Cell> := move arr; "
        "var x: int := arr[0]!.V; "
        "return x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "arr"));
}

// §28.2 / §28.3 K1.3 (#4065): the WRITE-side twin of the disposed-`arr`
// row — a slot WRITE `arr[0] := new Cell(2)` into a dominantly disposed
// ordinary indexer is a §28.2 use-after-dispose, surfaced by the WIDENED
// `handleAssignment`'s explicit `IndexExpr` gate (the write-side analogue of
// the read-side `IndexExpr` arm widened above). Before the fix this shape
// compiled clean — the write-side gate was the same
// `isMoveOnlyInlineArrayType`-only check that #1928 half b shipped — and the
// `:=` plain-assignment path does not run a general `checkRead(*as.target)`
// (only the compound-assignment path does, and only the explicit
// IndexExpr-target gate covers the move-only-array case for plain `:=`).
// Closes the write-side half of the same gap. // per §28.2, v0.294, #1928,
// #4065
BEZEL_TEST(check_read_index_expr_target_4065,
           slot_write_to_disposed_owning_array_rejected_spec_28_2) {
    auto r = parseAndCheckOwnership(wrap(
        "using var arr: OwningArray<Cell> := [new Cell(1)]; "
        "arr.Dispose(); "
        "arr[0] := new Cell(2); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(hasDisposeWarningFor(r, "arr"));
}

// §28.3 K1.3 (#4065): the WRITE-side moved-from twin — `arr[0] := …` after
// the whole move leaves the indexer receiver MovedFrom. Reaches the same
// widened `handleAssignment` gate as the dispose row above. // per §28.3
// K1.3, v0.294, #4065
BEZEL_TEST(check_read_index_expr_target_4065,
           slot_write_after_whole_move_owning_array_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var arr: OwningArray<Cell> := [new Cell(1)]; "
        "var stolen: OwningArray<Cell> := move arr; "
        "arr[0] := new Cell(2); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "arr"));
}

// =============================================================================
// ACCEPTS — no over-rejection.
// =============================================================================

// §28.3 K1.3 (#4065): a slot read WITHOUT any prior move on the ordinary
// indexer receiver is well-formed — `arr` is live, so `arr[0]` reads a live
// location. Accepts (guards against the widened gate rejecting the live case).
BEZEL_TEST(check_read_index_expr_target_4065,
           slot_read_of_live_owning_array_is_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var arr: OwningArray<Cell> := [new Cell(1)]; "
        "var x: int := arr[0]!.V; "
        "return x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 K1.3 (#4065): after `move a[i]` (a PARTIAL slot move) the array
// binding is live (Owned — the SLOT moved, not the array; #1925's own
// reasoning). A slot read of a different slot is well-formed. Accepts (guards
// the gate against firing on a partial-move receiver — the live-binding
// state passes silently).
BEZEL_TEST(check_read_index_expr_target_4065,
           slot_read_after_element_move_is_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "using var m: Cell? := move a[0]; "
        "var x: int := a[1]!.V; "
        "return x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 K1.3 (#4065): a copyable value-element inline array `[2]int` is not
// move-only — `var b := a` COPIES it (both stay live), so a slot read of `a`
// afterward is well-formed. Accepts (the widened gate is element-type-agnostic
// but still consults only the receiver binding's state, which is Owned here).
BEZEL_TEST(check_read_index_expr_target_4065,
           slot_read_of_copyable_value_array_is_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var a: [2]int := [10, 20]; "
        "var b: [2]int := a; "
        "var x: int := a[0]; "
        "return x + b[1];"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// RUNTIME — the valid slot read of a live `OwningArray<T>` disposes once.
// =============================================================================

// §28.3 / §28 (#4065): a valid slot read `arr[0]!.V` of a LIVE ordinary
// indexer (`OwningArray<Cell>`) reads the slot and lets the scope-end
// disposal dispose each heap cell exactly once. A double-free would print
// "D1" twice (and ASan/LSan would flip r.ran to false). Confirms the fix
// rejects the bad slot read of a disposed `arr` without breaking the valid
// slot read of a live `arr`.
BEZEL_TEST(check_read_index_expr_target_4065,
           live_owning_array_slot_read_disposes_once_spec_28_3) {
    const char* kRuntime =
        "module M; "
        "class Cell "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  destructor Console.WriteLine(\"D\" + V.ToString()); end destructor; "
        "end Cell; "
        "class C public static method Main(): int "
        "  using var arr: OwningArray<Cell> := [new Cell(1)]; "
        "  Console.WriteLine(\"v\" + arr[0]!.V.ToString()); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(kRuntime);
    if (wasSkipped(r)) return;  // ASan toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("v1") != std::string::npos);
}
