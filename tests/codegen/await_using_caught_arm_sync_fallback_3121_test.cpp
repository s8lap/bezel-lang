// Tests for #3121 — the §28.12 exception-edge gap the #2892 ordering ruling
// exposed — and its closing by issue #7705.
//
// WHAT THE DOCUMENT REQUIRES.  Spec §28.12 *Asynchronous disposal* (v0.350):
//
//     Asynchronous cleanup runs only where the program wrote `await using`
//     (whose implicit `finally` awaits `DisposeAsync` on both the normal and
//     exception edges of an async body, with §29.7's synchronous fallback on
//     early iterator abandonment) or awaited a direct `DisposeAsync()` call.
//
// HISTORY.  Every exception-edge disposal used to go through
// `emitDisposals(..., isUnwind=true)` with no DisposeAsync, i.e. the
// synchronous `Dispose()` (the destructor + physical reclaim) — a recorded
// deferral ("the async await-across-active-unwind path is a deferred
// increment"), which #2892 widened by retiring the one edge (the catch arm of
// a try with a finally and a matching arm, #2869's exit mode 6) that happened
// to await.  These tests pinned that fallback's counters (1101, 101, 1101,
// 1102, 1) and said that "a future async-unwind-driver landing ... will need to
// update the expected counters (1101 -> 1110 etc.)".
//
// ISSUE #7705 IS THAT LANDING.  An unwind pass emitted in the async body's own
// resume function now suspends: the in-flight exception is parked across the
// await, `DisposeAsync` is awaited (step 1: the destructor body is skipped),
// and the exception is restored (or superseded by a fault of DisposeAsync
// itself).  Every row below therefore flipped to the §28.12 reading:
//
//   canonical   1101 -> 1110   (DisposeAsync +10, catch +100, finally +1000)
//   rethrow      101 ->  110   (observed at the arm's print: +10, +100)
//   arm return  1101 -> 1110
//   two         1102 -> 1120   (two DisposeAsync, no destructors)
//   simple         1 ->   10   (negative control: no try/finally around it)
//
// The shape of each program is unchanged:
//
//   try
//     await using var r := new AsyncRes();
//     var k := T.Boom();                // throws InvalidOperationException
//   catch var e: InvalidOperationException =>
//     AsyncRes.disposed := AsyncRes.disposed + 100;
//   finally
//     AsyncRes.disposed := AsyncRes.disposed + 1000;
//   end;
//   Console.WriteLine(AsyncRes.disposed);
//
// (`AsyncRes.DisposeAsync` adds 10, its destructor adds 1.)
//
// THE ORACLE IS A LIVE-OBJECT COUNTER, NOT "NO LEAK".  The subclasses used
// here count both construction (+1 each) and destructor runs (+1 each) —
// `_dtor` MUST equal `_made` after the program returns, and `_live` MUST be
// 0 — so a double dispose faults or shows a wrong `_dtor`, and an
// under-release leaks.  Generated-code ASan+LSan is wired through
// `runAotChecked`; the value channel travels on stdout because the expected
// counter (1101 etc.) exceeds the 8-bit exit-code ceiling — a process exit
// status is masked to WEXITSTATUS, so the counter must be printed rather
// than returned (tests/README.md).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Compile `source` to a temp native executable under ASan/LSan and run it;
// returns the captured stdout with one trailing newline stripped.  Returns
// `<did-not-run>` if the compile failed, the program crashed under ASan,
// or the subprocess did not terminate cleanly — the standard value-channel
// sentinel for an AOT run that the oracle cannot interpret.
std::string runStdoutChecked(const std::string& source) {
    auto r = runAotChecked(source);
    if (!r.ran) return std::string("<did-not-run>");
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();
    }
    return out;
}

// §28.12 oracle: `DisposeAsync` adds 10 (the ASYNC path), the destructor adds
// 1 (the SYNC fallback path, §28.12: "class should have a destructor for the
// synchronous fallback").  Both arms are stack-mounted so the program can
// observe the count even from the catch / finally.
const char* kPrelude =
    "module M; "
    "class AsyncRes: IAsyncDisposable "
    "  public static disposed: int := 0; "
    "  public async method DisposeAsync(): Task "
    "    AsyncRes.disposed := AsyncRes.disposed + 10; "
    "  end DisposeAsync; "
    "  destructor AsyncRes.disposed := AsyncRes.disposed + 1; end destructor; "
    "end AsyncRes; "
    "class T "
    "  public static method Boom(): int "
    "    throw new InvalidOperationException(\"no\"); "
    "  end Boom; "
    "end T; ";

// The §28.12 catch-arm of try-with-finally+matching-arm edge, in the issue's
// canonical shape.  Printed value: 1110 (DisposeAsync=10, catch=100,
// finally=1000, no destructor).  Was 1101 (the sync fallback) before #7705.
std::string programCanonical() {
    return std::string(kPrelude) +
        "class C "
        "  public static async method Main(): Task<int> "
        "    try "
        "      await using var r := new AsyncRes(); "
        "      var k := T.Boom(); "
        "    catch var e: InvalidOperationException => "
        "      AsyncRes.disposed := AsyncRes.disposed + 100; "
        "    finally "
        "      AsyncRes.disposed := AsyncRes.disposed + 1000; "
        "    end; "
        "    Console.WriteLine(AsyncRes.disposed); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// Variant: the catch arm RETHROWS the exception.  Same edge.  The observable
// MUST be printed BEFORE the rethrow: the exception unwinds out of Main past
// the finally, so a `Console.WriteLine(AsyncRes.disposed)` after the
// try/catch/finally block would never run.  The value 110 (DisposeAsync +10,
// catch +100) is what we observe at the point of the print; the finally's
// +1000 has not yet been applied.  Was 101 (destructor, not DisposeAsync).
std::string programRethrow() {
    return std::string(kPrelude) +
        "class C "
        "  public static async method Main(): Task<int> "
        "    try "
        "      await using var r := new AsyncRes(); "
        "      var k := T.Boom(); "
        "    catch var e: InvalidOperationException => do "
        "      AsyncRes.disposed := AsyncRes.disposed + 100; "
        "      Console.WriteLine(AsyncRes.disposed); "
        "      throw; "
        "    end "
        "    finally "
        "      AsyncRes.disposed := AsyncRes.disposed + 1000; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// Variant: the catch arm RETURNS from an async method.  Exit mode 5 in the
// async state machine — same body scope, same edge.
std::string programArmReturn() {
    return std::string(kPrelude) +
        "class C "
        "  public static async method Once(): Task<int> "
        "    try "
        "      await using var r := new AsyncRes(); "
        "      var k := T.Boom(); "
        "      return 1; "
        "    catch var e: InvalidOperationException => do "
        "      AsyncRes.disposed := AsyncRes.disposed + 100; "
        "      return 0; "
        "    end "
        "    finally "
        "      AsyncRes.disposed := AsyncRes.disposed + 1000; "
        "    end; "
        "  end Once; "
        "  public static async method Main(): Task<int> "
        "    var v := await C.Once(); "
        "    Console.WriteLine(AsyncRes.disposed); "
        "    return v; "
        "  end Main; "
        "end C;";
}

// Variant: TWO `await using var` declared in the same try body — both on the
// §28.12 catch-arm edge, both awaited.  Counter is 1120 (two DisposeAsync,
// no destructors, catch 100, finally 1000).  Was 1102 (two destructors).  A
// driver that awaited only the FIRST resource on this edge would show 1111.
std::string programTwoAwaitUsing() {
    return std::string(kPrelude) +
        "class C "
        "  public static async method Main(): Task<int> "
        "    try "
        "      await using var r1 := new AsyncRes(); "
        "      await using var r2 := new AsyncRes(); "
        "      var k := T.Boom(); "
        "    catch var e: InvalidOperationException => "
        "      AsyncRes.disposed := AsyncRes.disposed + 100; "
        "    finally "
        "      AsyncRes.disposed := AsyncRes.disposed + 1000; "
        "    end; "
        "    Console.WriteLine(AsyncRes.disposed); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// Negative-control: the SIMPLE throw-in-do-await-using shape (no try/finally
// around it).  Pinning 10 here proves the oracle distinguishes the shapes and
// the test is not just "everything is 1110-shaped".  This mirrors
// `async_28_12.await_using_fault_path_awaits_dispose_async_spec_28_12_7705`
// (async_test.cpp), so the two values agree on the same expectation.
std::string programSimpleShape() {
    return std::string(kPrelude) +
        "class C "
        "  public static async method Main(): Task<int> "
        "    var result := 0; "
        "    try "
        "      do "
        "        await using var r := new AsyncRes(); "
        "        throw new InvalidOperationException(); "
        "      end; "
        "    catch Exception => "
        "      result := AsyncRes.disposed; "
        "    end; "
        "    Console.WriteLine(result); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

}  // namespace

// ===========================================================================
// §28.12 catch-arm of try-with-finally+matching-arm: the #3121 shape, #7705.
// ===========================================================================

// §28.12 (v0.350): the catch arm FALLS THROUGH.  DisposeAsync is awaited at
// the throw site's unwind, before the arm and the finally (§28.10): 1110.
BEZEL_TEST(await_using_caught_arm_3121,
           canonical_awaits_dispose_async_spec_28_12_7705) {
    BEZEL_EXPECT_EQ(runStdoutChecked(programCanonical()), std::string("1110"));
}

// §28.12 (v0.350): the catch arm RETHROWS the exception.  Same caught-arm
// edge, exercised independently so a partial fix that only handled the swallow
// case fails here.  The value at the arm's print site: 10 + 100 = 110.
BEZEL_TEST(await_using_caught_arm_3121,
           rethrow_awaits_dispose_async_spec_28_12_7705) {
    // Exception escapes Main past the finally → WEXITSTATUS(non-zero).  The
    // observable therefore travels on stdout BEFORE the rethrow unwinds, and
    // ASan/LSan still gate the run via runAotChecked.
    auto r = runAotChecked(programRethrow());
    BEZEL_EXPECT_TRUE(r.ran);  // did NOT crash; ran clean.
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') out.pop_back();
    if (!out.empty() && out.back() == '\r') out.pop_back();
    BEZEL_EXPECT_EQ(out, std::string("110"));
}

// §28.12 / §28.10 / §30.9 (v0.350): the catch arm RETURNS from an ASYNC method
// (exit mode 5 — the body scope is disposed at the return SITE, not after the
// finally).  Same edge, DisposeAsync awaited: 1110.
BEZEL_TEST(await_using_caught_arm_3121,
           arm_return_async_awaits_dispose_async_spec_28_12_7705) {
    BEZEL_EXPECT_EQ(runStdoutChecked(programArmReturn()), std::string("1110"));
}

// §28.12 (v0.350) NESTED: TWO `await using var` in the same try body — both
// awaited on the exception edge: 1120 (no destructors).
BEZEL_TEST(await_using_caught_arm_3121,
           two_resources_both_awaited_spec_28_12_7705) {
    BEZEL_EXPECT_EQ(runStdoutChecked(programTwoAwaitUsing()),
                    std::string("1120"));
}

// Negative control — the SIMPLE shape (throw inside `do await using`, caught by
// an outer try WITHOUT a `finally`): 10, so the oracle is not "always
// 1110-shaped".
BEZEL_TEST(await_using_caught_arm_3121,
           simple_throw_shape_awaits_dispose_async_spec_28_12_7705_negative_control) {
    BEZEL_EXPECT_EQ(runStdoutChecked(programSimpleShape()),
                    std::string("10"));
}
