// Codegen / semantics + leak — issue #3699 (spec §15.1.1 "Owning inline
// arrays", §15.3 "Move-only value types", §28.10 "`using var`", §29.7
// "Disposal of partially-consumed iterators", §30.7 "Async iterators", v0.286):
// a `[N owned]T` / `[N owned]T?` move-only owning inline-array LOCAL declared in
// ANY state-machine body never disposed its slots.
//
// §15.1.1: "When an owning inline array's lifetime ends, the array's
// auto-dispose pass disposes each non-null slot. For locals, this happens at
// scope end via the standard owned-local discharge (§28.3)." In a sync
// iterator's MoveNext, an async method's Resume and an async iterator's Resume
// that pass never ran, so the ELEMENTS' DESTRUCTORS never ran — an observable
// semantic defect, not only a leak.
//
// `Cell` bumps a static counter in its constructor and decrements it in its
// destructor, so the `n=` this suite prints is a DIRECT oracle for "did the slot
// dispose": 0 iff every element was destroyed exactly once. A leak leaves it
// POSITIVE; a double free drives it NEGATIVE (and trips ASan first). Measured on
// r12-cycle-1, 200 activations of `var a: [2 owned]Cell? := [new Cell(1),
// new Cell(2)];`, full drain:
//
//     plain method (control)   total=600 n=0     clean
//     sync iterator            total=600 n=400   6400 B / 400 allocs
//     async method             total=600 n=400   6400 B / 400 allocs
//     async iterator           total=600 n=400   6400 B / 400 allocs
//
// `n=400` is 200 rounds x 2 slots whose destructors never ran.
//
// ── WHAT WAS WRONG. Not a pass that mis-answered: the pass never ran.
// `registerInlineOwnedArrayDisposal` (LlvmBackend_Setup.cpp) opens with
// `if (currentFn_ != moveOnlyArrayTransferredFn_) return true;`, and that token
// was assigned in exactly two places, both in LlvmBackend_Setup.cpp and both
// keyed to the ENTRY fn. A state-machine body lowers with `currentFn_ ==
// moveNextFn` / `resumeFn`, so the gate was false for the whole body.
//
// ── DIRECTION OF DANGER IS A DOUBLE-FREE, because the fix ARMS a disposal.
// `moveOnlyArrayTransferred_` exists precisely to SUPPRESS that disposal for a
// local whose slots were moved out, so arming it in a state machine had to
// re-run the transfer analysis over the SAME body rather than merely re-point
// the token. Every row is `runAotChecked` (generated-code ASan with
// LeakSanitizer on), so a double free or a use-after-free fails the row before
// `n` is compared, and `n` is itself a corruption oracle. Rows 5-9 are the two
// edges where a state machine's dispose stack can run a SECOND time — §29.7
// ABANDONMENT (`break`, where the yield handler's `smIterCleanupAtState_`
// snapshot replays the stack from Dispose) and a body that THROWS after a yield.
// A plain `inlineOwnedArray` Disposal is NOT on the §27.5 exclusion list in
// LlvmBackend_Stmts.cpp's yield handler (the neighbouring `inlineOwnedArrayCell`
// is), so this commit makes that entry LIVE in the abandonment snapshot for the
// first time — which is exactly why those rows exist. Rows 10-13 are the
// transferred-out shapes that must NOT be disposed here at all.
// per §15.1.1, §15.3, §29.7, §30.7 (issue #3699)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// Cell: a disposable class with a static live-instance counter. ctor +1,
// destructor -1. Mirrors tests/codegen/inline_owned_array_disposal_747_test.cpp.
const char* kHdr =
    "module M; import System; import System.Collections.Generic; "
    "import System.Threading.Tasks; "
    "class Cell "
    "  public static n: int := 0; "
    "  public V: int := 0; "
    "  public constructor(v: int) Cell.n := Cell.n + 1; V := v; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; ";

const char* kTail =
    "    Console.WriteLine(\"total=\" + total.ToString() + "
    "                      \" n=\" + Cell.n.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";
}  // namespace

// ── Row 1, the CONTROL: a plain (non-state-machine) method. Its entry fn IS
// the fn `emitFunctionBody` keyed the token to, so the disposal has always
// armed here. Clean on BOTH sides — it localises rows 2-4 to the state machines.
// 200 * 3 = 600. // per §15.1.1 (issue #3699)
BEZEL_TEST(state_machine_inline_owned_array_disposal_3699,
           plain_method_control_disposes_both_slots_spec_15_1_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Gen(): int "
        "    var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "    return 3; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Gen(); end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 2, the SYNC ITERATOR. Pre-fix `total=600 n=400`, 6400 B / 400 allocs:
// 400 Cells constructed and never destroyed. The printed total is identical on
// both sides — `n` and LSan are the only oracles that see this.
// 200 * (1 + 2) = 600. // per §15.1.1, §29.7 (issue #3699)
BEZEL_TEST(state_machine_inline_owned_array_disposal_3699,
           sync_iterator_body_disposes_both_slots_spec_15_1_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "    yield return 1; "
        "    yield return 2; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      for var v in C.Gen() do total := total + v; end; "
        "    end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 3, the ASYNC ITERATOR. Same shape over `IAsyncEnumerable<int>`.
// Pre-fix `total=600 n=400`, 6400 B / 400 allocs.
// 200 * (1 + 2) = 600. // per §15.1.1, §30.7 (issue #3699)
BEZEL_TEST(state_machine_inline_owned_array_disposal_3699,
           async_iterator_body_disposes_both_slots_spec_30_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "    yield return 1; "
        "    yield return 2; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      await for var v in C.Gen() do total := total + v; end; "
        "    end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 4, the ASYNC METHOD — the venue issue 3699's own measurement table did
// not list, found by running the same probe against `emitAsyncMethod`'s resume.
// Pre-fix `total=600 n=400`, 6400 B / 400 allocs, identical to the two iterators.
// 200 * 3 = 600. // per §15.1.1, §30.2 (issue #3699)
BEZEL_TEST(state_machine_inline_owned_array_disposal_3699,
           async_method_body_disposes_both_slots_spec_30_2) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Step(x: int): Task<int> "
        "    return x; "
        "  end Step; "
        "  public static async method Gen(): Task<int> "
        "    var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "    var r := await C.Step(3); "
        "    return r; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + await C.Gen(); end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 5, the NON-NULLABLE `[N owned]T` spelling (§15.1.1 admits both). Same
// venue as row 2; it is a separate row because the two spellings reach
// `registerInlineOwnedArrayDisposal` through different `rt->arrayOwned` shapes.
// 200 * (1 + 2) = 600. // per §15.1.1 (issue #3699)
BEZEL_TEST(state_machine_inline_owned_array_disposal_3699,
           sync_iterator_non_nullable_form_disposes_both_slots_spec_15_1_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    var a: [2 owned]Cell := [new Cell(1), new Cell(2)]; "
        "    yield return 1; "
        "    yield return 2; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      for var v in C.Gen() do total := total + v; end; "
        "    end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 6, OVER-RELEASE probe 1 — §29.7 ABANDONMENT, sync iterator. The
// consumer `break`s after the first element, so MoveNext's own scope-end
// disposal never runs and the replay from Dispose is the ONLY thing that can
// dispose the slots. This commit puts a plain `inlineOwnedArray` entry into that
// `smIterCleanupAtState_` snapshot for the first time — the #3500 comment in
// LlvmBackend_Stmts.cpp says the kind "is gated on `moveOnlyArrayTransferredFn_`,
// a token this commit does not touch, so it is as inert in an iterator body
// after this change as it was before", and this commit is the one that ends
// that. It must run EXACTLY once over 200 abandonments: twice is an ASan double
// free, never is `n=400`. 200 * 1 = 200. // per §29.7 (issue #3699)
BEZEL_TEST(state_machine_inline_owned_array_disposal_3699,
           abandoned_sync_iterator_disposes_the_slots_exactly_once_spec_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "    yield return 1; "
        "    yield return 2; "
        "    yield return 3; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      for var v in C.Gen() do total := total + v; break; end; "
        "    end; " +
        kTail),
        "total=200 n=0\n");
}

// ── Row 7, the async twin of row 6: `break` out of an `await for`.
// 200 * 1 = 200. // per §29.7, §30.7 (issue #3699)
BEZEL_TEST(state_machine_inline_owned_array_disposal_3699,
           abandoned_async_iterator_disposes_the_slots_exactly_once_spec_30_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "    yield return 1; "
        "    yield return 2; "
        "    yield return 3; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      await for var v in C.Gen() do total := total + v; break; end; "
        "    end; " +
        kTail),
        "total=200 n=0\n");
}

// ── Row 8, OVER-RELEASE probe 2 — THROW AFTER A YIELD, sync iterator. The
// exception edge runs the dispose stack AND the consumer's unwind reaches
// Dispose; the two stay exclusive because `propagateBB` parks the machine at -1
// on that edge (#3531) and Dispose's switch has no case for it. Same
// exactly-once obligation as row 6, on the edge the #3500 comment records as
// historically the dangerous one. 200 * (1 + 1) = 400.
// per §29.7, §28.3 (issue #3699)
BEZEL_TEST(state_machine_inline_owned_array_disposal_3699,
           throwing_sync_iterator_disposes_the_slots_exactly_once_spec_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "    yield return 1; "
        "    throw new InvalidOperationException(\"pqr\"); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      try "
        "        for var v in C.Gen() do total := total + v; end; "
        "      catch "
        "        var e: Exception => do total := total + 1; end "
        "      end try; "
        "    end; " +
        kTail),
        "total=400 n=0\n");
}

// ── Row 9, the async twin of row 8. 200 * (1 + 1) = 400.
// per §29.7, §30.7 (issue #3699)
BEZEL_TEST(state_machine_inline_owned_array_disposal_3699,
           throwing_async_iterator_disposes_the_slots_exactly_once_spec_30_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "    yield return 1; "
        "    throw new InvalidOperationException(\"pqr\"); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      try "
        "        await for var v in C.Gen() do total := total + v; end; "
        "      catch "
        "        var e: Exception => do total := total + 1; end "
        "      end try; "
        "    end; " +
        kTail),
        "total=400 n=0\n");
}

// ── Row 10, the async-METHOD throw edge: the resume leaves by an exception
// after a real suspend, so the array must be disposed once by the unwind path.
// 200 * 3 = 600. // per §28.3, §30.2 (issue #3699)
BEZEL_TEST(state_machine_inline_owned_array_disposal_3699,
           throwing_async_method_disposes_the_slots_exactly_once_spec_30_2) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Step(x: int): Task<int> "
        "    return x; "
        "  end Step; "
        "  public static async method Gen(): Task<int> "
        "    var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "    var r := await C.Step(3); "
        "    throw new InvalidOperationException(\"pqr\"); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      try "
        "        total := total + await C.Gen(); "
        "      catch "
        "        var e: Exception => do total := total + 3; end "
        "      end try; "
        "    end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 11, TRANSFERRED OUT, sync iterator. `var b := a` is a whole-array
// move-only transfer (§15.3): b's slots alias a's pointers, `a` is marked in
// `moveOnlyArrayTransferred_` and its disposal must be SKIPPED while b's runs.
// This is the row that proves the fix re-RUNS the transfer analysis over the
// state-machine body instead of merely re-pointing the token — with a stale set
// this is a double free, not a leak. 200 * (1 + 2) = 600.
// per §15.3 (issue #3699)
BEZEL_TEST(state_machine_inline_owned_array_disposal_3699,
           transferred_out_sync_iterator_local_is_not_double_freed_spec_15_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "    var b: [2 owned]Cell? := a; "
        "    yield return 1; "
        "    yield return 2; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      for var v in C.Gen() do total := total + v; end; "
        "    end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 12, TRANSFERRED OUT ACROSS A YIELD, async iterator: the transfer
// happens in a LATER activation than the declaration, so the two locals are
// restored from the frame on different resumes. 200 * (1 + 2) = 600.
// per §15.3, §30.7 (issue #3699)
BEZEL_TEST(state_machine_inline_owned_array_disposal_3699,
           transferred_out_across_a_yield_is_not_double_freed_spec_15_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "    yield return 1; "
        "    var b: [2 owned]Cell? := a; "
        "    yield return 2; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      await for var v in C.Gen() do total := total + v; end; "
        "    end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 13, TRANSFERRED OUT in an async METHOD, across a real suspend.
// 200 * 3 = 600. // per §15.3, §30.2 (issue #3699)
BEZEL_TEST(state_machine_inline_owned_array_disposal_3699,
           transferred_out_async_method_local_is_not_double_freed_spec_15_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Step(x: int): Task<int> "
        "    return x; "
        "  end Step; "
        "  public static async method Gen(): Task<int> "
        "    var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "    var r := await C.Step(3); "
        "    var b: [2 owned]Cell? := a; "
        "    return r; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + await C.Gen(); end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 14, PER-SLOT transfer: `move a[0]` nulls the slot, so the scope-end
// pass must skip slot 0 and dispose slot 1, while `t` disposes the moved value.
// The array itself stays OUT of `moveOnlyArrayTransferred_` (an index receiver
// is a borrow position), so this row exercises the armed disposal and the
// per-slot null guard together, inside a state machine.
// 200 * (1 + 2) = 600. // per §15.3, §15.1.1 (issue #3699)
BEZEL_TEST(state_machine_inline_owned_array_disposal_3699,
           slot_move_then_null_in_an_iterator_is_not_double_freed_spec_15_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "    using var t: Cell? := move a[0]; "
        "    a[0] := null; "
        "    yield return 1; "
        "    yield return 2; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      for var v in C.Gen() do total := total + v; end; "
        "    end; " +
        kTail),
        "total=600 n=0\n");
}

// ── Row 15, the SAVE/RESTORE half (`MoveOnlyArrayTransferScope`). A GENERIC
// sync iterator is specialised from inside `Outer`, and `Outer` then declares a
// `[N owned]T` of its OWN. Neither `buildSpecializedMethodFn` nor
// `emitInlineGenericMethod` saves `moveOnlyArrayTransferred_`/`…Fn_`, so without
// the scope this commit adds beside `BorrowingReclaimSetScope`, arming the
// disposal in the iterator would COST `Outer` its own disposal — trading one
// leak for another. 200 * ((1 + 2) + 3) = 1200.
// per §15.3, §15.1.1, §28.3 (issue #3699)
BEZEL_TEST(state_machine_inline_owned_array_disposal_3699,
           generic_iterator_spec_restores_the_outer_bodys_transfer_set_spec_15_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Gen<T>(x: T): IEnumerable<int> "
        "    var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "    yield return 1; "
        "    yield return 2; "
        "  end Gen; "
        "  public static method Outer(): int "
        "    var t := 0; "
        "    for var v in C.Gen<int>(1) do t := t + v; end; "
        "    var ys: [2 owned]Cell? := [new Cell(3), new Cell(4)]; "
        "    return t + ys[0]!.V; "
        "  end Outer; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Outer(); end; " +
        kTail),
        "total=1200 n=0\n");
}
