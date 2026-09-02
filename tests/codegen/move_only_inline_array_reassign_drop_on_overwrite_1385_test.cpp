// fix #1385 / §28.7 v0.215: drop-on-overwrite for WHOLE-VALUE reassignment of
// a move-only OWNING inline-array local or field (`[N owned]T` / `[N owned]T?`).
//
// Spec §28.7 v0.215 (extending the v0.161 drop-on-overwrite rule to owning
// inline arrays, per #1385):
//   "reassigning a [N owned]T location disposes each non-moved-from slot of
//    the displaced value (reverse slot order), then stores"
// Before this fix, whole-value reassignment of a move-only inline array was a
// PLAIN store — the displaced slots' class instances LEAKED with no diagnostic
// at all (the §15.3 warning was the user-visible signal of the leak; with
// drop-on-overwrite there is no leak, and no warning should fire). The
// specifically-cited moved-from path is `move a[i]`, which already nulls the
// slot in the alloca (#747 lowerMoveFromCollectionSlot), so the new helper's
// null-guard naturally skips moved-from slots — exactly once across move +
// reassign + scope-end.
//
// This file tests the WHOLE-VALUE reassignment case (the complement of #1328's
// class-field/local drop-on-overwrite and #1549's warning):
//   - `arr := [new c0, new c1]` from a `[2 owned]Cell?` literal: the displaced
//     slots (both Cells in arr1) are disposed; the new slots are populated.
//   - Same with `[N owned]Cell` (non-nullable): no null-guard skips, all slots
//     disposed.
//   - Whole-array reassignment `arr := other` from another owning array.
//   - Reassignment after `move arr[i]` — the moved-from slot is null in the
//     alloca, the null-guard skips it, the live slot is disposed.
//   - Overwrite chain (3 successive reassignments): every previous slot's Cells
//     are disposed.
//   - Constructor-time first-time assignment to a move-only-array FIELD via the
//     field-initializer path is unaffected (no overwrite yet).
//   - ASan sweep on a representative case: no leak, no double-free.
//
// Spec citations: §28.7 v0.215 (#1385 extension), §15.3 (move-only value
// types), §15.1.1 (owning inline arrays), §28.4 (auto-dispose at scope end).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "native_test_harness.hpp"
#include "support/run_under_asan.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;

namespace {
namespace fs = std::filesystem;

// Cell: destructor-counter idiom (the dispose-exactly-once oracle).
// `Cell.n` is incremented on construct, decremented on dispose. A correct
// program exits with `Cell.n == 0`; a leak leaves `n > 0`; a double-free
// drives `n` negative or aborts.
const std::string kCellHdr =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; ";

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// Wrap `body` into a complete program: Cell + a class C with a Main method.
// Avoids nested-paren macro confusion by assembling the program as a single
// concatenation expression passed to BEZEL_EXPECT_EQ.
std::string progWithBody(const std::string& body) {
    return std::string(kCellHdr) +
        "class C public static method Main(): int " + body +
        " end Main; end C;";
}

// AOT compile → object → link with ASan → run. Returns 0 (clean), 1
// (sanitizer fired), -1 (toolchain missing).
int runUnderASan(const std::string& source) {
    // fix #3653: forwards to the ONE shared implementation.
    return bezel::test::runUnderAsan(source, "bezel_1385_asan");
}

}  // namespace

// ===========================================================================
// Value tests — destructor-counter idiom
// ===========================================================================

// §28.7 / #1385: whole-value reassignment of a `[2 owned]Cell?` local from a
// fresh `[…]` literal disposes both displaced slots before overwriting.
BEZEL_TEST(move_only_inline_array_reassign_drop_on_overwrite_1385,
           local_literal_reassign_disposes_both_slots_spec_28_7) {
    std::string src = progWithBody(
        std::string("do ") +
        "  var a: [2 owned]Cell? := [new Cell(), new Cell()]; " +
        "  if Cell.n <> 2 then return 101; end; " +
        "  a := [new Cell(), new Cell()]; " +
        "  if Cell.n <> 2 then return 102; end; " +
        "end; " +
        "return Cell.n; ");
    BEZEL_EXPECT_EQ(run(src), 0);
}

// §28.7 / #1385: the non-nullable `[N owned]Cell` form — every slot is always
// live (no nullable), so reassignment disposes every displaced slot.
BEZEL_TEST(move_only_inline_array_reassign_drop_on_overwrite_1385,
           local_non_nullable_reassign_disposes_all_slots_spec_28_7) {
    std::string src = progWithBody(
        std::string("do ") +
        "  var a: [2 owned]Cell := [new Cell(), new Cell()]; " +
        "  if Cell.n <> 2 then return 101; end; " +
        "  a := [new Cell(), new Cell()]; " +
        "  if Cell.n <> 2 then return 102; end; " +
        "end; " +
        "return Cell.n; ");
    BEZEL_EXPECT_EQ(run(src), 0);
}

// §28.7 / #1385: whole-value reassignment from ANOTHER move-only array
// (`a := b`). `a`'s slots are disposed, then `b`'s slots are copied into `a`.
// `b` is then moved-from (semantically) but the alloca's slots still hold the
// values — the §15.3 whole-move track suppresses `b`'s scope-end disposal, so
// neither `a`'s second storage nor `b`'s scope end double-frees.
BEZEL_TEST(move_only_inline_array_reassign_drop_on_overwrite_1385,
           local_reassign_from_other_array_no_double_free_spec_28_7) {
    std::string src = progWithBody(
        std::string("do ") +
        "  var b: [2 owned]Cell? := [new Cell(), new Cell()]; " +
        "  var a: [2 owned]Cell? := [new Cell(), new Cell()]; " +
        "  if Cell.n <> 4 then return 101; end; " +
        "  a := b; " +
        "  if Cell.n <> 2 then return 102; end; " +
        "end; " +
        "return Cell.n; ");
    BEZEL_EXPECT_EQ(run(src), 0);
}

// §28.7 / #1385: overwrite chain — three successive reassignments. Only the
// most-recent array's slots are live at scope end.
BEZEL_TEST(move_only_inline_array_reassign_drop_on_overwrite_1385,
           local_overwrite_chain_all_disposed_spec_28_7) {
    std::string src = progWithBody(
        std::string("do ") +
        "  var a: [2 owned]Cell? := [new Cell(), new Cell()]; " +
        "  if Cell.n <> 2 then return 101; end; " +
        "  a := [new Cell(), new Cell()]; " +
        "  if Cell.n <> 2 then return 102; end; " +
        "  a := [new Cell(), new Cell()]; " +
        "  if Cell.n <> 2 then return 103; end; " +
        "end; " +
        "return Cell.n; ");
    BEZEL_EXPECT_EQ(run(src), 0);
}

// §28.7 / #1385: after `move a[0]` the slot is null in the alloca (per #747);
// reassigning `a` disposes only the still-live slot [1] (the moved-from slot
// is null → null-guard skips). The moved-out value is still owned by its new
// owner (its `using var`) and is disposed once at its scope end.
//
// After the reassign: slot 0 (moved-from) was null → skipped. Slot 1
// (live) was disposed (Cell.n -= 1). Then 2 new Cells are stored
// (Cell.n += 2). Net: Cell.n = 2 - 1 + 2 = 3. Then inner-scope end
// disposes `c` (Cell.n = 2) and outer-scope end disposes `a`'s two new
// slots (Cell.n = 0). Final Cell.n = 0.
BEZEL_TEST(move_only_inline_array_reassign_drop_on_overwrite_1385,
           local_reassign_after_move_skips_moved_from_slot_spec_28_7) {
    std::string src = progWithBody(
        std::string("do ") +
        "  var a: [2 owned]Cell? := [new Cell(), new Cell()]; " +
        "  if Cell.n <> 2 then return 101; end; " +
        "  do " +
        "    using var c := move a[0]; " +
        "    if Cell.n <> 2 then return 102; end; " +
        "    a := [new Cell(), new Cell()]; " +
        // 1 old (slot 1) disposed + 2 new stored: Cell.n = 3
        "    if Cell.n <> 3 then return 103; end; " +
        "  end; " +
        // c disposed (Cell.n = 2), then outer scope-end disposes a's 2 new
        // slots (Cell.n = 0)
        "end; " +
        "return Cell.n; ");
    BEZEL_EXPECT_EQ(run(src), 0);
}

// §28.7 / #1385: an owning-inline-array FIELD. Reassigning
// `holder._slots := [...]` disposes the field's previous slots.
BEZEL_TEST(move_only_inline_array_reassign_drop_on_overwrite_1385,
           field_reassign_displaces_prev_slots_spec_28_7) {
    std::string src = std::string(kCellHdr) +
        "class Holder "
        "  public slots: [2 owned]Cell? := [new Cell(), new Cell()]; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  do using var h := new Holder(); "
        "    if Cell.n <> 2 then return 101; end; "
        "    h.slots := [new Cell(), new Cell()]; "
        "    if Cell.n <> 2 then return 102; end; "
        "  end; "
        "  return Cell.n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// ===========================================================================
// ASan AOT tests — heap correctness (no leak, no double-free)
// ===========================================================================

// §28.7 / #1385: a representative overwrite chain under ASan — no leak and
// no double-free. ASan catches both categories via heap quarantine.
BEZEL_TEST(move_only_inline_array_reassign_drop_on_overwrite_1385,
           asan_overwrite_chain_no_leak_no_double_free_spec_28_7) {
    std::string src = kCellHdr +
        "class C public static method Main(): int "
        "  do "
        "    var a: [2 owned]Cell? := [new Cell(), new Cell()]; "
        "    a := [new Cell(), new Cell()]; "
        "    a := [new Cell(), new Cell()]; "
        "  end; "
        "  return 0; "
        "end Main; end C;";
    int rc = runUnderASan(src);
    if (rc == -1) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_EQ(rc, 0);
}

// §28.7 / #1385: reassignment-after-`move` under ASan — the moved-from slot
// must not be disposed by the reassignment helper (null-guard), and the
// remaining live slot must be disposed exactly once.
BEZEL_TEST(move_only_inline_array_reassign_drop_on_overwrite_1385,
           asan_reassign_after_move_no_double_free_spec_28_7) {
    std::string src = kCellHdr +
        "class C public static method Main(): int "
        "  do "
        "    var a: [2 owned]Cell? := [new Cell(), new Cell()]; "
        "    do "
        "      using var c := move a[0]; "
        "      a := [new Cell(), new Cell()]; "
        "    end; "
        "  end; "
        "  return 0; "
        "end Main; end C;";
    int rc = runUnderASan(src);
    if (rc == -1) return;            // toolchain unavailable: SKIP (#3626)
    BEZEL_EXPECT_TRUE(rc >= 0);      // -2 program-compile / -3 link / -4 system() → FAIL (#3633)
    BEZEL_EXPECT_EQ(rc, 0);
}