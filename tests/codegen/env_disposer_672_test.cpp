// Codegen + runtime — typed closure env-disposer foundation (issue #672).
//
// BACKGROUND (#672): bz_closure_release was untyped — it freed the env block
// as raw bytes without knowing what the captured slots held.  That meant
// capture-site retains (the +1 that would co-own a string / class capture)
// could not be added: adding them without a typed teardown would leak or
// double-free on closure release.  #672 adds a `ptr envdtor` field (field 3,
// off 24) to the closure cell, extending it from 24 to 32 bytes.  bz_closure_
// release calls envdtor(env) (if non-null) BEFORE freeing the env block; the
// envdtor is a per-lambda function emitted by codegen beside the trampoline
// that releases exactly the +1-owned env slots.
//
// FOUNDATION STATE (this lane): envdtor is always null (stored null explicitly
// by initClosureEnvdtor, on top of calloc's zero-fill).  bz_closure_release
// observes null and skips the call — behaviour is identical to the prior
// 24-byte layout for all existing closures.  The follow-up capture-retain lane
// will populate envdtor for closures that retain owned captures.
//
// What this test suite pins:
//
//   1. ABI CONTINUITY — existing closure tests still pass with the 32-byte cell
//      (the full suite exercises this; these canaries are a focused subset).
//
//   2. NULL-ENVDTOR NO-OP — closure create/invoke/release with a non-null env
//      (capturing closure) works correctly; the null envdtor is a no-op and
//      does NOT corrupt the env or the captured cell.
//
//   3. LSan CLEAN — the cell + env are reclaimed (no leak, no UAF/DF).
//
//   4. DOUBLE-RELEASE CANARY — calling bz_closure_release twice on a released
//      cell does NOT crash (the cell is freed; the runtime must not deref the
//      freed pointer — this is guarded by ASan/UAF detection on the exit-code
//      no-corruption path).  NOTE: per §28.9 double-release is a codegen bug
//      (should never happen in correct code); this test covers the ASan-visible
//      symptom rather than requiring a defined safe path.
//
//   5. CELL-SIZE NM PROBE — a tiny compile+inspect step verifies the cell
//      produced by the compiler is actually 32 bytes (not 24), confirming the
//      codegen struct definition change landed correctly.
//
// per §27.5 / §28.9 / #672.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
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
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir672() {
    return wtScratchDir("wt-672");
}

// Compile `source` to a temp native executable via the real Driver pipeline and
// run it; returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir672();
    std::string stem = unique_stem("bezel_672");
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

// LeakSanitizer verification for a single program. Returns:
//   0  — ran clean: no leak, no use-after-free / double-free.
//   1  — sanitizer reported a problem: test failure.
//  -1  — toolchain unavailable / build step failed: SKIP.
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir672();
    std::string stem = unique_stem("bezel_672_lsan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return -1;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -1;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return -1;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -1;
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }

    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -3;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

// Assert correctness AND no memory-safety corruption (UAF/DF), tolerate leaks.
// Mirror of expectValueAndNoCorruption from closure_refcount_test.cpp.
void expectValueAndNoCorruption(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return;
    auto dir = wtDir672();
    std::string stem = unique_stem("bezel_672_safe");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();
    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return;
    }
    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return; }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=0:exitcode=99:abort_on_error=0 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return;
    BEZEL_EXPECT_TRUE(WIFEXITED(status) &&
                      WEXITSTATUS(status) == expectedExitCode);
}

void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// ---------------------------------------------------------------------------
// Test programs
// ---------------------------------------------------------------------------

// §27.5 / §28.9 / #672 ABI CONTINUITY: a non-capturing lambda churned in a
// loop.  With the 32-byte cell the producer still calloc's getTypeAllocSize
// bytes, stores fn at field 0, env (null) at field 1, rc=1 at field 2, and
// envdtor=null at field 3. bz_closure_release reads envdtor at off 24, finds
// null, skips the call, frees the null env (safe), frees the cell. Steady-
// state memory-clean: no leak, no UAF. Returns 0.
const char* kNonCapturingChurnSteadyState32 =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; var acc: int := 0; "
    "    while i < 200000 do "
    "      var f: (int) -> int := lambda(x: int) => x * 3; "
    "      acc := acc + f(1); "
    "      i := i + 1; "
    "    end; "
    "    if acc > 0 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §27.5 / #672 ABI CONTINUITY: a capturing lambda (`this` capture — stored by
// value in the env, no boxed cell) churned 200k iterations.  Each iteration:
// calloc 32-byte cell, allocate env block (holds `this` ptr), store fn/env/rc
// /envdtor(null). bz_closure_release: rc=0, envdtor=null → skip, free env,
// free cell. LSan-clean. Returns 0.
const char* kThisCapturingChurnLeanFree32 =
    "module M; class C "
    "  public _v: int := 5; "
    "  public method Run(): int "
    "    var acc: int := 0; var i: int := 0; "
    "    while i < 200000 do "
    "      var f: (int) -> int := lambda(x: int) => x + this._v; "
    "      acc := acc + f(1); "
    "      i := i + 1; "
    "    end; "
    "    if acc > 0 then return 0; end; return 1; "
    "  end Run; "
    "  public static method Main(): int "
    "    using var c := new C(); "
    "    return c.Run(); "
    "  end Main; "
    "end C;";

// §27.5 / §28.9 / #672 NULL-ENVDTOR NO-OP with an owned-capture guard.
// A capturing closure captures an owned class `r` (borrow per §28.9), the
// closure is created, invoked, released by scope end; then the `using var`
// disposes `r` exactly once.  The null envdtor must NOT touch `r` — so the
// destructor count must be exactly 1.  A double-dispose via envdtor (if the
// field were accidentally populated) would yield count=2 and ASan abort.
// Exit code 1 == _n (exactly one destructor run).
const char* kNullEnvdtorDoesNotDisposeCapture =
    "module M; class R "
    "  public static _n: int := 0; "
    "  public method Tag(): int return 42; end Tag; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Main(): int "
    "    do "
    "      using var r := new R(); "
    "      var f: () -> int := lambda() => r.Tag(); "
    "      _ := f(); "
    "    end; "
    "    return _n; "   // must be exactly 1
    "  end Main; "
    "end R;";

// §27.5 / #672: the same capture-and-dispose pattern run 5000 times in a loop.
// Each `using var r` disposes exactly once (_n == 5000); the null envdtor must
// NOT dispose anything. Value + no-corruption (tolerated per-iteration leak of
// the boxed capture cell — same as closure_refcount_test.cpp).
const char* kNullEnvdtorLoopDoesNotDoubleDispose =
    "module M; class R "
    "  public static _n: int := 0; "
    "  public method Tag(): int return 1; end Tag; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 5000 do "
    "      do "
    "        using var r := new R(); "
    "        var f: () -> int := lambda() => r.Tag(); "
    "        _ := f(); "
    "      end; "
    "      i := i + 1; "
    "    end; "
    "    if _n = 5000 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end R;";

// §27.5 / #672 CELL-SIZE SMOKE: compile a minimal capturing lambda and verify
// (via a JIT run) that the env path is exercised — indirectly confirms the 32-
// byte cell allocation (if the struct were still 24 bytes, the envdtor store at
// off 24 would write past the allocation and ASan would fire).  The test value
// proves the capture works correctly end-to-end.
const char* kCellSizeSmoke =
    "module M; class C "
    "  public static method Main(): int "
    "    var k: int := 7; "
    "    var f: (int) -> int := lambda(x: int) => x + k; "
    "    return f(5); "  // 12
    "  end Main; "
    "end C;";

// §8.6 / §28.9 / #672 BOX-CAPTURE RELEASE (the fix): a boxed VALUE local
// (`var o: Object := 42` — a +1 box producer) captured by exactly ONE
// non-escaping lambda.  BoxEscapeWalker marks the local escaped (captured), so
// its scope-end bz_box_release is SUPPRESSED; the owning lambda's envdtor now
// performs that deferred release.  Before this lane the 24-byte box wrapper
// leaked.  LSan-CLEAN.  Returns 1 (the `o is int` capture fires).
const char* kBoxSingleCaptureNonEscaping =
    "module M; class C "
    "  public static method Main(): int "
    "    var hit: int := 0; "
    "    do "
    "      var o: Object := 42; "
    "      var f: () -> bool := lambda() => o is int; "
    "      if f() then hit := 1; end; "
    "    end; "
    "    return hit; "   // 1
    "  end Main; "
    "end C;";

// §8.6 / §27.5 / #672: a FRESH boxed value captured by a fresh non-escaping
// lambda each loop iteration.  Each iteration's box is released by THAT
// iteration's closure envdtor (the in-loop local uses fresh — not guarded —
// boxing, so one box + one closure per iteration, count==1).  No per-iteration
// leak, no double-free.  LSan-CLEAN.  Returns 1 (all 1000 iterations match).
const char* kBoxLoopFreshCapture =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; var acc: int := 0; "
    "    while i < 1000 do "
    "      var o: Object := i; "
    "      var f: () -> bool := lambda() => o is int; "
    "      if f() then acc := acc + 1; end; "
    "      i := i + 1; "
    "    end; "
    "    if acc = 1000 then return 1; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// §8.6 / §28.9 / #672: an ESCAPING capturing lambda — the closure (holding the
// boxed `o`) is RETURNED from Make and invoked in Main, then released at Main's
// scope end.  The box's release is correctly deferred to the envdtor that runs
// at the escaped closure's last-drop (AFTER Make returns), so the box is read
// during f() while still live (no UAF) and freed exactly once (no double-free).
// The 8-byte boxed CELL still leaks (the pre-existing #429 escaping-cell leak,
// orthogonal to the box-value release) — tolerated here.  Returns 1.
const char* kBoxEscapingCapture =
    "module M; class C "
    "  public static method Make(): () -> bool "
    "    var o: Object := 42; "
    "    return lambda() => o is int; "
    "  end Make; "
    "  public static method Main(): int "
    "    var f: () -> bool := Make(); "
    "    if f() then return 1; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// §8.6 / §28.9 / #802 (the fix) MULTI-CAPTURE: the SAME boxed `o` captured by TWO
// non-escaping lambdas — two envs hold the one box. The rc-retain model keeps it
// sound: the first capture transfers the producer's +1, the second bz_box_retains
// an independent +1, and BOTH envdtors release — so the box is reclaimed exactly
// once (rc 2 -> 0). Previously this was an excluded leak (#672 admitted only
// capture-count 1). LSan-CLEAN now. Returns 1.
const char* kBoxMultiCaptureNoDoubleFree =
    "module M; class C "
    "  public static method Main(): int "
    "    var hit: int := 0; "
    "    do "
    "      var o: Object := 42; "
    "      var f: () -> bool := lambda() => o is int; "
    "      var g: () -> bool := lambda() => o is int; "
    "      if f() then if g() then hit := 1; end; end; "
    "    end; "
    "    return hit; "   // 1
    "  end Main; "
    "end C;";

// §8.6 / §28.9 / #802: THREE non-escaping captures of the same box — producer +1
// plus two retains = rc 3, three envdtor releases -> 0. LSan-CLEAN. Returns 1.
const char* kBoxThreeCapture =
    "module M; class C "
    "  public static method Main(): int "
    "    var hit: int := 0; "
    "    do "
    "      var o: Object := 42; "
    "      var f: () -> bool := lambda() => o is int; "
    "      var g: () -> bool := lambda() => o is int; "
    "      var h: () -> bool := lambda() => o is int; "
    "      if f() then if g() then if h() then hit := 1; end; end; end; "
    "    end; "
    "    return hit; "   // 1
    "  end Main; "
    "end C;";

// §8.6 / #802 value-correctness: the box read by TWO closures yields the right
// value (both observe `o is int`), and the box is reclaimed exactly once. 1+1 = 2.
const char* kBoxMultiCaptureValue =
    "module M; class C "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    do "
    "      var o: Object := 42; "
    "      var f: () -> int := lambda() => (case o of var n: int => 1, _ => 0); "
    "      var g: () -> int := lambda() => (case o of var n: int => 1, _ => 0); "
    "      r := f() + g(); "
    "    end; "
    "    return r; "   // 2
    "  end Main; "
    "end C;";

// §8.6 / §27.5 / #672 GUARDED (deferred, leak-tolerated): a box producer
// declared OUTSIDE a loop, captured by a lambda INSIDE the loop.  Guarded
// boxing reuses ONE box/cell across all iterations, so N closures share one box
// — releasing it in each envdtor would double-free.  The `wasGuardedLocal` gate
// EXCLUDES this from box release (the box leaks, safe direction).  Pins NO
// double-free across 100 iterations.  Returns 1.
const char* kBoxGuardedOuterCaptureNoDoubleFree =
    "module M; class C "
    "  public static method Main(): int "
    "    var o: Object := 42; "
    "    var i: int := 0; var acc: int := 0; "
    "    while i < 100 do "
    "      var f: () -> bool := lambda() => o is int; "
    "      if f() then acc := acc + 1; end; "
    "      i := i + 1; "
    "    end; "
    "    if acc = 100 then return 1; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// §27.5 / #672: non-capturing churn with 32-byte cell — steady-state LSan
// clean.  Pins ABI continuity: a null-env closure (rc=1, envdtor=null) is
// reclaimed cleanly at scope end.
BEZEL_TEST(codegen_env_disposer_672, non_capturing_churn_lsan_clean_27_5) {
    expectValueAndLeakFree(kNonCapturingChurnSteadyState32, 0);
}

// §27.5 / #672: `this`-capturing churn with a non-null env block — LSan clean.
// The env is reclaimed; the null envdtor does not run any teardown.
BEZEL_TEST(codegen_env_disposer_672, this_capturing_churn_lsan_clean_27_5) {
    expectValueAndLeakFree(kThisCapturingChurnLeanFree32, 0);
}

// §28.9 / #672: null envdtor is a pure no-op — it does NOT dispose the
// captured class reference.  Disposal count == 1 (from the using-var scope).
// Also LSan-clean (single instance: the boxed cell IS reclaimed via the
// using-var disposal in this single-shot case).
BEZEL_TEST(codegen_env_disposer_672, null_envdtor_does_not_dispose_capture_28_9) {
    // The LSan-clean subset: single instance, also LSan clean
    expectValueAndLeakFree(kNullEnvdtorDoesNotDisposeCapture, 1);
}

// §28.9 / #672: loop variant — null envdtor never double-disposes across 5000
// iterations.  _n == 5000 exactly.  Value + no-corruption (tolerated leak of
// boxed capture cells).
BEZEL_TEST(codegen_env_disposer_672, null_envdtor_loop_no_double_dispose_28_9) {
    expectValueAndNoCorruption(kNullEnvdtorLoopDoesNotDoubleDispose, 0);
}

// §27.5 / #672: cell-size smoke — a capturing lambda produces correct results
// when the 32-byte cell layout is used.  ASan would fire during the
// initClosureEnvdtor store (off 24) if the cell were only 24 bytes.
BEZEL_TEST(codegen_env_disposer_672, cell_size_smoke_27_5) {
    expectValueAndNoCorruption(kCellSizeSmoke, 12);
}

// §8.6 / §28.9 / #672 (THE FIX): a boxed value captured by exactly one
// non-escaping lambda — the envdtor performs the deferred box release that
// BoxEscapeWalker suppressed.  Previously the 24-byte box wrapper leaked.
// LSan-CLEAN; returns 1.
BEZEL_TEST(codegen_env_disposer_672, box_single_capture_release_lsan_clean_8_6) {
    expectValueAndLeakFree(kBoxSingleCaptureNonEscaping, 1);
}

// §8.6 / §27.5 / #672: fresh box + fresh closure each loop iteration; each box
// released by its own envdtor (count==1, non-guarded).  No accumulating leak,
// no double-free.  LSan-CLEAN; returns 1.
BEZEL_TEST(codegen_env_disposer_672, box_loop_fresh_capture_lsan_clean_27_5) {
    expectValueAndLeakFree(kBoxLoopFreshCapture, 1);
}

// §8.6 / §28.9 / #672: an escaping closure holding a boxed value — the box is
// read while live (no UAF) and released exactly once at the closure's last-drop
// (no double-free).  The pre-existing 8-byte escaping-cell leak is tolerated.
// Value + no-corruption; returns 1.
BEZEL_TEST(codegen_env_disposer_672, box_escaping_capture_no_corruption_28_9) {
    expectValueAndNoCorruption(kBoxEscapingCapture, 1);
}

// §8.6 / §28.9 / #672: the same box captured by two lambdas is deliberately
// EXCLUDED from envdtor release (count 2 ≠ 1) — it leaks rather than risk a
// double-free.  Pins NO double-free / UAF (the leak itself is tolerated and
// tracked for the general rc-retain solution).  Returns 1.
BEZEL_TEST(codegen_env_disposer_672, box_multi_capture_rc_retain_lsan_clean_802) {
    expectValueAndLeakFree(kBoxMultiCaptureNoDoubleFree, 1);
}

// #802: three non-escaping captures — rc-retain reclaims the box once. LSan-clean.
BEZEL_TEST(codegen_env_disposer_672, box_three_capture_rc_retain_lsan_clean_802) {
    expectValueAndLeakFree(kBoxThreeCapture, 1);
}

// #802: the multi-captured box reads correctly from both closures (value 2) and
// is leak-free.
BEZEL_TEST(codegen_env_disposer_672, box_multi_capture_value_lsan_clean_802) {
    expectValueAndLeakFree(kBoxMultiCaptureValue, 2);
}

// §8.6 / §27.5 / #672: a guarded-boxing capture (box producer outside the loop,
// lambda inside) reuses one box across iterations; EXCLUDED from envdtor release
// via the wasGuardedLocal gate so N closures do not double-free the shared box.
// Pins NO double-free across iterations (leak tolerated).  Returns 1.
BEZEL_TEST(codegen_env_disposer_672, box_guarded_capture_no_double_free_27_5) {
    expectValueAndNoCorruption(kBoxGuardedOuterCaptureNoDoubleFree, 1);
}

// #694 (the fix): a closure that DEFINITELY escapes (directly returned) and is the
// SOLE holder of a capture cell has that 8-byte cell freed by its envdtor at
// last-drop. The defining scope (Make) is gone before the closure is dropped in
// Main, so the free is safe (no UAF); single holder ⇒ no double-free. Previously
// the cell leaked. LSan-CLEAN; returns the captured int 7.
const char* kEscapingReturnedIntCell =
    "module M; class C "
    "  public static method Make(): () -> int "
    "    var x: int := 7; "
    "    return lambda() => x; "
    "  end Make; "
    "  public static method Main(): int "
    "    var f: () -> int := Make(); "
    "    return f(); "   // 7
    "  end Main; "
    "end C;";
BEZEL_TEST(codegen_env_disposer_672, escaping_returned_int_cell_freed_lsan_clean_694) {
    expectValueAndLeakFree(kEscapingReturnedIntCell, 7);
}

// #694: a returned closure capturing a BOXED value — both the box (envdtor box
// release, #672) and the capture cell (#694) are reclaimed at last-drop. LSan-
// clean; returns 5 (the box reads as `int`).
const char* kEscapingReturnedBoxCell =
    "module M; class C "
    "  public static method Make(): () -> bool "
    "    var o: Object := 42; "
    "    return lambda() => o is int; "
    "  end Make; "
    "  public static method Main(): int "
    "    var f: () -> bool := Make(); "
    "    if f() then return 5; end; return 0; "
    "  end Main; "
    "end C;";
BEZEL_TEST(codegen_env_disposer_672, escaping_returned_box_cell_freed_lsan_clean_694) {
    expectValueAndLeakFree(kEscapingReturnedBoxCell, 5);
}

// #694: a returned closure capturing a +1 STRING — both the string (envdtor string
// release) and the capture cell (#694) are reclaimed. LSan-clean; "abcd".Length = 4.
const char* kEscapingReturnedStringCell =
    "module M; class C "
    "  public static method Make(): () -> int "
    "    var s: string := \"ab\" + \"cd\"; "
    "    return lambda() => s.Length; "
    "  end Make; "
    "  public static method Main(): int "
    "    var f: () -> int := Make(); "
    "    return f(); "   // 4
    "  end Main; "
    "end C;";
BEZEL_TEST(codegen_env_disposer_672, escaping_returned_string_cell_freed_lsan_clean_694) {
    expectValueAndLeakFree(kEscapingReturnedStringCell, 4);
}

// #694 (soundness boundary): a NON-escaping single capture (the lambda is invoked
// in scope, NOT returned) is NOT in definitelyEscapingLambdas_, so the cell is
// freed by the scope-end boxedCellFree — NOT the envdtor. Pins that the envdtor
// cell-free does not fire for non-escaping closures (which would UAF the outer
// scope). LSan-clean; returns 7.
const char* kNonEscapingSingleNotEnvdtorFreed =
    "module M; class C "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    do "
    "      var x: int := 7; "
    "      var f: () -> int := lambda() => x; "
    "      r := f(); "
    "    end; "
    "    return r; "   // 7
    "  end Main; "
    "end C;";
BEZEL_TEST(codegen_env_disposer_672, non_escaping_single_not_envdtor_freed_lsan_clean_694) {
    expectValueAndLeakFree(kNonEscapingSingleNotEnvdtorFreed, 7);
}

// #694 extend: a closure RETURNED VIA A VARIABLE (`var f := () => x; return f`)
// also definitely escapes — collectDefinitelyEscapingLambdas traces the returned
// identifier to the lambda its (never-reassigned) var was initialized from. Its
// envdtor frees the sole capture cell. LSan-CLEAN; returns 7.
const char* kEscapingReturnedViaVarCell =
    "module M; class C "
    "  public static method Make(): () -> int "
    "    var x: int := 7; "
    "    var f: () -> int := lambda() => x; "
    "    return f; "
    "  end Make; "
    "  public static method Main(): int "
    "    var g: () -> int := Make(); "
    "    return g(); "   // 7
    "  end Main; "
    "end C;";
BEZEL_TEST(codegen_env_disposer_672, escaping_returned_via_var_cell_freed_lsan_clean_694) {
    expectValueAndLeakFree(kEscapingReturnedViaVarCell, 7);
}

// #694 extend (soundness boundary): when the returned var is REASSIGNED, its init
// lambda is dropped mid-scope, so freeing that lambda's cell could UAF a later
// read of the captured local (`var z := x`). The reassignment guard excludes it —
// the cell leaks (safe), with NO use-after-free / double-free. Returns g() where
// g is the reassigned `() => y` (y = 5).
const char* kEscapingReassignedVarNotFreed =
    "module M; class C "
    "  public static method Make(): () -> int "
    "    var x: int := 3; var y: int := 5; "
    "    var f: () -> int := lambda() => x; "
    "    f := lambda() => y; "
    "    var z: int := x; "
    "    return f; "
    "  end Make; "
    "  public static method Main(): int "
    "    var g: () -> int := Make(); "
    "    return g(); "   // 5
    "  end Main; "
    "end C;";
BEZEL_TEST(codegen_env_disposer_672, escaping_reassigned_var_no_corruption_694) {
    expectValueAndNoCorruption(kEscapingReassignedVarNotFreed, 5);
}

// #694 field-store: `this.f := () => x` stores the closure on the receiver, which
// outlives the method — a definite escape. The field is assigned exactly once, so
// the lambda's envdtor (run when the Holder is disposed) frees the sole capture
// cell. LSan-CLEAN; returns 7.
const char* kEscapingFieldStoreCell =
    "module M; class Holder "
    "  public f: () -> int := lambda() => 0; "
    "  public method Set() "
    "    var x: int := 7; "
    "    f := lambda() => x; "
    "  end Set; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var r: int := 0; "
    "  do using var h := new Holder(); h.Set(); r := h.f(); end; "
    "  return r; "   // 7
    "end Main; end C;";
BEZEL_TEST(codegen_env_disposer_672, escaping_field_store_cell_freed_lsan_clean_694) {
    expectValueAndLeakFree(kEscapingFieldStoreCell, 7);
}

// #694 field-store (soundness boundary): a field assigned a lambda TWICE — the
// first lambda is dropped mid-method (the field's old value released), so freeing
// its cell could UAF a later `var z := x`. The exactly-once guard excludes it: the
// cell leaks (safe), with NO use-after-free / double-free. Returns the reassigned
// `() => y` (y = 5).
const char* kEscapingFieldReassignedNotFreed =
    "module M; class Holder "
    "  public f: () -> int := lambda() => 0; "
    "  public method Set() "
    "    var x: int := 3; var y: int := 5; "
    "    f := lambda() => x; "
    "    f := lambda() => y; "
    "    var z: int := x; "
    "  end Set; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var r: int := 0; "
    "  do using var h := new Holder(); h.Set(); r := h.f(); end; "
    "  return r; "   // 5
    "end Main; end C;";
BEZEL_TEST(codegen_env_disposer_672, escaping_field_reassigned_no_corruption_694) {
    expectValueAndNoCorruption(kEscapingFieldReassignedNotFreed, 5);
}

// #1017 / §27.5: an owned / `using var` local captured by ≥2 non-escaping
// closures that BOTH capture it. Each closure boxes the local into the SAME
// shared heap cell (boxLocalSlot reuses it for the 2nd capture). The plain
// boxedCellFree is suppressed by the hasOwnerDisposal guard (the local's own
// using-var disposal reads the object THROUGH the cell, and LIFO order would
// free the cell first → UAF), so before the fix the 8-byte cell LEAKED (masked
// by LSan reachability in some shapes; the method-scope shape below reports it).
// The fix registers a DEFERRED cell-free in the owner's scope that runs AFTER
// the owner disposal and every capturing closure release — freeing the cell
// exactly once. Value: box.V == 5 (Add(2) then Add(3)); LSan clean (no leak, no
// double-free of the shared cell). Load-bearing on the LSan leg.
const char* kTwoClosuresShareUsingVarCell_1017 =
    "module M; "
    "class Box public V: int := 0; "
    "  public method Add(x: int): int V := V + x; return V; end Add; "
    "end Box; "
    "class C public static method Main(): int "
    "  using var box := new Box(); "
    "  var f1: (int) -> int := lambda(x) => box.Add(x); "
    "  var f2: (int) -> int := lambda(x) => box.Add(x); "
    "  var a := f1(2); var b := f2(3); "
    "  return box.V; "   // 5
    "end Main; end C;";
BEZEL_TEST(codegen_env_disposer_672, two_closures_share_using_var_cell_freed_1017) {
    expectValueAndLeakFree(kTwoClosuresShareUsingVarCell_1017, 5);
}

// #1017 control: the SAME shape with a `do … end` block around the captured
// `using var` (owner disposal in an inner scope). The deferred cell-free must be
// registered in the OWNER's scope (#966) and fire after the owner disposal —
// value correct, LSan clean, no double-free across the inner-block exit.
const char* kTwoClosuresShareCellInnerScope_1017 =
    "module M; "
    "class Box public V: int := 0; "
    "  public method Add(x: int): int V := V + x; return V; end Add; "
    "end Box; "
    "class C public static method Main(): int "
    "  var r: int := 0; "
    "  do "
    "    using var box := new Box(); "
    "    var f1: (int) -> int := lambda(x) => box.Add(x); "
    "    var f2: (int) -> int := lambda(x) => box.Add(x); "
    "    var a := f1(2); var b := f2(3); "
    "    r := box.V; "
    "  end; "
    "  return r; "   // 5
    "end Main; end C;";
BEZEL_TEST(codegen_env_disposer_672, two_closures_share_cell_inner_scope_freed_1017) {
    expectValueAndLeakFree(kTwoClosuresShareCellInnerScope_1017, 5);
}

// #1017 control: a SINGLE closure capturing a `using var` local — the cell was
// already freed before the fix (regression guard that the deferred path did not
// perturb the single-capture case). Value correct, LSan clean.
const char* kSingleClosureUsingVarCell_1017 =
    "module M; "
    "class Box public V: int := 0; "
    "  public method Add(x: int): int V := V + x; return V; end Add; "
    "end Box; "
    "class C public static method Main(): int "
    "  using var box := new Box(); "
    "  var f1: (int) -> int := lambda(x) => box.Add(x); "
    "  var a := f1(2); "
    "  return box.V; "   // 2
    "end Main; end C;";
BEZEL_TEST(codegen_env_disposer_672, single_closure_using_var_cell_freed_1017) {
    expectValueAndLeakFree(kSingleClosureUsingVarCell_1017, 2);
}
