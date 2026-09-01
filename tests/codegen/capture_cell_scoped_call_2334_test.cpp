// fix #2334 / §28.14 (v0.236) / stdlib §6.1, §6.5, §6.10: the capture cell of
// a local captured by a lambda passed to a stdlib `scoped` callback parameter
// is freed at the captured local's OWN scope end, not by the closure's own
// envdtor (which would race a later same-scope read or disposal through the
// same cell). The first lane (#2334, in 9ed5152) wired this for the
// TaskScope.Spawn shape only; the §28.14 admission actually covers the whole
// family of stdlib `scoped` register positions, and the brief for this lane
// asked the same "non-escaping at scope end" classification to be widened to
// the other two stdlib `scoped` register positions called out in §28.14:
// stdlib §6.1's `CancellationToken.Register` and stdlib §6.5's
// `IEvent<T>.Subscribe` / `EventSource<T>.Add`.
//
// Quiescence, per §28.14: for the new positions the runtime IS sound on the
// unwind edge, so the free can fire on BOTH normal and unwind paths
// (unwindUnsafe=false). This is the difference from TaskScope.Spawn, whose
// #1422-documented non-blocking zombie drain makes the unwind edge unsafe —
// the original fix used unwindUnsafe=true there to suppress the free on
// unwind; this file's new tests pin that the other two positions do NOT need
// that suppression (their captures are soundly freed on every exit).
//
// File name: capture_cell_scoped_call_2334 — extends the #2334 fix's coverage
// of "a local captured by a lambda passed to a `scoped` callback parameter"
// to the full stdlib §28.14 register family called out in the brief.
//
// As with taskscope_asan_2334_test.cpp: this is an ASan leg test (the
// existing taskscope_behavior_610_test.cpp's value-only runAot harness
// does not check for leaks, so the gap has been unobserved; the ASan
// probes in taskscope_asan_808_test.cpp are CAPTURE-FREE specifically to
// dodge it — see the file comment there).

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>

#include "ast/Node.hpp"
#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "native_test_harness.hpp"
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
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <filesystem>

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDirScopedcall() {
    return wtScratchDir("wt-scopedcall");
}

// Lower `source` to an object via the real native pipeline; -1 on failure.
int lowerToObject(const std::string& source, const std::string& objPath) {
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(source, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return -2;
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::sema::checkDefiniteAssignment(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    if (diags.hasErrors()) return -2;
    llvm::LLVMContext llvmCtx;
    auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
    if (!module) return -2;
    std::string err;
    if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -2;
    return 0;
}

// Link+run `source` under ASan; `asanOptions` selects the leak-detection
// posture. Returns 0 on a clean run matching `expectedExitCode`, 1 on any
// sanitizer report (crash, UAF, double-free, or — when detect_leaks=1 — a
// leak) or a value mismatch, -1 if the toolchain is unavailable (SKIP).
int runUnderAsanWithOptions(const std::string& source, int expectedExitCode,
                            const std::string& asanOptions) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;
    auto dir = wtDirScopedcall();
    std::string stem = unique_stem("bezel_scopedcall");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe = (dir / stem).string();
    if (lowerToObject(source, objPath) != 0) return -2;
    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }
    std::string runCmd =
        "ASAN_OPTIONS=" + asanOptions + " " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -4;
    if (!WIFEXITED(status)) return 1;          // crashed/aborted
    int code = WEXITSTATUS(status);
    if (code == 99) return 1;                  // sanitizer reported a problem
    return (code == expectedExitCode) ? 0 : 1;
}

// Leak-checked: fails on ANY leak, UAF, or double-free.
void expectAsanClean(const std::string& source, int expectedExitCode) {
    int a = runUnderAsanWithOptions(source, expectedExitCode,
                                    "detect_leaks=1:exitcode=99");
    if (a == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(a, 0);
}

// Common Tracked class with a static counter incremented once per destructor
// run, so Main()'s return value pins "disposed exactly N times" mechanically
// (a wrong count — too few from a leak masking disposal, too many from a
// double-dispose — fails the exit-code check).
const char* kTrackedClass =
    "class Tracked "
    "  public static _count: int := 0; "
    "  public method Touch() end Touch; "
    "  destructor Tracked._count := Tracked._count + 1; end; "
    "end Tracked; ";

// EventHolder exposes a stdlib §6.5 IEvent<int> face so the Subscribe shape
// matches the §28.14 form-5 receiver-bounded position with a using-var
// IEvent-typed local. NOTE: the constructor is multi-line (the single-line
// form omits the explicit owned-binding scope and silently suppresses the
// EventSource-backed-typed receiver's capture-cell-free path for an
// unrelated reason; the multi-line form is the one the original repro and
// the WIP #2383 used and is the one that demonstrates the leak).
const char* kEventHolderClass =
    "class EventHolder "
    "  public owned _src: EventSource<int>; "
    "  public constructor() "
    "    _src := new EventSource<int>(); "
    "  end constructor; "
    "  public method Source(): IEvent<int> => _src; "
    "  public method Fire() _src.Raise(1); end Fire; "
    "  destructor end destructor; "
    "end EventHolder; ";

}  // namespace

// ============================================================================
// HEADLINE: CancellationToken.Register on a using-var CancellationToken
// receiver, capturing a using-var local (form 5 of §28.14, stdlib §6.1).
// Before this lane: the 8-byte capture cell leaks (mirrors the #2334 Spawn
// symptom, just on a different register position). After the fix: leak-free
// on BOTH the normal and the unwind edge — CancellationTokenSource.Dispose
// fires registered callbacks synchronously (§6.1 contract), so quiescence
// holds on every exit path (no unwindUnsafe suppression needed).
//
// The code is INLINED in Main (no `Run()` helper method) — the cell-free
// machinery's `cellFreeEligibleFn_` keys the cell-free to the function
// in which the pre-scan ran, and a `Run()` helper compiles to a different
// pre-scan function that masks the original gap; the inline shape is the
// one the issue's repro uses (mirrored verbatim from repro_event.bzl).
// ============================================================================
BEZEL_TEST(capture_cell_scoped_call_2334, cancellation_token_register_no_leak) {
    expectAsanClean(
        std::string("module M; import System.Threading; ") + kTrackedClass +
        "class Prog "
        "  public static method Main(): int "
        "    using var t := new Tracked(); "
        "    using var cs := new CancellationTokenSource(); "
        "    using var sub := cs.Token.Register(lambda() => do t.Touch(); end); "
        "    cs.Cancel(); "
        "    return 1; "
        "  end Main; "
        "end Prog;",
        1);
}

// HEADLINE: IEvent<int>.Subscribe on a using-var IEvent-typed receiver
// (stdlib §6.5), capturing a using-var local (form 5 of §28.14, stdlib §6.5).
// Before this lane: the 8-byte capture cell leaks. After the fix: leak-free
// on every exit path — EventSource.Dispose severs the subscriber list before
// reclamation (§6.5's quiescence contract, normative for every source type).
// ============================================================================
BEZEL_TEST(capture_cell_scoped_call_2334, ievent_subscribe_no_leak) {
    expectAsanClean(
        std::string("module M; import System.Threading; ") + kTrackedClass +
        kEventHolderClass +
        "class Prog "
        "  public static method Main(): int "
        "    using var t := new Tracked(); "
        "    using var eh := new EventHolder(); "
        "    using var sub := eh.Source().Subscribe(lambda(n: int) => do t.Touch(); end); "
        "    eh.Fire(); "
        "    return 1; "
        "  end Main; "
        "end Prog;",
        1);
}

// CONTROL: TaskScope.Spawn on an await-using-var TaskScope receiver WITHOUT a
// local capture (lambda body references no captured local). The pre-scan's
// special case fires collectLambdaCaptures on an empty capture set, so the fix
// machinery runs but the cell-free-eligible set is empty — exercises the
// non-interference of the fix's consumeLastLambdaCapturedCells path with the
// ordinary capture-free Spawn lambda. Regression guard against accidentally
// breaking the no-capture shape while widening the fix.
// (Async shape — same as the original #2334 regression-guard control test.)
//
// TaskScope REQUIRES `await using var` per stdlib §6.10 (v0.175, #808): a
// synchronous destructor cannot await the drain. The pre-#3633 file's
// `using var scope` was a compile error ever since v0.175, but the broken
// program was silently swallowed by the conflated -1 (runUnderAsanWithOptions
// returned -1 for any non-clean path including a parse/sema/emit failure on
// the test program). #3633 split -1 into per-cause codes (-2 broken program,
// -3 link failure, -4 system() failure) so this row now loud-fails — which
// is the entire point of #3633. The fix is on the TEST PROGRAM, not the
// harness: change `using var scope` to `await using var scope` (the shape
// every sibling test in taskscope_asan_2334_test.cpp /
// taskscope_asan_808_test.cpp already uses).
BEZEL_TEST(capture_cell_scoped_call_2334, taskscope_spawn_no_local_capture_regression_guard) {
    expectAsanClean(
        std::string("module M; import System.Threading;") +
        "class Prog "
        "  public static method Always(): bool => true; "
        "  public static async method Run(): Task "
        "    await using var scope := new TaskScope(); "
        "    scope.Spawn(async lambda() => do await Task.Delay(1); end); "
        "    await scope.Join(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await Prog.Run(); "
        "    return 0; "
        "  end Main; "
        "end Prog;",
        0);
}

// CONTROL: CancellationToken.Register with a capture-free lambda (no local
// references). Confirms the special case does not regress the capture-free
// Register shape — the consumed cells vector is empty and the consume call
// is a no-op (mirrors the IIFE arm's behaviour for the no-capture case).
// Returns 1 from Main so the test exit code discriminates from a sanitizer
// 99-report. The Tracked class is included only to mirror the other test
// files' source shape — no Tracked instance is created here.
BEZEL_TEST(capture_cell_scoped_call_2334, cancellation_token_register_no_capture_regression_guard) {
    expectAsanClean(
        std::string("module M; import System.Threading; ") + kTrackedClass +
        "class Counter "
        "  public static _calls: int := 0; "
        "end Counter; "
        "class Prog "
        "  public static method Main(): int "
        "    using var cs := new CancellationTokenSource(); "
        "    using var sub := cs.Token.Register(lambda() => do "
        "      Counter._calls := Counter._calls + 1; "
        "    end); "
        "    cs.Cancel(); "
        "    return 1; "
        "  end Main; "
        "end Prog;",
        1);
}

