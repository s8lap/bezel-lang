// fix #4336 (§15.1.1 / §15.3 / §28.7, v0.298): a SINGLE-INDEX assignment into a
// plain inline-array slot of an OWNING inline array `[N owned]T` /
// `[N owned]T?` (`outer[i] := new ...`) was leaking the OLD slot value on every
// iteration — the IndexExpr-arm plain-array-assignment fallback at
// `LlvmBackend_Stmts.cpp:~459` overwrote the slot with `CreateStore` and never
// emitted the per-slot dispose-and-null pass the spec (§28.7's "drop-on-overwrite"
// rule for move-only inline arrays) requires.
//
// The whole-array reassignment arm at the same file's `~1055` line already
// called `lowerOwningInlineArrayReassign` (`§28.7 / #1385 / v0.260`, extended by
// `#3263` for throwing slot disposers), closing the parallel leak for
// `outer := [...]`. This regression pins that the IndexExpr-arm catches up —
// the issue's repro shape (`var outer: [1 owned]IHolder := [new Widget("init")];
// outer[0] := new Widget("hello");`) loop-amplified 200× reported exactly one
// `Direct leak ... in 200 object(s) ... bz$M.C$Once` row (the displaced
// "init" Widget, never released on overwrite) on the pre-fix compiler.
//
// BORROW-SHAPE SAFETY: a non-owning `[N]IHolder` slot is, per §15.1, a
// borrow — the elements stored are *borrows* whose owner lives elsewhere, and
// disposing them here would double-free that owner's instance on a future read
// through its borrow source. This fix is gated on `rt->arrayOwned = true` so
// the borrow shape is preserved; a sibling non-owning shape (the issue's
// literal repro used `[1]IHolder`) is a separate spec/ruling question that the
// fix deliberately does NOT address (see "Filed-surviving".
//
// SCOPE ELEMENTS (measured per program):
//   * drop-on-overwrite: each iteration disposes the previous-slot Widget
//     before the new Widget is stored.
//   * ASAN/LSan-clean under `runAotChecked` — `r.ran` is TRUE on the post-fix
//     compiler (the leak row flips it to FALSE pre-fix).
//   * The disposal covers the dynamics-dispatch path: `Widget.Dispose()` runs
//     on the prior iteration's instance exactly once, with no double-free
//     (the local's scope-end `inlineOwnedArray` pass would dispose the final
//     "hello" Widget — measured clean).
//
// LOOP-AMPLIFICATION (LANE_RULES §3 / §30-AMENDED): the leak row rep is amplified
// to 200 iterations in the test driver; one-shot programs would pass under
// LSan on BOTH sides because the still-on-stack value is conservatively
// reached (measured: clean one-shot both sides; 200x shows the leak).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

}  // namespace

// =============================================================================
// #4336 — single-index assignment into an owning inline-array slot disposes
// the OLD slot value. Loop-amplified 200×; LSan-clean iff the drop-on-overwrite
// pass fires.
// =============================================================================
const char* kSingleIndexOverwriteLoop =
    "module M; "
    "interface IHolder "
    "  method GetV(): string; "
    "end IHolder; "
    "class Widget: IHolder "
    "  private _s: string; "
    "  public constructor(s: string) "
    "    _s := s; "
    "  end constructor; "
    "  public method GetV(): string => _s; "
    "end Widget; "
    "class C "
    "  public static method Once(): int "
    "    var outer: [1 owned]IHolder := [new Widget(\"init\")]; "
    "    outer[0] := new Widget(\"hello\"); "
    "    return outer[0].GetV().Length; "
    "  end Once; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 200 do "
    "      total := total + C.Once(); "
    "      i := i + 1; "
    "    end; "
    "    return total; "
    "  end Main; "
    "end C;";

// §15.1.1 / §28.7: the per-slot `outer[0] := new Widget("hello")` overwrites the
// "init" Widget. Pre-fix the OLD Widget was leaked on every iteration; post-fix
// the per-slot dispose-and-reclaim pass fires between the load-old and the
// store-new, freeing the disposed instance exactly once. The pre-existing
// scope-end disposal (registerInlineOwnedArrayDisposal at the local-decl site)
// covers the NEW Widget as well, so the program is fully LSan-clean.
BEZEL_TEST(owning_inline_array_index_assign_drop_on_overwrite_4336,
           single_index_overwrite_does_not_leak_spec_15_1_1_28_7) {
    JitResult r = runAotChecked(kSingleIndexOverwriteLoop);
    BEZEL_EXPECT_TRUE(r.ran);
    // Loop computes (5 chars * 200 iterations) = 1000 — the sink observable is
    // independent of the disposal semantics, but asserting it confirms the
    // assignment is actually happening and the IndexExpr arm did NOT silently
    // drop the write (which the pre-fix codegen could have produced on a
    // lowering failure — a #3025/#2961 family degrade).
    BEZEL_EXPECT_EQ(r.exitCode, 1000 % 256);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// #4336 — ACCEPT-SIDE CONTROL: a slot write on a NON-OVERWRITTEN index of a
// owning array does NOT dispose the slot (the slot was already-disposed-of
// remains live; an unmoved slot must not be dropped on the wrong index write).
// =============================================================================
//
// Simple existence smoke: a single-index assignment into a [2 owned]Cell? at
// index 1 must NOT dispose index 0's slot (index 0 was never written by the
// assigner); the program still disposes BOTH slots at scope-end exactly once,
// each, when the local's normal-exit disposal pass fires (no double-free, no
// dropped slot 0). LSan-clean iff the per-slot dispose is correctly SCOPED
// to the slot being overwritten — and NOT to any other slot.
const char* kSingleIndexNonOverwritten =
    "module M; "
    "class Cell "
    "  public n: int := 0; "
    "  public constructor(v: int) "
    "    n := v; "
    "  end constructor; "
    "  destructor end destructor; "
    "end Cell; "
    "class C "
    "  public static method Main(): int "
    "    var arr: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
    "    arr[1] := new Cell(3); "
    "    return arr[0]!.n + arr[1]!.n; "
    "  end Main; "
    "end C;";
BEZEL_TEST(owning_inline_array_index_assign_drop_on_overwrite_4336,
           single_index_non_overwritten_slot_does_not_double_dispose_spec_28_7) {
    JitResult r = runAotChecked(kSingleIndexNonOverwritten);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, (1 + 3) % 256);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// #4336 — BORROW-SHAPE ACCEPT-SIDE CONTROL (per §15.1): a single-index write
// into a NON-owning `[N]Holder` slot (a borrow per §15.1) does NOT dispose
// the slot value, AND the slot's value source lives elsewhere (the borrow
// is the LHS here, not a fresh `new X()` — the test deliberately avoids the
// latter shape because that is the spec-contradicting leak path #4351
// filed for separate ruling). The fix is gated on `arrRt->arrayOwned`;
// this control's job is "the fix does not introduce spurious disposal on
// the borrow side" — only an owning-flag-true type enters the dispose
// block.
// =============================================================================
//
// The program runs, exits 0 (7+5 mod 256 = 12), and is LSan-clean: `h`
// owns its Holder, scope-end disposes it; the slot never holds a fresh
// owned value, so no slot-disposal obligation arises.
const char* kBorrowShapeOverwrite =
    "module M; "
    "class Holder "
    "  public n: int := 0; "
    "  public constructor(v: int) "
    "    n := v; "
    "  end constructor; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    var h: Holder := new Holder(5); "
    "    var arr: [1]Holder := [h]; "  // borrow-of-h (Holder is class, no
                                     // `owned` marker; per §15.1 borrow
                                     // only)
    "    arr[0] := h; "                // borrow again (slot->borrow
                                     // overwrite; no fresh allocation)
    "    return arr[0].n + h.n; "
    "  end Main; "
    "end C;";
BEZEL_TEST(owning_inline_array_index_assign_drop_on_overwrite_4336,
           single_index_write_into_borrow_slot_does_not_over_dispose_spec_15_1) {
    JitResult r = runAotChecked(kBorrowShapeOverwrite);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, (5 + 5) % 256);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
