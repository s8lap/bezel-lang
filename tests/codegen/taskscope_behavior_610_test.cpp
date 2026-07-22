// JIT behaviour tests for TaskScope — stdlib §6.10 (v0.175, #808), on the
// §28.14 storage-bounded capture mechanism.
//
// §6.10 semantics under test:
//   - Spawn starts the child immediately on the current domain; children run
//     concurrently and complete before Join returns.
//   - Join is the SOLE re-raise site: the FIRST fault re-raises there
//     (first-fault-wins, no aggregation), and the first fault cancels the
//     siblings via the scope's token (cooperative, §30.8).
//   - Spawn after close (Join returned / disposal began) raises
//     InvalidOperationException — deterministic, never a silent drain
//     extension.
//   - missing `await Join()` on a non-provable path → IOE from DisposeAsync.
//   - the exception edge cancels + drains + SWALLOWS child faults (the
//     original exception propagates unchanged).
//   - CancelAfter bounds the whole scope (the #1413 timer machinery).
//   - captured owned locals ('using var' sources declared before the scope)
//     are disposed exactly once, after the drain (§28.14 storage-bounded
//     capture; destructor counter).

#include <string>

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// §6.10: two spawned children run and complete before `await Join()` returns —
// both counter increments are visible after the join.
BEZEL_TEST(taskscope_behavior_610, children_complete_before_join_returns) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class Prog "
        "  public static async method Main(): Task<int> "
        "    var count := 0; "
        "    await using var scope := new TaskScope(); "
        "    scope.Spawn(async lambda() => do "
        "      await Task.Delay(8); "
        "      count := count + 1; "
        "    end); "
        "    scope.Spawn(async lambda() => do "
        "      await Task.Delay(3); "
        "      count := count + 1; "
        "    end); "
        "    await scope.Join(); "
        "    return count; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// §6.10: the FIRST fault re-raises at `await Join()` (catchable there), and
// the sibling observes cancellation through the scope's Token (§30.8
// cooperative cancellation) — first-fault-wins + cancel-siblings.
BEZEL_TEST(taskscope_behavior_610, first_fault_reraises_and_cancels_siblings) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class Prog "
        "  public static async method Main(): Task<int> "
        "    var caught := false; "
        "    var sawCancel := false; "
        "    await using var scope := new TaskScope(); "
        "    var tok := scope.Token; "
        "    scope.Spawn(async lambda() => do "
        "      await Task.Delay(5); "
        "      throw new InvalidOperationException(\"boom\"); "
        "    end); "
        "    scope.Spawn(async lambda() => do "
        "      var i := 0; "
        "      while i < 500 and not tok.IsCancellationRequested do "
        "        await Task.Delay(2); "
        "        i := i + 1; "
        "      end; "
        "      if tok.IsCancellationRequested then sawCancel := true; end; "
        "    end); "
        "    try "
        "      await scope.Join(); "
        "    catch "
        "      var e: InvalidOperationException => do caught := true; end "
        "    end; "
        "    if caught and sawCancel then return 0; end; "
        "    return 1; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §6.10: Spawn AFTER the scope closed (Join returned) raises
// InvalidOperationException — deterministic, catchable at the Spawn site.
BEZEL_TEST(taskscope_behavior_610, spawn_after_close_raises_ioe) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class Prog "
        "  public static async method Main(): Task<int> "
        "    var threw := false; "
        "    await using var scope := new TaskScope(); "
        "    await scope.Join(); "
        "    try "
        "      scope.Spawn(async lambda() => do await Task.Delay(1); end); "
        "    catch "
        "      var e: InvalidOperationException => do threw := true; end "
        "    end; "
        "    if threw then return 0; end; "
        "    return 1; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §6.10: missing `await Join()` on a path the compiler cannot prove (a
// conditional join that does not execute) → InvalidOperationException from
// DisposeAsync. The IOE faults the helper's task at its scope disposal and is
// caught by the awaiter — exactly the "IOE from DisposeAsync" backstop.
BEZEL_TEST(taskscope_behavior_610, missing_join_runtime_ioe_from_dispose) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class Prog "
        "  public static method Cond(): bool => false; "
        "  public static async method Run(): Task "
        "    await using var scope := new TaskScope(); "
        "    if Prog.Cond() then "
        "      await scope.Join(); "
        "    end; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var caught := false; "
        "    try "
        "      await Prog.Run(); "
        "    catch "
        "      var e: InvalidOperationException => do caught := true; end "
        "    end; "
        "    if caught then return 0; end; "
        "    return 1; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §6.10: the EXCEPTION edge — an exception thrown between Spawn and Join
// cancels the scope's token, drains the children, and SWALLOWS their faults
// (a child's own InvalidOperationException never masks the original
// ArgumentException; Join is the sole re-raise site).
BEZEL_TEST(taskscope_behavior_610, exception_edge_drains_and_swallows) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class Prog "
        "  public static method Always(): bool => true; "
        "  public static async method Run(): Task "
        "    await using var scope := new TaskScope(); "
        "    var tok := scope.Token; "
        "    scope.Spawn(async lambda() => do "
        "      var i := 0; "
        "      while i < 500 and not tok.IsCancellationRequested do "
        "        await Task.Delay(2); "
        "        i := i + 1; "
        "      end; "
        "    end); "
        "    scope.Spawn(async lambda() => do "
        "      await Task.Delay(2); "
        "      throw new InvalidOperationException(\"child boom\"); "
        "    end); "
        "    if Prog.Always() then "
        "      throw new ArgumentException(\"outer boom\"); "
        "    end; "
        "    await scope.Join(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var caught := false; "
        "    try "
        "      await Prog.Run(); "
        "    catch "
        "      var e: ArgumentException => do caught := true; end "
        "    end; "
        "    await Task.Delay(50); "
        "    if caught then return 0; end; "
        "    return 1; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §6.10: CancelAfter bounds the whole scope — a long-running child observes
// the token cancel and exits; Join returns normally (no fault: the child
// completes by choice, it is not aborted).
BEZEL_TEST(taskscope_behavior_610, cancel_after_bounds_the_scope) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class Prog "
        "  public static async method Main(): Task<int> "
        "    var sawCancel := false; "
        "    await using var scope := new TaskScope(); "
        "    var tok := scope.Token; "
        "    scope.CancelAfter(TimeSpan.FromMilliseconds(15)); "
        "    scope.Spawn(async lambda() => do "
        "      var i := 0; "
        "      while i < 500 and not tok.IsCancellationRequested do "
        "        await Task.Delay(2); "
        "        i := i + 1; "
        "      end; "
        "      if tok.IsCancellationRequested then sawCancel := true; end; "
        "    end); "
        "    await scope.Join(); "
        "    if sawCancel then return 0; end; "
        "    return 1; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §6.10: Cancel() cancels all children now (the same signal path).
BEZEL_TEST(taskscope_behavior_610, explicit_cancel_reaches_children) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class Prog "
        "  public static async method Main(): Task<int> "
        "    var sawCancel := false; "
        "    await using var scope := new TaskScope(); "
        "    var tok := scope.Token; "
        "    scope.Spawn(async lambda() => do "
        "      var i := 0; "
        "      while i < 500 and not tok.IsCancellationRequested do "
        "        await Task.Delay(2); "
        "        i := i + 1; "
        "      end; "
        "      if tok.IsCancellationRequested then sawCancel := true; end; "
        "    end); "
        "    await Task.Delay(5); "
        "    scope.Cancel(); "
        "    await scope.Join(); "
        "    if sawCancel then return 0; end; "
        "    return 1; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.14 / §6.10: captured owned sources (`using var` locals declared before
// the scope) are used by the spawned work and disposed EXACTLY ONCE — the
// destructor counter reads 2 after the drain and the sources' scope end.
BEZEL_TEST(taskscope_behavior_610, captured_owned_sources_disposed_exactly_once) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class Tracked "
        "  public static _count: int := 0; "
        "  public method Touch() end Touch; "
        "  destructor Tracked._count := Tracked._count + 1; end; "
        "end Tracked; "
        "class Prog "
        "  public static async method Run(): Task "
        "    using var a := new Tracked(); "
        "    using var b := new Tracked(); "
        "    await using var scope := new TaskScope(); "
        "    scope.Spawn(async lambda() => do "
        "      a.Touch(); "
        "      await Task.Delay(3); "
        "      a.Touch(); "
        "    end); "
        "    scope.Spawn(async lambda() => do "
        "      b.Touch(); "
        "      await Task.Delay(1); "
        "    end); "
        "    await scope.Join(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await Prog.Run(); "
        "    return Tracked._count; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// §6.10: a worker may Spawn MORE work while Join drains (drain completeness —
// the counter never reaches zero early); all of it completes before Join
// returns.
BEZEL_TEST(taskscope_behavior_610, respawn_during_drain_completes) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class Prog "
        "  public static async method Main(): Task<int> "
        "    var count := 0; "
        "    await using var scope := new TaskScope(); "
        "    scope.Spawn(async lambda() => do "
        "      await Task.Delay(2); "
        "      count := count + 1; "
        "      scope.Spawn(async lambda() => do "
        "        await Task.Delay(2); "
        "        count := count + 1; "
        "      end); "
        "    end); "
        "    await scope.Join(); "
        "    return count; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}
