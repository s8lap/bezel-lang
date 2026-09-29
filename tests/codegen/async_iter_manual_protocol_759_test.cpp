// fix #759 / §29.3 / §30.7 / stdlib §4.13: the MANUAL single-use
// async-iterator protocol — `await using var en := Gen(); await en.MoveNextAsync()
// / en.Current` — now lowers correctly. Previously only the `await for`
// desugaring open-coded the IAsyncEnumerator vtable protocol; a manual receiver
// IR-failed.
//
// Three cooperating gaps were closed: (1) sema typed `en.MoveNextAsync()` as
// Task<bool> (was untyped → the await produced an i32 → `not <i32>` emitted the
// malformed `br i32 %nv` / "Branch condition is not 'i1' type!"); (2) codegen
// dispatched MoveNextAsync through vtable slot 2 returning the Task<bool> ptr,
// and `en.Current` through slot 3 returning T (was a wrong value); (3) UNMASK —
// scope-end disposal of the `await using var` was rerouted from the generic
// asyncDispose path (which dispatches vtable slot 2 = MoveNextAsync on this
// frame and frees → heap corruption) to the iterator-format slot-1 Dispose
// (iterEnumDispose), exactly as the sync `for` / `await for` loops do (§29.7).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §29.3 / §30.7: the full manual protocol — two values read back through
// MoveNextAsync()/Current, then a final MoveNextAsync() returns false at
// exhaustion. Each assertion returns a distinct nonzero code; exit 0 = all pass.
BEZEL_TEST(async_iter_manual_protocol_759, reads_values_and_terminates) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerator<int> "
        "    yield return 42; "
        "    yield return 99; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    await using var en := Gen(); "
        "    var m1 := await en.MoveNextAsync(); "
        "    if not m1 then return 1; end; "        // was: IR_FAIL `br i32`
        "    if en.Current <> 42 then return 2; end; "  // was: wrong value (-1)
        "    var m2 := await en.MoveNextAsync(); "
        "    if not m2 then return 3; end; "
        "    if en.Current <> 99 then return 4; end; "
        "    var m3 := await en.MoveNextAsync(); "
        "    if m3 then return 5; end; "            // exhausted → false
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}

// §30.7: the minimal IR_FAIL repro from the issue — a single `not <MoveNextAsync>`
// in a branch condition. Before the fix this failed IR verification at compile
// time ("Branch condition is not 'i1' type!"); now it branches on a valid i1.
BEZEL_TEST(async_iter_manual_protocol_759, move_next_async_branches_as_bool) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerator<int> "
        "    yield return 7; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    await using var en := Gen(); "
        "    var m1 := await en.MoveNextAsync(); "
        "    if not m1 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}

// §29.7 (UNMASK): on early abandonment (the consumer leaves the `await using`
// scope before exhausting the iterator), scope-end disposal must run the
// iterator's pending `finally` EXACTLY ONCE via the slot-1 Dispose — not crash
// (slot-2 MoveNextAsync misdispatch) and not leak. `cleaned` reads back 1.
BEZEL_TEST(async_iter_manual_protocol_759, await_using_disposes_finally_once) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C "
        "  public static cleaned: int := 0; "
        "  public static async method Gen(): IAsyncEnumerator<int> "
        "    try "
        "      yield return 42; "
        "      yield return 99; "
        "    finally "
        "      C.cleaned := C.cleaned + 1; "
        "    end; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    do "
        "      await using var en := Gen(); "
        "      var m1 := await en.MoveNextAsync(); "
        "      if en.Current <> 42 then return 90; end; "
        "    end; "                                  // en disposed here
        "    return C.cleaned; "                     // 1
        "  end Main; "
        "end C;"), 1);
}
