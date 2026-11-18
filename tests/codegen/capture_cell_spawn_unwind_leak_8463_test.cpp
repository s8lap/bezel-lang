// fix #8463 (§27.5 / §28.9 / §28.14, v0.359): the capture cell of a LOCAL
// captured by a `TaskScope.Spawn` child is reclaimed on the frame's NORMAL
// exit only; when the enclosing async frame leaves by an EXCEPTION, the cell
// is never reclaimed.
//
// THE DEFECT. `collectCellFreeEligible` (src/codegen/LlvmBackend_Setup.cpp)
// classifies a local captured by a `TaskScope.Spawn` lambda as free-eligible at
// scope end and tags it `unwindUnsafeCellFree_` (#2334): the TaskScope's
// EXCEPTION-edge disposal is a NON-BLOCKING zombie drain
// (`bz_task_scope_dispose_unwind`, pinned by `taskscope_behavior_610,
// exception_edge_drain_is_nonblocking_zombie_1422`), so a still-running child
// may read the cell and the scope-end FREE must not run on the unwind edge.
// `Disposal::skipCellFreeOnUnwind` therefore SKIPS that free — and the cell is
// LEAKED on that edge.
//
// MEASURED on the base (r30-c1 @ f2589f706), with the issue's own repro
// verbatim: "SUMMARY: AddressSanitizer: 400 byte(s) leaked in 100
// allocation(s)" — one 4-byte `int` cell per call, 100 calls.
//
// THE FIX. The capture cell is reclaimed by COUNTING instead of by a scope-end
// free: the env that holds the cell announces itself with `bz_cell_retain` and
// drops that reference in its envdtor, so the frame's scope-end RELEASE
// cannot outrun a child that is still reading the cell (a counted reader keeps
// the cell alive by construction) and the cell is freed by whoever lets go
// LAST. That is the #4509 protocol (`retainCellInEnv` / `Disposal::cellRelease`
// / `bz_cell_release`), armed here for the TaskScope.Spawn shape.
//
// THE SPEC. §27.5 (v0.359): "Captured variables are heap-allocated in a closure
// object". §28.10 (v0.359): "Disposal happens on every exit path: normal
// completion, `break`, `return`, or exception". §28.14 (v0.359) *Quiescence*:
// "Disposal of the bounding object must guarantee that no stored callback is
// invoked after disposal completes (by draining in-flight work or severing
// registrations before teardown)" — and stdlib §6.10 (v0.359), `TaskScope`:
// "**Quiescence (normative, §28.14)**: `DisposeAsync` completes only after
// every child task has completed and been reclaimed; no `scoped` callback runs
// after it returns." The compiler-allocated capture cell is reclaimed on the
// local's scope end on EVERY edge; the frame's exit edge is one of them.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own output and exit value as well as the leak count, so a
// silent zero-fill cannot pass. The rows LOOP (100 iterations inside the
// program, the issue's own repro), so the leak cannot hide under the
// sanitizer's noise.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor the runtime's §28.9 over-release check
// fired. `runAotChecked` reports an abort and any sanitizer report as
// `ran == false`.
void didNotCrash(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("refcount over-release") == std::string::npos);
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    didNotCrash(r, exitCode, out);
    assertNoLeak(r);
}

// The issue's own repro, verbatim. The captured local is `n` (an `int`, so a
// 4-byte capture cell); the frame leaves by an exception AFTER the scope has
// been joined and disposed, so the child is done and the cell is dead weight
// on the frame's exit.
const char* kIssueRepro =
    "module M; "
    "public class Program "
    "  public static async method R(i: int): Task "
    "    var n := i + 1; "
    "    do "
    "      await using var scope := new TaskScope(); "
    "      scope.Spawn(async lambda() => do "
    "        await Task.Delay(1); "
    "        Console.WriteLine(n); "
    "      end); "
    "      await scope.Join(); "
    "    end; "
    "    throw new ArgumentException(\"boom\"); "
    "  end R; "
    "  public static async method Main(): Task "
    "    for var i in 0..<100 do "
    "      try "
    "        await Program.R(i); "
    "      catch var e: ArgumentException => "
    "        Console.WriteLine(\"caught\"); "
    "      end; "
    "    end for; "
    "  end Main; "
    "end Program; ";

// The output the issue's repro prints: each iteration prints the child's `n`
// and then the catch arm's "caught" — the child has completed before
// `scope.Join()` returns, so the order is fixed.
std::string issueReproOutput() {
    std::string out;
    for (int i = 1; i <= 100; ++i) {
        out += std::to_string(i);
        out += "\ncaught\n";
    }
    return out;
}

// THE REGRESSION ROW: the issue's repro, verbatim. RED on the base (the cell
// leaks on the frame's exception edge), GREEN with the fix.
BEZEL_TEST(capture_cell_spawn_unwind_leak_8463,
           spawn_capture_cell_reclaimed_when_frame_leaves_by_exception) {
    auto r = runAotChecked(kIssueRepro);
    expectClean(r, 0, issueReproOutput());
}

// The issue's own second measurement — "8 bytes when the captured local is a
// `Task`" — i.e. the same shape with a CLASS-typed captured local (the cell
// holds a pointer, not an int). Same RED on the base, same GREEN with the fix.
BEZEL_TEST(capture_cell_spawn_unwind_leak_8463,
           spawn_capture_cell_class_typed_reclaimed_on_exception_edge) {
    const std::string source =
        "module M; "
        "public class Program "
        "  public static async method R(i: int): Task "
        "    var t := Task.Delay(1); "
        "    do "
        "      await using var scope := new TaskScope(); "
        "      scope.Spawn(async lambda() => do "
        "        await Task.Delay(1); "
        "        await t; "
        "        Console.WriteLine(\"child\"); "
        "      end); "
        "      await scope.Join(); "
        "    end; "
        "    throw new ArgumentException(\"boom\"); "
        "  end R; "
        "  public static async method Main(): Task "
        "    for var i in 0..<100 do "
        "      try "
        "        await Program.R(i); "
        "      catch var e: ArgumentException => "
        "        Console.WriteLine(\"caught\"); "
        "      end; "
        "    end for; "
        "  end Main; "
        "end Program; ";
    std::string expected;
    for (int i = 0; i < 100; ++i)
        expected += "child\ncaught\n";
    auto r = runAotChecked(source);
    expectClean(r, 0, expected);
}

// THE DRAIN EDGE: the frame leaves by an exception WHILE the spawned child is
// still running (the throw is INSIDE the scope, so the TaskScope's
// exception-edge disposal is the non-blocking zombie drain). The cell must
// SURVIVE the frame's scope end — the child still reads it — and must then be
// reclaimed when the child is done. This row is what keeps the fix from
// trading the leak for a use-after-free: a scope-end FREE here would be the
// #2334 crash, and the row's leak assertion is what makes the cell's LATE
// reclaim (the child's envdtor) observable.
//
// The child READS the captured local (that read is the use-after-free window)
// but prints nothing — its condition is never true — so the program's output
// is the deterministic "caught" per iteration and the row's output assertion
// is not a race with the children's timing.
//
// RED on the base (the cell leaks: the scope-end free is skipped on the unwind
// edge), GREEN with the fix.
BEZEL_TEST(capture_cell_spawn_unwind_leak_8463,
           spawn_capture_cell_reclaimed_while_child_still_running) {
    const std::string source =
        "module M; "
        "public class Program "
        "  public static async method R(i: int): Task "
        "    var n := i + 1; "
        "    do "
        "      await using var scope := new TaskScope(); "
        "      scope.Spawn(async lambda() => do "
        "        await Task.Delay(3); "
        "        if n > 1000 then Console.WriteLine(\"impossible\"); end; "
        "      end); "
        "      if i >= 0 then "
        "        throw new ArgumentException(\"boom\"); "
        "      end; "
        "      await scope.Join(); "
        "    end; "
        "  end R; "
        "  public static async method Main(): Task "
        "    for var i in 0..<100 do "
        "      try "
        "        await Program.R(i); "
        "      catch var e: ArgumentException => "
        "        Console.WriteLine(\"caught\"); "
        "      end; "
        "    end for; "
        "    await Task.Delay(300); "
        "  end Main; "
        "end Program; ";
    std::string expected;
    for (int i = 0; i < 100; ++i)
        expected += "caught\n";
    auto r = runAotChecked(source);
    expectClean(r, 0, expected);
}

}  // namespace
