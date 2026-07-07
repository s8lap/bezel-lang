// Spec-derived tests for the §15.3 mandated warning on a provably-leaking
// move-only whole-value reassignment (issue #1549).
//
// §15.3 (Move-only value types), the "Whole-value reassignment" rule:
//   "arr := newValue; on a move-only local or field: the assignment is plain
//    — the compiler does not insert automatic cleanup of the previous slots.
//    If the LHS holds any slot that is not in moved-from state at the
//    assignment site, those slots leak. ... The compiler emits a warning
//    when reassignment is reachable from a state where some slot is
//    provably non-moved-from; the warning identifies the slots at risk and
//    points at the standard discharge mechanisms."
//
// Before this fix, the ownership pass tracked no per-slot moved-from state
// for a move-only inline-array LOCAL at all, so a whole-value reassignment
// of a `[2 owned]Cell?` local with both slots live compiled clean, ran, and
// leaked both cells with zero diagnostics — the §15.3 mandate was
// unimplemented (the leak itself is the separately-tracked #1385; this
// issue is only the missing warning).
//
// This is a WARNING, not a leak-fix: none of these programs are expected to
// have compile ERRORS (`r.diagnostics.hasErrors()` stays false throughout);
// #1385 tracks actually disposing the displaced slots.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// `class Cell` is a disposable-shaped class (mirrors the #780 sink tests);
// `body` is the Main method body.
std::string wrap(const std::string& body) {
    return std::string(
        "module M; class Cell public n: int := 0; end Cell; "
        "class C public static method Main(): int ") + body +
        " end Main; end C;";
}

// True iff some diagnostic is the §15.3 move-only-array reassignment-leak
// warning, naming `name` and citing exactly `expectedSlots` (e.g. "0, 1" or
// "1") as the bracketed at-risk-slot list.
bool hasReassignLeakWarning(const OwnershipResult& r, const std::string& name,
                            const std::string& expectedSlots) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("§15.3") == std::string::npos) continue;
        if (d.message.find("whole-value reassignment of move-only array `" +
                            name + "`") == std::string::npos)
            continue;
        if (d.message.find("[" + expectedSlots + "]") == std::string::npos)
            continue;
        return true;
    }
    return false;
}

// True iff ANY §15.3 reassignment-leak warning fired (any local, any slots).
bool hasAnyReassignLeakWarning(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("§15.3") == std::string::npos) continue;
        if (d.message.find("whole-value reassignment of move-only array") !=
            std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// =============================================================================
// WARNS — reassignment reachable while some slot is provably non-moved-from.
// =============================================================================

// §15.3: the exact repro from #1549 — a `[2 owned]Cell?` local with both
// slots literal-initialized (so both are live and non-moved-from) is
// whole-value-reassigned. WARNS, naming both at-risk slots `[0, 1]`; not an
// error (the program still compiles).
BEZEL_TEST(move_only_array_reassign_leak_1549,
           warns_when_both_slots_live_at_reassignment) {
    auto r = parseAndCheckOwnership(wrap(
        "do "
        "  var a: [2 owned]Cell? := [new Cell(), new Cell()]; "
        "  a := [new Cell(), new Cell()]; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasReassignLeakWarning(r, "a", "0, 1"));
}

// §15.3: only slot 1 is still live (slot 0 was drained via `move a[0]` +
// `null` restoration before the reassignment) — the warning identifies just
// the at-risk slot, `[1]`, not the already-proven-empty slot 0.
BEZEL_TEST(move_only_array_reassign_leak_1549,
           warns_naming_only_the_still_live_slot) {
    auto r = parseAndCheckOwnership(wrap(
        "do "
        "  var a: [2 owned]Cell? := [new Cell(), new Cell()]; "
        "  var c0 := move a[0]; "
        "  a[0] := null; "
        "  c0!.Dispose(); "
        "  a := [new Cell(), new Cell()]; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasReassignLeakWarning(r, "a", "1"));
}

// =============================================================================
// NO WARNING — every slot is provably moved-from before the reassignment.
// =============================================================================

// §15.3: both slots drained via `move a[i]` and restored to `null` (the
// standard discharge mechanism the warning message itself points at) before
// the whole-value reassignment. No slot is provably non-moved-from at the
// reassignment site, so no warning fires.
BEZEL_TEST(move_only_array_reassign_leak_1549,
           no_warning_when_every_slot_moved_and_restored_to_null_first) {
    auto r = parseAndCheckOwnership(wrap(
        "do "
        "  var a: [2 owned]Cell? := [new Cell(), new Cell()]; "
        "  var c0 := move a[0]; a[0] := null; c0!.Dispose(); "
        "  var c1 := move a[1]; a[1] := null; c1!.Dispose(); "
        "  a := [new Cell(), new Cell()]; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAnyReassignLeakWarning(r));
}

// §15.3: the whole array was already transferred out as a unit (`var b := a;`
// — a plain-binding move for a move-only value, §15.3 bullet 1) before `a`
// is reassigned. Nothing is left owned in `a` to leak, so no warning fires.
BEZEL_TEST(move_only_array_reassign_leak_1549,
           no_warning_when_whole_array_moved_out_first) {
    auto r = parseAndCheckOwnership(wrap(
        "do "
        "  var a: [2 owned]Cell? := [new Cell(), new Cell()]; "
        "  var b: [2 owned]Cell? := a; "
        "  a := [new Cell(), new Cell()]; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAnyReassignLeakWarning(r));
}
