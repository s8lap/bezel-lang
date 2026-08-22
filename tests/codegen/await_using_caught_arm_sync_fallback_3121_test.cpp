// Regression tests for #3121 — the §28.12 exception-edge gap the #2892 ordering
// ruling exposed.
//
// WHAT THE DOCUMENT REQUIRES.  Spec §28.12 *Asynchronous disposal* (v0.252):
//
//     Asynchronous cleanup runs only where the program wrote `await using`
//     (whose implicit `finally` awaits `DisposeAsync` on both the normal and
//     exception edges of an async body, with §29.7's synchronous fallback on
//     early iterator abandonment) or awaited a direct `DisposeAsync()` call.
//
// WHAT THE COMPILER DOES.  Every exception-edge disposal goes through
// `emitDisposals(..., isUnwind=true)`, whose `asyncDispose` arm (#3121's
// `LlvmBackend_Disposal.cpp:5558`) calls the synchronous `Dispose()` (the
// class destructor + physical reclaim) rather than awaiting `DisposeAsync()`.
// That is a long-standing recorded deferral — the same one `tests/codegen/
// async_test.cpp`'s §28.12 header states ("the async await-across-active-
// unwind path is a deferred increment, reported as the sync-dispose fallback
// decision") and the one #1422 records for `TaskScope.DisposeAsync` on its
// exception edge ("awaiting across an already-active unwind is a deferred
// increment").
//
// WHAT #2892 CHANGED, AND WHY IT WAS DELIBERATE.  Before the #2892 ruling
// (§28.10 / §31.2, v0.241) retired #1263's keep-body-scope-alive-past-finally
// deferral, ONE exception edge happened to award the async path to `await
// using`: the catch arm of a try that has BOTH a `finally` and a matching
// arm.  #2869 (v0.240) gave that edge a dedicated exit mode (mode 6) which
// disposed the try body scope with `isUnwind=false` + `undischargedOwned=true`
// — deliberately, "so that no await is lost".  With the body scope now
// unwind-disposed AT THE THROW SITE (the #2892 ruling's structural answer),
// the mode-6 carve-out and the `undischargedOwned` distinction are gone, and
// the caught-arm edge joins every other exception edge on the sync-dispose
// fallback.  The narrowing is one line of behaviour on one shape and is now
// uniform with the rest of the compiler; the #3121 filing records it as a
// measurement rather than re-deriving it.
//
// MEASURED, same program, ASan-clean and disposed-exactly-once in both
// states (`AsyncRes.DisposeAsync` adds 10, its destructor adds 1, the catch
// arm adds 100, the finally adds 1000):
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
// * before the #2892 migration (commit 663a68e): **1110**  (DisposeAsync awaited,
//   no destructor ran)
// * after it (and currently, on HEAD):       **1101**  (destructor ran, §28.12
//   synchronous fallback)
//
// Every other exception edge — plain throw inside the await-using scope caught
// by an outer try (no finally), throw that escapes the try, throw from a
// `using var` body caught by a try with finally but no arm, throw caught by a
// rethrowing arm — printed the `…01` shape both before AND after #2892.  The
// catch-arm of a try-with-finally is the ONE edge that lost its async path,
// and it lost it because the deferral that carried it (#1263's keep-alive)
// was the same deferral #2892 retired.
//
// WHAT WOULD CLOSE IT.  The deferred async unwind driver — the same one
// §28.10/§29.7 name and #1422 waits on.  Without it, no path on this edge
// can suspend; §28.12's parenthetical over-promises for the exception edge in
// this shape until the driver exists.  Every test below pins the §28.12
// gap's observable value so the narrowing is not silently widened OR narrowed
// further: a future async-unwind-driver landing that flips the sync fallback
// to await-DisposeAsync on this edge will need to update the expected
// counters (1101 → 1110 etc.), at which point the test fails LOUDLY with the
// exact pin and forces the change to be made consciously.
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
// canonical shape.  Printf'd value: 1101 (dtor=1, catch=100, finally=1000).
// Pre-#2892 would have printed 1110 (DisposeAsync awaited, no destructor ran).
// Pinned value here is the post-#2892 SYNC-FALLBACK oracle; flipping it to
// 1110 requires the async unwind driver (see file header).
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

// Variant: the catch arm RETHROWS the exception.  Same edge, same sync
// fallback — the exception is in flight again past the finally so the async
// path is still unavailable.  The observable MUST be printed BEFORE the
// rethrow: the exception unwinds out of Main past the finally, so a
// `Console.WriteLine(AsyncRes.disposed)` after the try/catch/finally block
// would never run.  The post-dtor-+catch value 101 is what we observe at the
// point of the print; the finally's +1000 has not yet been applied.  A fix
// that wrongly landed the async path here would print 110 (DisposeAsync
// +100, no dtor) instead of 101 — the test would fail loudly.
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
// §28.12 catch-arm edge, both expected to take the sync fallback.  Counter
// is 1102 (dtor 1 + dtor 1 + catch 100 + finally 1000).  Pre-#2892 would have
// been 1120 (two DisposeAsync awaited, no destructors).  Pins the gap on
// NESTED resources on the same edge — a future async unwind driver that
// awaited only the FIRST resource on this edge would show 1111 (one
// DisposeAsync + one dtor + catch + finally) and the test would fail loudly.
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
// around it) was already on the sync fallback BEFORE #2892 and stays on it
// AFTER — pinning 1 here proves the oracle distinguishes the two shapes and
// the test is not just "everything is 1101-shaped".  This mirrors the
// existing `async_28_12.await_using_fault_path_sync_dispose_fallback` test
// (async_test.cpp:1711) so the two values agree on the same expectation.
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
// §28.12 catch-arm of try-with-finally+matching-arm: the #3121 pin.
// ===========================================================================

// §28.12 / §28.10 (v0.252): the catch arm FALLS THROUGH.  Pre-#2892 this
// shape printed 1110 (DisposeAsync awaited); the #2892 ruling retired the
// deferral that carried it.  Today: 1101 (destructor ran, sync fallback).
// This pin forces any future async unwind driver landing to consciously
// update the expected counter.
BEZEL_TEST(await_using_caught_arm_3121, canonical_sync_fallback_spec_28_12) {
    BEZEL_EXPECT_EQ(runStdoutChecked(programCanonical()), std::string("1101"));
}

// §28.12 / §28.10 (v0.252): the catch arm RETHROWS the exception.  Same
// caught-arm edge as the canonical case; same sync fallback.  Pinned here so
// that the "catch swallows vs catch rethrows" axis is exercised independently
// and a partial fix that only handled the swallow case would fail this test.
// The observable is the value at the catch arm's print site: dtor + catch =
// 1 + 100 = 101.  An async-path fix would print 110 (DisposeAsync +100, no
// dtor) and the test would fail.
BEZEL_TEST(await_using_caught_arm_3121, rethrow_sync_fallback_spec_28_12) {
    // Exception escapes Main past the finally → WEXITSTATUS(non-zero).  The
    // observable therefore travels on stdout BEFORE the rethrow unwinds, and
    // ASan/LSan still gate the run via runAotChecked.
    auto r = runAotChecked(programRethrow());
    BEZEL_EXPECT_TRUE(r.ran);  // did NOT crash; ran clean.
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') out.pop_back();
    if (!out.empty() && out.back() == '\r') out.pop_back();
    BEZEL_EXPECT_EQ(out, std::string("101"));
}

// §28.12 / §28.10 / §30.9 (v0.252): the catch arm RETURNS from an ASYNC
// method (exit mode 5 — the body scope is disposed at the return SITE, not
// after the finally).  Same edge, same sync fallback.  Pinned here so the
// async-method return-from-arm shape is exercised independently and a fix
// that landed only on the sync method's catch arm would fail this test.
BEZEL_TEST(await_using_caught_arm_3121, arm_return_async_sync_fallback_spec_28_12) {
    BEZEL_EXPECT_EQ(runStdoutChecked(programArmReturn()), std::string("1101"));
}

// §28.12 (v0.252) / §28.10 NESTED: TWO `await using var` declared in the
// same try body — both on the §28.12 catch-arm edge, both expected to take
// the sync fallback.  Counter is 1102 (two destructors + catch + finally).
// Pre-#2892: 1120 (two DisposeAsync awaited, no destructors).  Pins the gap
// on N resources on the same edge.
BEZEL_TEST(await_using_caught_arm_3121, two_resources_both_sync_fallback_spec_28_12) {
    BEZEL_EXPECT_EQ(runStdoutChecked(programTwoAwaitUsing()),
                    std::string("1102"));
}

// Negative control — the SIMPLE shape (throw inside `do await using`,
// caught by an outer try WITHOUT a `finally`) was already on the sync
// fallback before #2892 and stays on it.  Pinned at 1 here so a regression
// that swings this shape to the async path (1010) gets caught separately
// from the #3121 pins, and so the oracle is not "always 1101-shaped".
BEZEL_TEST(await_using_caught_arm_3121,
           simple_throw_shape_unchanged_spec_28_12_negative_control) {
    BEZEL_EXPECT_EQ(runStdoutChecked(programSimpleShape()),
                    std::string("1"));
}
