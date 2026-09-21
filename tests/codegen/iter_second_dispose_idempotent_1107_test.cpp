// Codegen — second explicit Dispose() on an iterator IEnumerator<T> must be a
// no-op, not a SIGSEGV (§28.2 / §29.7 v0.183).
// Issue #1107 (adversarial sweep #9): calling `e.Dispose()` twice on the same
// IEnumerator<T> local crashed with SIGSEGV. The first call freed the frame and
// nulled the local slot; the second call reloaded null and dereferenced it to
// load the vtable → null-pointer dereference. Fix: null-guard the explicit
// enumerator Dispose dispatch on a non-null receiver (mirrors the scope-end and
// null-conditional paths), and add an idempotency guard at the top of the
// iterator Dispose function body (defense-in-depth).
//
// REWORK (per-gate review, HIGH-severity regression): the original idempotency
// guard was predicated on `state == -1`, but that value is OVERLOADED — it is
// set BOTH by Dispose (the "already disposed" case the guard targets) AND by
// MoveNext on NATURAL EXHAUSTION (the body fall-through), where the frame is
// still allocated and STILL OWES DISPOSAL. On the dominant exhaust-then-Dispose
// pattern, `state` was already `-1` when the first (legitimate) Dispose ran, so
// the guard returned BEFORE free(self) / disposeOwnedParams() / emitFreeSpillArea
// — a frame + owned-param LEAK (§29.7/§29.8). The rework re-predicates the guard
// on a Dispose-only sentinel (`state == -2`) that MoveNext never produces: the
// first Dispose (after MoveNext's exhaustion `-1`, a suspended yield `N`, or
// never-iterated `0`) proceeds and frees the frame; a SECOND Dispose (after the
// first set the `-2` sentinel) no-ops. §28.2 models idempotency as a "per-
// instance disposed flag"; `-2` is that flag, encoded in the existing i32 state
// field (readable in checked mode where the frame is quarantined).
//
// Spec authority:
//   §28.2 v0.183 — "Dispose() on any class instance is idempotent: the first
//   call performs the disposal; subsequent calls are no-ops."
//   §29.7 — "second Dispose() is a no-op."
//   §29.8 — "When the iterator is disposed, the state machine disposes any
//   owned parameter that has not been moved out of by the body."
//
// Tests:
//   (1) second Dispose on a bare IEnumerator<int> — no crash, rc=0 (the bug).
//   (2) second Dispose after one MoveNext with try/finally — finally runs
//       exactly once, rc=1 (the idempotency guard prevents a double-run).
//   (3) normal single-dispose iteration still works — regression check, rc=3.
//   (4) second Dispose on an IEnumerable<int>-sourced enumerator — no crash.
//   (5) LEAK-CHECKED: exhaust-then-Dispose frees the frame (the regressed path)
//       — runUnderLeakSanitizer; the original guard leaked the calloc'd frame.
//   (6) LEAK-CHECKED: exhaust-then-Dispose disposes an owned iterator param
//       (§29.8) — runUnderLeakSanitizer; the original guard leaked the frame
//       AND the undisposed owned parameter.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir1107() {
    return wtScratchDir("wt-1107");
}

// Compile `source` to a temp native executable via the real Driver and run it.
// Returns the process exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir1107();
    std::string stem = unique_stem("bezel_1107");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    {
        std::ofstream out(src);
        out << source;
    }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}

// Compile `source` through the FULL pipeline (lex/parse/sema/OWNERSHIP/codegen)
// to an AOT object, link it against the ASan-instrumented Bezel runtime, and run
// under the LeakSanitizer. Returns 1 if a leak/UAF was detected (the program
// exited 99), 0 if clean, or -1 if the toolchain/build is unavailable (SKIP).
// Unlike compileAndRun (exit-code only), this OBSERVES leaks — the regressed
// exhaust-then-Dispose path leaked the frame without changing the exit code.
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1107_lsan_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe = (dir / stem).string();
    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return bezel::test::asanBuildFailure("parse");
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return bezel::test::asanBuildFailure("sema");
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return bezel::test::asanBuildFailure("emitModule");
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err))
            return bezel::test::asanBuildFailure("emitObjectFile");
    }
    auto cleanup = [&] { std::remove(objPath.c_str()); std::remove(exe.c_str()); };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        cleanup();
        return bezel::test::asanLinkFailure("link");
    }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // leak/UAF
    return 0;
}

}  // namespace

// ===========================================================================
// (1) §28.2 / §29.7: second explicit Dispose() on IEnumerator<int> must be a
//     no-op. Previously this SIGSEGV'd because the first Dispose freed the
//     frame and nulled the slot; the second reloaded null and dereferenced it.
//     Expected exit code: 0 (no crash).
// ===========================================================================
BEZEL_TEST(iter_second_dispose_1107, second_dispose_is_noop) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class Test
  public static method Gen(): IEnumerator<int>
    yield return 1;
  end Gen;
  public static method Main(): int
    var e := Gen();
    e.Dispose();
    e.Dispose();
    return 0;
  end Main;
end Test;
)"), 0);
}

// ===========================================================================
// (2) §28.2 / §29.7: second Dispose after one MoveNext with try/finally —
//     the finally must run exactly once. The first Dispose runs the finally
//     (counter becomes 1) and marks state=-1; the second Dispose is a no-op
//     (does not re-run the finally). Without the idempotency guard the finally
//     could double-run.
//     Expected exit code: 1 (counter == 1).
// ===========================================================================
BEZEL_TEST(iter_second_dispose_1107, second_dispose_finally_once) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static counter: int := 0;
  public static method Gen(): IEnumerator<int>
    try
      yield return 1;
      yield return 2;
    finally
      C.counter := C.counter + 1;
    end;
  end Gen;
end C;
class Main
  public static method Main(): int
    var e := C.Gen();
    if e.MoveNext() then
      _ := e.Current;
    end;
    e.Dispose();
    e.Dispose();
    return C.counter;
  end Main;
end Main;
)"), 1);
}

// ===========================================================================
// (3) Regression: normal single-dispose iteration still works after the fix.
//     A simple IEnumerator<int> iterated via MoveNext/Current, then disposed
//     once. The sum must be correct (1+2=3).
//     Expected exit code: 3.
// ===========================================================================
BEZEL_TEST(iter_second_dispose_1107, single_dispose_normal_iteration) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class Test
  public static method Gen(): IEnumerator<int>
    yield return 1;
    yield return 2;
  end Gen;
  public static method Main(): int
    var e := Gen();
    var sum := 0;
    while e.MoveNext() do
      sum := sum + e.Current;
    end;
    e.Dispose();
    return sum;
  end Main;
end Test;
)"), 3);
}

// ===========================================================================
// (4) §28.2 / §29.7: second Dispose on an IEnumerator<int> obtained via a
//     method-local. The enumerator is created directly (no IEnumerable wrapper);
//     disposing it twice must be a no-op. Tests the null-guard path for a
//     non-trivial receiver expression (method call result stored in a local).
//     Expected exit code: 0.
// ===========================================================================
BEZEL_TEST(iter_second_dispose_1107, second_dispose_method_result) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class Test
  public static method Make(): IEnumerator<int>
    yield return 7;
  end Make;
  public static method Main(): int
    var e := Make();
    e.Dispose();
    e.Dispose();
    return 0;
  end Main;
end Test;
)"), 0);
}

// ===========================================================================
// (5) §29.7 LEAK-CHECKED regression test (the path the original #1107 guard
//     regressed): iterate an IEnumerator<int> to EXHAUSTION via MoveNext, then
//     Dispose. MoveNext sets `state = -1` on natural exhaustion; the original
//     `state == -1` idempotency guard then treated the FIRST (legitimate)
//     Dispose as already-disposed and returned BEFORE free(self) — the calloc'd
//     state-machine frame LEAKED. The rework's Dispose-only sentinel (`-2`) lets
//     this first Dispose proceed and free the frame. Verified via the
//     LeakSanitizer (full pipeline + AOT), NOT exit-code-only: a leaked frame
//     does not change the exit code, so compileAndRun could not catch it. This
//     test FAILS (leak) under the original guard and PASSES (no leak) under the
//     rework.
// ===========================================================================
BEZEL_TEST(iter_second_dispose_1107, exhaust_then_dispose_no_frame_leak) {
    int lsan = runUnderLeakSanitizer(R"(
module M;
class Test
  public static method Gen(): IEnumerator<int>
    yield return 1;
    yield return 2;
    yield return 3;
  end Gen;
  public static method Main(): int
    var e := Gen();
    while e.MoveNext() do
      _ := e.Current;
    end;
    e.Dispose();
    return 0;
  end Main;
end Test;
)");
    if (lsan == -1) return;                 // toolchain unavailable -> SKIP
    BEZEL_EXPECT_EQ(lsan, 0);               // frame freed — no leak
}

// ===========================================================================
// (6) §29.8 LEAK-CHECKED: an iterator with an OWNED parameter, iterated to
//     exhaustion then Dispose'd. The state machine takes ownership of the param
//     at construction and must auto-dispose it (§29.8) when the iterator is
//     disposed — AFTER the body's cleanups and BEFORE the frame is freed. The
//     original `state == -1` guard short-circuited the first post-exhaustion
//     Dispose before disposeOwnedParams() ran, leaking BOTH the frame AND the
//     undisposed owned parameter (R). The rework sentinel lets Dispose proceed.
//     LSan catches a leak of either. FAILS (leak) under the original guard.
// ===========================================================================
BEZEL_TEST(iter_second_dispose_1107, exhaust_then_dispose_owned_param_no_leak) {
    int lsan = runUnderLeakSanitizer(R"(
module M;
class R
  public static _n: int := 0;
  public constructor() end constructor;
  destructor R._n := R._n + 1; end destructor;
end R;
class Test
  public static method Gen(owned r: R): IEnumerator<int>
    yield return 1;
    yield return 2;
  end Gen;
  public static method Main(): int
    var e := Gen(new R());
    while e.MoveNext() do
      _ := e.Current;
    end;
    e.Dispose();
    return R._n;
  end Main;
end Test;
)");
    if (lsan == -1) return;                 // toolchain unavailable -> SKIP
    BEZEL_EXPECT_EQ(lsan, 0);               // frame freed + owned param disposed
}
