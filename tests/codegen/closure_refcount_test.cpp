// Codegen + runtime — closure-cell reference counting (issue #422, closures
// increment). Per §28.9, function-typed values (lambda closures, capturing or
// not, and method references) are RUNTIME-MANAGED and exempt from manual
// disposal: their backing heap object is reclaimed automatically. Before this
// change codegen NEVER freed a closure cell or its captured-variable env —
// every lambda / method reference calloc'd a `{ fn, env }` cell (and, when
// capturing, an env block) and leaked them.
//
// The closure cell grows from { ptr fn, ptr env } (16 bytes) to
// { ptr fn, ptr env, i64 rc } (24 bytes): rc is appended AFTER env so fn (off
// 0) and env (off 8) keep their offsets — every call site loads fn at offset 0
// and env at GEP index 1, both unmoved. Every cell producer (lowerLambda,
// makeMethodThunk) initializes rc := 1 (the producer's reference).
// bz_closure_release decrements and, at zero, frees the env block then the
// cell; bz_closure_retain increments; both are no-ops on null.
//
// CRITICAL — captured values are NOT owned by the closure. Per §28.9 (line:
// "Capturing an owned class-typed local does NOT transfer ownership into the
// closure. ... The original `using var` (or other owned location) retains the
// disposal obligation.") and the §28.3 capture table ("Captured owned local
// inside a closure → Borrowed"), a closure holds only BORROWS of its captures.
// Freeing a closure cell therefore frees ONLY the cell and its env block — it
// must NEVER dispose a captured value (the original owned location disposes it
// exactly once; disposing it again from the closure would be a double-free).
// The boxed capture cells (the shared heap cells that back captured locals) are
// likewise NOT freed by the closure: they are shared with the enclosing frame
// and outlive the closure (freeing them on closure release would be a UAF).
//
// Codegen releases a closure-typed `var` local at scope end ONLY when a
// conservative whole-body walk proves it never escapes AND it was initialized
// by a codegen-internal +1 producer (a lambda expression / method reference —
// never a call result, to avoid return-handoff ambiguity). Closures that escape
// (returned, stored in a field, passed as an argument, aliased) are
// conservatively LEFT leaked (a leak is acceptable; a double-free / UAF is not).
//
// Two complementary checks, mirroring string_refcount_test.cpp:
//   1. compileAndRun: native exit code proves correctness (the value is right;
//      and for the captured-owned case, the destructor count proves the capture
//      is disposed EXACTLY once — not zero, not twice).
//   2. runUnderLeakSanitizer: re-lower + link against an ASan-built runtime and
//      run under LSan — verifies the reclaim (no leak) AND memory safety (no
//      use-after-free / double-free). Skipped gracefully when no toolchain.

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
static const std::filesystem::path& wtDir422cl() {
    return wtScratchDir("wt-422cl");
}

// Compile `source` to a temp native executable via the real Driver pipeline and
// run it; returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir422cl();
    std::string stem = unique_stem("bezel_422cl");
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
//   1  — sanitizer reported a problem (leak or corruption): test failure.
//  -1  — toolchain unavailable / build step failed: SKIP (not a failure).
// Uses the memoized ASan runtime object (compiled once per process).
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    auto dir = wtDir422cl();
    std::string stem = unique_stem("bezel_422cl_lsan");
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
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -1; }

    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                      // clean
}

// Assert correctness (exit code) AND, when the toolchain is present, that LSan
// reports no leak/UAF/double-free.
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);     // 0 == LSan clean (no leak, no UAF/DF)
}

// Assert correctness AND that ASan reports no MEMORY-SAFETY error
// (use-after-free / double-free), but TOLERATE a leak. Used for the
// conservatively-leaked escape cases (returned / field-stored / aliased
// closures) where this increment intentionally over-retains (leaks) rather than
// risk a premature free. A leak exits 99 under detect_leaks; a UAF/DF also
// exits non-zero — so we run with detect_leaks=0 and require a clean exit,
// which still catches every memory-safety corruption (the thing that must NOT
// regress) while permitting the deliberate leak.
void expectValueAndNoCorruption(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return;  // toolchain unavailable: skip
    auto dir = wtDir422cl();
    std::string stem = unique_stem("bezel_422cl_safe");
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
    // exitcode=99: a DISTINCT ASan failure code so a UAF/double-free can never
    // be confused with the program legitimately returning expectedExitCode
    // (ASan's default error exitcode is 1, masking a UAF in any test that
    // expects 1). No test expects 99.
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=0:exitcode=99:abort_on_error=0 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return;
    BEZEL_EXPECT_TRUE(WIFEXITED(status) &&
                      WEXITSTATUS(status) == expectedExitCode);
}

// --- Programs ------------------------------------------------------------

// §27.5 / §28.9 (load-bearing, env-reclaim): a long loop that builds and drops a
// freshly CAPTURING closure into a non-escaping local each iteration. The lambda
// captures `this` (an instance method's receiver), which §27.5 stores in the env
// BY VALUE (the receiver is not reassignable, so no shared cell is boxed). So
// each iteration allocates a closure cell { fn, env, rc } AND a non-null env
// block (the 8-byte capture array holding the `this` pointer) — and BOTH are
// reclaimed by the scope-end bz_closure_release (the local is non-escaping, used
// only as a direct-call callee). Steady-state, LSan-clean. Before this change
// every iteration leaked the closure cell + env block. (A capturing closure over
// a value-typed LOCAL additionally boxes that local into a separate shared cell,
// which is NOT a closure cell and is leaked separately — see kCapturingIntChurn
// below; that orthogonal leak is why this LSan-clean test captures `this`, whose
// capture needs no boxed cell.) Returns 0.
const char* kLoopThisCapturingChurnSteadyState =
    "module M; class C "
    "  public _base: int := 10; "
    "  public method Run(): int "
    "    var acc: int := 0; var i: int := 0; "
    "    while i < 200000 do "
    "      var f: (int) -> int := lambda(x: int) => x + this._base; "
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

// §27.5 / §28.9: a long loop that builds and drops a NON-capturing lambda each
// iteration. A non-capturing lambda still allocates a `{ fn, null_env, rc }`
// cell (no env block). The local is non-escaping (a direct call) and
// +1-produced → released each iteration. Reclaimed to LSan-clean steady state.
const char* kLoopNonCapturingChurnSteadyState =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; var acc: int := 0; "
    "    while i < 200000 do "
    "      var f: (int) -> int := lambda(x: int) => x * 2; "
    "      acc := acc + f(1); "
    "      i := i + 1; "
    "    end; "
    "    if acc > 0 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §27.5: a closure that captures a value-typed LOCAL (`k`), churned in a loop.
// The closure cell + env block are reclaimed each iteration (my increment), but
// the BOXED cell that §27.5 allocates to hold the captured `k` (a separate
// shared-storage heap cell, NOT a closure cell — it backs the captured variable,
// is shared with the enclosing frame, and may be shared with other closures) is
// a distinct allocation class that closure-cell refcounting does NOT reclaim:
// freeing it on closure release would be a UAF (the frame / another closure may
// still read it). So this case retains a bounded per-creation leak of the boxed
// cell — TOLERATED (leak-over-double-free) — while proving the value is correct
// and there is NO corruption. Value + no-corruption (a deliberate leak, never a
// double-free / UAF). Returns 0.
const char* kCapturingIntChurn =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; var acc: int := 0; "
    "    while i < 5000 do "
    "      var k: int := i; "
    "      var f: (int) -> int := lambda(x: int) => x + k; "
    "      acc := acc + f(1); "
    "      i := i + 1; "
    "    end; "
    "    if acc > 0 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 (DECISIVE double-dispose guard): a closure captures an OWNED class
// resource, the closure is created + invoked + dropped, then the `using var`
// scope ends. Per §28.9 the closure does NOT own the capture — it holds a
// borrow; the `using var` retains the disposal obligation. The destructor must
// run EXACTLY ONCE (from the using-var scope end), regardless of the closure's
// reclamation. If the closure release wrongly disposed the captured value the
// count would be 2 (a double-dispose / double-free). Exit code = _n == 1. Here
// the single boxed capture cell is reclaimed through the using-var disposal, so
// this single-instance case is additionally LSan-clean (the loop variant
// kCapturedOwnedLoopDisposedOnce, with per-iteration boxed cells, is not).
const char* kCapturedOwnedDisposedOnce =
    "module M; class R "
    "  public static _n: int := 0; "
    "  public method Tag(): int return 7; end Tag; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Main(): int "
    "    do "
    "      using var r := new R(); "
    "      var f: () -> int := lambda() => r.Tag(); "
    "      _ := f(); "
    "    end; "
    "    return _n; "                 // exactly one disposal → 1
    "  end Main; "
    "end R;";

// §28.9: the same capture-and-dispose pattern run MANY times in a loop. The
// destructor count must equal the iteration count (each `using var r` disposes
// its own r exactly once; the closure never disposes r). A double-dispose would
// show as 2*N → _n ≠ 5000 (and ASan would abort on the double-free of r's
// block). Run for value + no-corruption: the captured class reference is boxed
// into a per-iteration shared cell (§27.5) that closure-cell refcounting does
// not reclaim — a TOLERATED bounded leak — so this is NOT an LSan-clean case;
// the load-bearing property it pins is the EXACTLY-ONCE disposal (no double-free
// of the capture) under ASan, which holds regardless of that leak.
const char* kCapturedOwnedLoopDisposedOnce =
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
    "    if _n = 5000 then return 0; end; "  // each r disposed once; no doubles
    "    return 1; "
    "  end Main; "
    "end R;";

// §27.5: an ESCAPING closure — returned from a factory (the spec's MakeCounter).
// The closure outlives the factory's scope, so it is conservatively LEFT leaked
// (not released by the factory). Correctness: the returned closure is intact and
// callable across the boundary (no premature free / UAF). Value + no-corruption
// (a deliberate leak is tolerated). next()+next()+next() = 1+2+3 = 6.
const char* kEscapingClosureReturned =
    "module M; class C "
    "  public static method MakeCounter(): () -> int "
    "    var count := 0; "
    "    return lambda() => do "
    "      count := count + 1; "
    "      return count; "
    "    end; "
    "  end MakeCounter; "
    "  public static method Main(): int "
    "    var next := MakeCounter(); "
    "    return next() + next() + next(); "  // 6
    "  end Main; "
    "end C;";

// §27.5: an ESCAPING closure aliased into a second local (`var g := f`). With
// this increment `f` escapes (bound into `g`) so it is NOT released, and `g`'s
// initializer is an identifier (not a +1 producer) so it is NOT released either
// — the cell is leaked exactly once, never double-freed. Correctness: both names
// invoke the same closure; no corruption from sharing. Value + no-corruption.
const char* kAliasedClosureSharedFreedOnce =
    "module M; class C "
    "  public static method Apply(f: (int) -> int, v: int): int "
    "    return f(v); "
    "  end Apply; "
    "  public static method Main(): int "
    "    var k := 10; "
    "    var f: (int) -> int := lambda(x: int) => x + k; "
    "    var g: (int) -> int := f; "
    "    return Apply(f, 1) + Apply(g, 1); "  // (1+10)+(1+10) = 22
    "  end Main; "
    "end C;";

// §27.6 / §28.9: a method reference (a function value backed by a closure cell)
// churned in a loop. A static method reference lowers to a
// `{ thunk, null_env, rc }` cell; non-escaping (a direct call) + +1-produced →
// released each iteration. LSan-clean. The thunk's env is null for a static
// method, so only the cell is reclaimed.
const char* kMethodRefChurnSteadyState =
    "module M; class H "
    "  public static method Square(x: int): int => x * x; "
    "end H; "
    "class C "
    "  public static method Main(): int "
    "    var i: int := 0; var acc: int := 0; "
    "    while i < 200000 do "
    "      var f: (int) -> int := H.Square; "
    "      acc := acc + f(2); "
    "      i := i + 1; "
    "    end; "
    "    if acc > 0 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// --- FINDING [1] (§28.9 / §28.3): a +1 closure cell captured by an OUTER lambda
// THROUGH a safe-read sub-position (a call-callee `() => g()`) must NOT be
// released at scope end. The outer closure's env borrows (aliases) the captured
// cell and outlives the producing scope (§28.9: the closure value is
// runtime-managed; §28.3 classifies a capture as a borrow). Before the fix the
// escape walker re-applied call-callee safe-read suppression inside the lambda
// body, judged `g` non-escaping, and armed a scope-end bz_closure_release — a
// use-after-free (ASan: heap-buffer-overflow in bz_closure_release on the freed
// env cell). The fix makes ANY captured-local read inside a lambda an escape, so
// `g` is conservatively LEFT leaked (leak-over-UAF). Value + no-corruption.

// FINDING [1] D1: captured closure invoked inside the outer lambda `() => g()`.
const char* kCaptureClosureCalled =
    "module M; class C "
    "  public static method Main(): int "
    "    var g: () -> int := lambda() => 7; "
    "    var f: () -> int := lambda() => g(); "
    "    var r := f(); "             // g() => 7
    "    return r; "
    "  end Main; "
    "end C;";

// FINDING [1] D1 CONTROL: a bare-identifier closure capture `() => g` — already
// classified as an escape before the fix (forwarded onward). Documents the
// already-correct case. The outer lambda returns g, which the caller invokes.
const char* kCaptureClosureBareIdent =
    "module M; class C "
    "  public static method Main(): int "
    "    var g: () -> int := lambda() => 7; "
    "    var f := lambda() => g; "         // outer lambda forwards g (inferred type)
    "    var inner := f(); "
    "    return inner(); "           // g() => 7
    "  end Main; "
    "end C;";

}  // namespace

// --- LSan-clean tests (the reclaim: leaked before, clean after) ----------

BEZEL_TEST(codegen_closure_refcount_422, loop_this_capturing_churn_reclaimed_27_5) {
    // The load-bearing leak test: 200k iterations each freeing the capturing
    // closure cell AND its (non-null) env block at scope end → LSan-clean steady
    // state. Captures `this` (stored by value in the env, no boxed cell), so the
    // env-block reclaim is exercised cleanly. Leaked before this change.
    expectValueAndLeakFree(kLoopThisCapturingChurnSteadyState, 0);
}

BEZEL_TEST(codegen_closure_refcount_422, loop_non_capturing_churn_reclaimed_27_5) {
    // A non-capturing lambda's cell (null env) is reclaimed each iteration too.
    expectValueAndLeakFree(kLoopNonCapturingChurnSteadyState, 0);
}

BEZEL_TEST(codegen_closure_refcount_422, method_ref_churn_reclaimed_27_6) {
    // A static method reference's cell (null env) is reclaimed each iteration.
    expectValueAndLeakFree(kMethodRefChurnSteadyState, 0);
}

BEZEL_TEST(codegen_closure_refcount_422, captured_owned_disposed_exactly_once_28_9) {
    // The decisive double-dispose guard (single instance, also LSan-clean): the
    // captured owned `r` is disposed EXACTLY once — by the using-var scope end,
    // never by the closure release. Exit code 1 == _n after exactly one
    // destructor run. A double-dispose would yield 2 (and ASan would abort on the
    // double-free of r's block). The closure cell + env + the single boxed cell
    // are all reclaimed → LSan-clean.
    expectValueAndLeakFree(kCapturedOwnedDisposedOnce, 1);
}

// --- Value + no-corruption tests (tolerated leaks; double-free forbidden) -

BEZEL_TEST(codegen_closure_refcount_422, captured_owned_loop_disposed_once_28_9) {
    // 5000 capture-and-drop iterations: each r disposed exactly once (_n == 5000);
    // a double-dispose would yield 10000 (≠ 5000 → exit 1) and an ASan abort. The
    // per-iteration boxed capture cells are a tolerated leak (closure-cell
    // refcounting does not reclaim them) → value + no-corruption, not LSan-clean.
    expectValueAndNoCorruption(kCapturedOwnedLoopDisposedOnce, 0);
}

BEZEL_TEST(codegen_closure_refcount_422, capturing_int_churn_no_corruption_27_5) {
    // A value-typed capture boxes `k` into a shared cell that closure-cell
    // refcounting does not reclaim (freeing it would be a UAF). The closure cell
    // + env ARE reclaimed; the boxed cell leak is tolerated. Value correct, no
    // corruption (no double-free / UAF).
    expectValueAndNoCorruption(kCapturingIntChurn, 0);
}

BEZEL_TEST(codegen_closure_refcount_422, escaping_closure_returned_no_uaf_27_5) {
    // An escaping closure (returned from MakeCounter) is conservatively LEFT
    // leaked — never released by the factory — so it stays valid across the
    // boundary (no premature free / UAF). Value 6, no corruption.
    expectValueAndNoCorruption(kEscapingClosureReturned, 6);
}

BEZEL_TEST(codegen_closure_refcount_422, aliased_closure_freed_once_no_double_free_27_5) {
    // Sharing a closure between two names must never free it twice. (It is freed
    // zero times here — a tolerated leak — but crucially NOT twice; ASan would
    // abort on a double-free.)
    expectValueAndNoCorruption(kAliasedClosureSharedFreedOnce, 22);
}

// --- FINDING [1] (§28.9 / §28.3): captured-closure UAF — was a heap-buffer-
// overflow in bz_closure_release before the fix; now a tolerated leak (value +
// no-corruption). The cardinal assertion is NO memory-safety error under ASan.

BEZEL_TEST(codegen_closure_refcount_422, capture_closure_called_no_uaf_28_9) {
    // D1: () => g() — a captured closure invoked (call-callee safe-read) inside
    // an outer lambda. UAF in bz_closure_release before fix; clean (leaked)
    // after — the env borrows the captured cell, so it must not be released.
    expectValueAndNoCorruption(kCaptureClosureCalled, 7);
}

BEZEL_TEST(codegen_closure_refcount_422, capture_closure_bare_ident_control_no_uaf_28_9) {
    // D1 CONTROL: () => g — a bare-identifier closure capture, already an escape
    // before the fix; documents the already-correct case.
    expectValueAndNoCorruption(kCaptureClosureBareIdent, 7);
}
