// ASan/LSan AOT probes for TaskScope — stdlib §6.10 / spec §28.14 (v0.175,
// #808).
//
// The JIT behaviour tests (taskscope_behavior_610_test.cpp) check VALUES; the
// probes here compile the same shapes to a NATIVE binary linked against an
// ASan+LSan-instrumented runtime and assert the memory discipline:
//   - the FULL NURSERY LIFECYCLE (spawn × N with captures, join, dispose)
//     leaks nothing and frees nothing twice (child tasks, closure cells,
//     capture boxes, the scope struct, the internal cancellation cell);
//   - the FAULT PATH (a child fault re-raised at Join; a swallowed sibling
//     fault; the exception-edge unwind drain) neither leaks the exception
//     objects nor touches freed memory (§6.10: DisposeAsync always swallows;
//     the runtime disposes swallowed faults via their vtable).
//
// Mirrors the runUnderAsan recipe of array_reverse_chain_466_test.cpp
// (lowerToObject + the memoized ASan runtime object + clang link).

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>

#include "ast/Node.hpp"
#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "native_test_harness.hpp"
#include "support/run_under_asan.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <filesystem>

namespace {

namespace fs = std::filesystem;

// Compile `source` under ASan + LSan, run it, and check the exit code.
// Returns 0 (clean), 1 (sanitizer problem or wrong value), -1 (toolchain
// unavailable: SKIP).
int runUnderAsan(const std::string& source, int expectedExitCode) {
    // fix #3653: forwards to the ONE shared implementation.
    return bezel::test::runUnderAsanValue(source, expectedExitCode, "bezel_tscope808");
}

void expectAsanClean(const std::string& source, int expectedExitCode) {
    int a = runUnderAsan(source, expectedExitCode);
    if (a == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(a, 0);
}

}  // namespace

// §6.10 / §28.14: the FULL NURSERY LIFECYCLE — two spawned children (static-
// counter bodies: a lambda capturing an enclosing-frame LOCAL would also
// exercise the PRE-EXISTING async capture-cell leak — reproduced without any
// TaskScope, noted in cts_register_timer_1413_test.cpp — which would mask
// what THIS probe measures), Join, dispose. No leak (child tasks, closure
// cells, the scope struct, the internal cancel cell), no UAF, no double-free.
BEZEL_TEST(taskscope_asan_808, full_nursery_lifecycle_clean) {
    expectAsanClean(
        "module M; import System.Threading; "
        "class Prog "
        "  public static _count: int := 0; "
        "  public static async method Run(): Task<int> "
        "    await using var scope := new TaskScope(); "
        "    scope.Spawn(async lambda() => do "
        "      await Task.Delay(4); "
        "      Prog._count := Prog._count + 1; "
        "    end); "
        "    scope.Spawn(async lambda() => do "
        "      await Task.Delay(2); "
        "      Prog._count := Prog._count + 1; "
        "    end); "
        "    await scope.Join(); "
        "    return Prog._count; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var n := await Prog.Run(); "
        "    if n = 2 then return 0; end; "
        "    return 1; "
        "  end Main; "
        "end Prog;",
        0);
}

// §6.10 FAULT PATH: the first fault re-raises at Join (caught), the sibling's
// fault-free cancellation exit and the swallowed non-first fault leak nothing
// — the runtime disposes swallowed exception objects via their vtable.
BEZEL_TEST(taskscope_asan_808, fault_path_first_fault_and_swallow_clean) {
    expectAsanClean(
        "module M; import System.Threading; "
        "class Prog "
        "  public static async method Main(): Task<int> "
        "    var caught := false; "
        "    await using var scope := new TaskScope(); "
        "    scope.Spawn(async lambda() => do "
        "      await Task.Delay(2); "
        "      throw new InvalidOperationException(\"first\"); "
        "    end); "
        "    scope.Spawn(async lambda() => do "
        "      await Task.Delay(4); "
        "      throw new ArgumentException(\"second — swallowed\"); "
        "    end); "
        "    try "
        "      await scope.Join(); "
        "    catch "
        "      var e: Exception => do caught := true; end "
        "    end; "
        "    if caught then return 0; end; "
        "    return 1; "
        "  end Main; "
        "end Prog;",
        0);
}

// §6.10 EXCEPTION-EDGE drain: an outer throw between Spawn and Join unwinds
// through the scope's disposal (cancel + non-blocking zombie drain + swallow);
// the still-running child, its task, its closure, its own FAULT object, and
// the scope itself are all reclaimed — no leak, no UAF. (The children's
// bodies are capture-free for the same pre-existing-cell-leak reason as the
// lifecycle probe above; the cancellation-VALUE observation lives in the JIT
// behaviour tests.)
BEZEL_TEST(taskscope_asan_808, exception_edge_unwind_drain_clean) {
    expectAsanClean(
        "module M; import System.Threading; "
        "class Prog "
        "  public static method Always(): bool => true; "
        "  public static async method Run(): Task "
        "    await using var scope := new TaskScope(); "
        "    scope.Spawn(async lambda() => do "
        "      var i := 0; "
        "      while i < 20 do "
        "        await Task.Delay(2); "
        "        i := i + 1; "
        "      end; "
        "    end); "
        "    scope.Spawn(async lambda() => do "
        "      await Task.Delay(2); "
        "      throw new InvalidOperationException(\"drained and swallowed\"); "
        "    end); "
        "    if Prog.Always() then "
        "      throw new ArgumentException(\"outer\"); "
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
        "    await Task.Delay(80); "
        "    if caught then return 0; end; "
        "    return 1; "
        "  end Main; "
        "end Prog;",
        0);
}

// issue #1422 residual: §6.10 EXCEPTION-EDGE drain with SEVERAL children,
// TWO of which fault DURING the non-blocking zombie drain (after the outer
// throw already fired and the scope's token was cancelled) — extends the
// single-second-fault case above to the general "always swallows their
// faults" contract (stdlib §6.10 v0.228) under concurrent zombie faults, and
// to a child that is still suspended at an `await` when the drain begins
// (the first child's while-loop). All of it — the still-running child, the
// two that fault, the one that completes cleanly, their tasks, their own
// fault objects, and the scope itself — must be reclaimed exactly once: no
// leak, no UAF, no double-free. (Capture-free bodies for the same pre-
// existing async-closure capture-cell leak reason noted above — orthogonal
// to this drain and reproducible without any TaskScope; see issue #2334.)
BEZEL_TEST(taskscope_asan_808, exception_edge_several_children_multi_fault_clean) {
    expectAsanClean(
        "module M; import System.Threading; "
        "class Prog "
        "  public static method Always(): bool => true; "
        "  public static async method Run(): Task "
        "    await using var scope := new TaskScope(); "
        "    scope.Spawn(async lambda() => do "
        "      var i := 0; "
        "      while i < 20 do "
        "        await Task.Delay(2); "
        "        i := i + 1; "
        "      end; "
        "    end); "
        "    scope.Spawn(async lambda() => do "
        "      await Task.Delay(2); "
        "      throw new InvalidOperationException(\"first drained fault\"); "
        "    end); "
        "    scope.Spawn(async lambda() => do "
        "      await Task.Delay(4); "
        "      throw new ArgumentNullException(\"second drained fault\"); "
        "    end); "
        "    scope.Spawn(async lambda() => do "
        "      await Task.Delay(6); "
        "    end); "
        "    if Prog.Always() then "
        "      throw new ArgumentException(\"outer\"); "
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
        "    await Task.Delay(100); "
        "    if caught then return 0; end; "
        "    return 1; "
        "  end Main; "
        "end Prog;",
        0);
}
