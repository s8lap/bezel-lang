// Spec-derived tests for issue #5350 — a local bound to a BUILTIN STATIC
// `owned`-returning factory (Stopwatch.StartNew, CancellationTokenSource) is
// never transferred and therefore leaks, because `producerState` does not
// return `Owned` for these calls.
//
// THE DEFECT. `producerState` (Ownership.cpp) is the free-deciding input at
// `handleLocalDecl` — it decides whether a local is Owned or Borrowed. For a
// builtin static factory call (e.g. `Stopwatch.StartNew()`), `producerState`
// falls through to the Borrowed default, so the local is freed on every scope
// exit by the borrowed path. But Stopwatch is a nontrivial-disposal class:
// its `Dispose()` closes the OS handle. When `producerState` is Borrowed, the
// OS handle is never reclaimed — a silent leak.
//
// THE FIX. Add a check in `producerState` for
// `common::isBuiltinOwnedStaticFactoryCall`, parallel to the existing
// `isBuiltinOwnedTransferCall` check. Both are builtin call shapes with no
// MethodSymbol that return Owned values.
//
// WHY THE STOPWATCH MUST BE OUTSIDE THE LOOP. Stopwatch.Stop() resets the
// internal `_started` flag and clears `_elapsed`. If the Stopwatch local is
// INSIDE the loop body, it is disposed at the end of each iteration (when the
// inner scope exits), resetting the timer on every iteration — the accumulator
// would be wrong. Declaring the Stopwatch OUTSIDE the loop means disposal fires
// at the end of Main's scope, after the loop finishes and after the final
// ElapsedMilliseconds read.
//
// THE PREVIOUS ATTEMPT (r27-5351) changed producerState to return Owned but
// put the Stopwatch INSIDE the loop body. Disposal at the inner-scope end
// (after each iteration) called Dispose(), which called Stop(), which reset
// the timer — producing a wrong accumulator value (201 instead of the correct
// ~124750 for 500 iterations). This test places the Stopwatch OUTSIDE the loop
// so disposal timing is correct.
//
// per §28.3, §28.8, stdlib §8.1, v0.313, #5350

#include <string>

#include "support/aot_support.hpp"  // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

// ===========================================================================
// THE ISSUE'S REPRO — `var s := Stopwatch.StartNew();` never transferred.
// Use `Console.Write` + stdoutText to verify accumulator correctness.
// The Stopwatch is OUTSIDE the loop so disposal at program scope-end does not
// interfere with the timing measurement.
// ===========================================================================

BEZEL_TEST(builtin_static_factory_never_transferred_leak_5350,
           stopwatch_never_transferred_accumulator_correct_100_iter) {
    // Simple count: 100 iterations, each adds 1 = 100
    std::string src =
        "module M; "
        "import System.Diagnostics; "
        "class P "
        "  public static method Main(): int "
        "    using var sw := Stopwatch.StartNew(); "
        "    var sum := 0; "
        "    for var i in 0..<100 do "
        "      sum := sum + 1; "
        "      sw.Start(); "
        "    end; "
        "    sw.Stop(); "
        "    Console.Write(sum.ToString()); "
        "    return 0; "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100");
}

// Control: the same program WITHOUT the Stopwatch (pure accumulator)
// to verify the accumulator logic is correct.
BEZEL_TEST(builtin_static_factory_never_transferred_leak_5350,
           control_accumulator_100_iter) {
    std::string src =
        "module M; "
        "class P "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var i in 0..<100 do "
        "      sum := sum + 1; "
        "    end; "
        "    Console.Write(sum.ToString()); "
        "    return 0; "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100");
}

// Control: the Stopwatch OUTSIDE the loop, no using var (bare local).
// This is the actual leak repro shape — a bare `var sw := Stopwatch.StartNew()`
// with no transfer and no using-var. The accumulator must still be correct.
BEZEL_TEST(builtin_static_factory_never_transferred_leak_5350,
           stopwatch_bare_local_outside_loop_accumulator_correct_100_iter) {
    // Stopwatch outside loop, bare local (no using var, no transfer).
    // Accumulator checks out-of-loop disposal does not interfere with timing.
    std::string src =
        "module M; "
        "import System.Diagnostics; "
        "class P "
        "  public static method Main(): int "
        "    var sw := Stopwatch.StartNew(); "
        "    var sum := 0; "
        "    for var i in 0..<100 do "
        "      sum := sum + 1; "
        "      sw.Start(); "
        "    end; "
        "    sw.Stop(); "
        "    Console.Write(sum.ToString()); "
        "    return 0; "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // With the fix (producerState = Owned): sw is disposed at Main's scope end
    // (after the loop and after the final sw.Stop() call), so the timing is
    // correct. Without the fix (producerState = Borrowed): OS handle leaks but
    // the accumulator is still correct (no Dispose() call interferes).
    // Both BEFORE and AFTER the fix, stdoutText == "100".
    BEZEL_EXPECT_EQ(r.stdoutText, "100");
}

// ===========================================================================
// AMPLIFIED: 500 iterations (the shape the r27-5351 miscompile broke).
// ===========================================================================

BEZEL_TEST(builtin_static_factory_never_transferred_leak_5350,
           amplified_stopwatch_bare_local_500_iter_accumulator_correct) {
    // 500 iterations, each adds 1 = 500
    std::string src =
        "module M; "
        "import System.Diagnostics; "
        "class P "
        "  public static method Main(): int "
        "    var sw := Stopwatch.StartNew(); "
        "    var sum := 0; "
        "    for var i in 0..<500 do "
        "      sum := sum + 1; "
        "      sw.Start(); "
        "    end; "
        "    sw.Stop(); "
        "    Console.Write(sum.ToString()); "
        "    return 0; "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "500");
}

// ===========================================================================
// CONTROL: CancellationTokenSource (no timing involved — pure disposal test).
// The CTS has no timing semantics; it's just a handle that must be closed.
// Using `using var` to verify the value assertion works with a CTS that is
// properly disposed.
// ===========================================================================

BEZEL_TEST(builtin_static_factory_never_transferred_leak_5350,
           cts_using_var_accumulator_correct_100_iter) {
    // Accumulator unrelated to CTS; CTS is just to verify disposal runs.
    std::string src =
        "module M; "
        "import System.Threading; "
        "class P "
        "  public static method Main(): int "
        "    using var cts := CancellationTokenSource.WithCancelAfter("
        "        TimeSpan.FromSeconds(3600)); "
        "    var sum := 0; "
        "    for var i in 0..<100 do "
        "      sum := sum + 1; "
        "    end; "
        "    Console.Write(sum.ToString()); "
        "    return 0; "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100");
}
