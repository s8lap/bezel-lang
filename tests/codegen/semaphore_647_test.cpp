// Codegen for Semaphore (stdlib §6.3, issue #647).
//
// Semaphore: counting semaphore with permits.
// SemaphoreToken: owned guard returned by Acquire / TryAcquire; destructor
//   releases the held permit (bz_semaphore_token_release).
//
// Design (mirrors Mutex §6.2 / #647):
//   bz_semaphore_t: refcounted core (semaphore object +1, each token +1).
//   Uses sem_t (POSIX counting semaphore) for sync acquire/try_acquire;
//   async paths use the waiter-handoff machinery from bz_mutex_acquire_async.
//   TryAcquire timeout: sem_timedwait on CLOCK_REALTIME.
//   Guard (SemaphoreToken): bz_semaphore_token_t*, disposer = bz_semaphore_token_release.
//
// Sendability (per owner instruction / §6.3 v0.140):
//   Semaphore IS sendable (internally synchronized, same reasoning as Mutex).
//   SemaphoreToken is NOT sendable (domain-local held guard).
//
// Tests derive from:
//   - stdlib §6.3 Acquire example (using var token idiom)
//   - stdlib §6.3 TryAcquire example (nullable, narrowing)
//   - Permit counting: N=2 allows 2 acquires, 3rd TryAcquire → null
//   - Release restores: release after 2 acquires allows a 3rd
//   - TryAcquire timeout → null (all permits held; zero-timeout)
//   - double-dispose idempotent (§28.2)
//   - Token outliving Semaphore (refcounted core, ASan / LSan)
//   - using-var guard in a loop (no double-release per iteration)
//   - two-arg constructor (initialCount, maxCount)
//   - async acquire + cancellation + timeout (pool-worker path)
//   - ASan 200× contention/cancel hammer

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
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"
#include "support/emit_object_support.hpp"  // #4569: emitObjectFileForTest

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "support/unique_stem.hpp"

namespace fs = std::filesystem;
using bezel::test::runAot;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Wrap a Main body in a minimal module/class skeleton.
std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

// LeakSanitizer verification (mirrors mutex_lock_647_test.cpp pattern).
// Returns: 0 = clean; 1 = sanitizer fired; -1 = toolchain unavailable (SKIP).
int runUnderLeakSanitizer(const std::string& source, bool detectLeaks = true) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_647sem_lsan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

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
        // #4569: repointed onto the ASan-sweep wrapper, the same rename the
        // other 35 files in the #3917 burn-down took. #4569 listed this file
        // among the "concurrency tail" that needed a per-call sanitizer
        // decision because the *_647 family "carries sibling TSan rows in the
        // same file" -- measured, it does not: this file has exactly ONE
        // emitObjectFile call, no buildRuntimeSanitizerObject, and its link
        // line below is a hardcoded `-fsanitize=address`, so there is no
        // -fsanitize=thread path an instrumented object could reach.
        if (!bezel::test::emitObjectFileForTest(*module, objPath, err))
            return bezel::test::asanBuildFailure("emitObjectFile");
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        cleanup();
        return bezel::test::asanLinkFailure("link");
    }
    std::string runCmd =
        std::string("ASAN_OPTIONS=detect_leaks=") +
        (detectLeaks ? "1" : "0") + ":exitcode=99 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer fired
    return 0;  // clean
}

// Compile + run AOT (for async tests via Driver).
static int compileAndRunAsync(const std::string& source) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_647sem_async");
    std::string srcPath = (dir / (stem + ".bzl")).string();
    std::string exe     = (dir / stem).string();
    {
        std::ofstream out(srcPath);
        out << source;
    }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(srcPath, exe);
    std::remove(srcPath.c_str());
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(exe.c_str());
    return code;
}

static int compileAndRunAsyncRetrying(const std::string& source) {
    int v = compileAndRunAsync(source);
    for (int r = 0; r < 4 && v == -1; ++r) v = compileAndRunAsync(source);
    return v;
}

} // namespace

// ---------------------------------------------------------------------------
// §6.3 Acquire — the canonical "using var token" idiom (verbatim from spec).
// token := sem.Acquire() blocks until a permit is available, returns an owned
// SemaphoreToken; disposing token at scope end releases the permit.
// ---------------------------------------------------------------------------

// stdlib §6.3 example 1: Acquire() returns an owned SemaphoreToken; using-var
// scope exit releases it. Two sequential acquisitions succeed on a N=1 semaphore.
BEZEL_TEST(codegen_semaphore_6_3, acquire_guard_idiom_sequential) {
    // First acquisition and release (scope exit), then second.
    // If token is NOT released, the second Acquire would block forever.
    BEZEL_EXPECT_EQ(run(prog(
        "using var sem := new Semaphore(1); "
        "do "
        "  using var tok1 := sem.Acquire(); "
        "end; "
        "using var tok2 := sem.Acquire(); "
        "return 42; "
    )), 42);
}

// stdlib §6.3: Acquire releases on scope exit; after release TryAcquire succeeds.
BEZEL_TEST(codegen_semaphore_6_3, acquire_releases_on_scope_exit) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sem := new Semaphore(1); "
        "do "
        "  using var tok := sem.Acquire(); "
        "end; "
        // Permit is restored; TryAcquire with 0-ms timeout should succeed.
        "using var maybe := sem.TryAcquire(TimeSpan.FromMilliseconds(0)); "
        "if maybe <> null then "
        "  return 1; "
        "end; "
        "return 0; "
    )), 1);
}

// ---------------------------------------------------------------------------
// §6.3 Permit counting: N=2 allows 2 concurrent acquires; 3rd → null.
// ---------------------------------------------------------------------------

// stdlib §6.3: two-permit semaphore allows 2 acquires; 3rd TryAcquire → null.
BEZEL_TEST(codegen_semaphore_6_3, permit_counting_n2_third_null) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sem := new Semaphore(2); "
        "using var t1 := sem.Acquire(); "
        "using var t2 := sem.Acquire(); "
        // Both permits held; 3rd TryAcquire with 0-ms → null.
        "using var t3 := sem.TryAcquire(TimeSpan.FromMilliseconds(0)); "
        "if t3 = null then "
        "  return 99; "
        "end; "
        "return 0; "
    )), 99);
}

// stdlib §6.3: release one permit from N=2 allows a 3rd TryAcquire to succeed.
BEZEL_TEST(codegen_semaphore_6_3, release_restores_permit) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sem := new Semaphore(2); "
        "using var t1 := sem.Acquire(); "
        "var t2 := sem.Acquire(); "
        "t2.Dispose(); "   // explicit release — permit restored
        // Now 1 permit available; TryAcquire should succeed.
        "using var t3 := sem.TryAcquire(TimeSpan.FromMilliseconds(0)); "
        "if t3 <> null then "
        "  return 7; "
        "end; "
        "return 0; "
    )), 7);
}

// ---------------------------------------------------------------------------
// §6.3 TryAcquire — nullable result idiom.
// TryAcquire(timeout) returns owned SemaphoreToken? — non-null on success.
// ---------------------------------------------------------------------------

// stdlib §6.3 example 2: TryAcquire on an available semaphore succeeds.
BEZEL_TEST(codegen_semaphore_6_3, try_acquire_succeeds_on_available) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sem := new Semaphore(1); "
        "using var tok := sem.TryAcquire(TimeSpan.FromSeconds(5)); "
        "if tok <> null then "
        "  return 7; "
        "end; "
        "return 0; "
    )), 7);
}

// stdlib §6.3: TryAcquire returns null when no permits available.
BEZEL_TEST(codegen_semaphore_6_3, try_acquire_returns_null_when_no_permits) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sem := new Semaphore(1); "
        "using var outer := sem.Acquire(); "
        // No permits left; TryAcquire with 0-ms → null.
        "using var tok := sem.TryAcquire(TimeSpan.FromMilliseconds(0)); "
        "if tok = null then "
        "  return 99; "
        "end; "
        "return 0; "
    )), 99);
}

// stdlib §6.3: two-arg constructor (initialCount, maxCount) — works like
// initialCount variant (the maxCount bound is enforced by the runtime).
BEZEL_TEST(codegen_semaphore_6_3, two_arg_constructor_works) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sem := new Semaphore(2, 4); "
        "using var t1 := sem.Acquire(); "
        "using var t2 := sem.Acquire(); "
        "using var t3 := sem.TryAcquire(TimeSpan.FromMilliseconds(0)); "
        "if t3 = null then "
        "  return 55; "
        "end; "
        "return 0; "
    )), 55);
}

// ---------------------------------------------------------------------------
// fix #4445 / stdlib §6.3 v0.302: the two-arg constructor must raise
// `ArgumentException` when `max < 1` or `initial > max`. The one-arg form
// has no ceiling and is unaffected.
//
// The codegen emits the ArgumentException guard (max < 1 OR initial > max)
// BEFORE calling bz_semaphore_new, so the constructor's runtime path never
// sees a value that would have tripped the guard — only the throw, never a
// silent coercion. The runtime clamps in bz_semaphore_new stay as defensive
// defaults for any direct FFI / misuse path.
//
// Regression tests:
//   * Row (1) initial > max  → ArgumentException
//   * Row (2) max < 1        → ArgumentException
//   * Row (3) initial ≤ max AND max ≥ 1 → no exception (control)
//   * Row (4) one-arg form   → no exception (control: ceiling inert, max=INT_MAX)
// ---------------------------------------------------------------------------

// fix #4445 / stdlib §6.3 v0.302: `new Semaphore(5, 3)` (initial > max) raises
// a catchable ArgumentException. Without the guard, the runtime silently clamps
// max up to 5 and returns a working N=5 semaphore — exactly the v0.302 defect.
BEZEL_TEST(codegen_semaphore_6_3_4445, initial_gt_max_raises_argument_exception_4445) {
    BEZEL_EXPECT_EQ(run(prog(
        "var caught := 0; "
        "try "
        "  var sem := new Semaphore(5, 3); "
        "  sem.Dispose(); "
        "catch ArgumentException => do caught := 1; end "
        "end; "
        "return caught; "                 // 1
    )), 1);
}

// fix #4445 / stdlib §6.3 v0.302: `new Semaphore(0, 0)` (max < 1) raises a
// catchable ArgumentException. Without the guard, max gets clamped up to
// initial=0 — a silent zero-permit semaphore that no acquire can ever fulfil.
BEZEL_TEST(codegen_semaphore_6_3_4445, max_lt_one_raises_argument_exception_4445) {
    BEZEL_EXPECT_EQ(run(prog(
        "var caught := 0; "
        "try "
        "  var sem := new Semaphore(0, 0); "
        "  sem.Dispose(); "
        "catch ArgumentException => do caught := 1; end "
        "end; "
        "return caught; "                 // 1
    )), 1);
}

// fix #4445 / stdlib §6.3 v0.302 CONTROL: `new Semaphore(3, 5)` (initial ≤ max,
// max ≥ 1) constructs successfully and the semaphore works.
BEZEL_TEST(codegen_semaphore_6_3_4445, valid_two_arg_succeeds_4445) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sem := new Semaphore(3, 5); "
        "using var t1 := sem.Acquire(); "
        "using var t2 := sem.Acquire(); "
        "using var t3 := sem.Acquire(); "
        "return 33; "
    )), 33);
}

// fix #4445 / stdlib §6.3 v0.302 CONTROL: `new Semaphore(5)` (one-arg form)
// constructs successfully. The one-arg form has no ceiling; the codegen passes
// max=INT_MAX, so both guard conditions (max < 1, initial > max) are inert.
BEZEL_TEST(codegen_semaphore_6_3_4445, one_arg_form_unaffected_4445) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sem := new Semaphore(5); "
        "using var t1 := sem.Acquire(); "
        "using var t2 := sem.Acquire(); "
        "using var t3 := sem.Acquire(); "
        "using var t4 := sem.Acquire(); "
        "using var t5 := sem.Acquire(); "
        "return 44; "
    )), 44);
}

// ---------------------------------------------------------------------------
// §6.3 / §28.2: Double-dispose idempotency.
// Calling Dispose() on a SemaphoreToken twice must not crash or double-release.
// A subsequent Acquire must still succeed (the permit was released exactly once).
// ---------------------------------------------------------------------------

// §28.2: explicit double-dispose of a SemaphoreToken is idempotent.
BEZEL_TEST(codegen_semaphore_6_3_28_2, token_double_dispose_idempotent) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sem := new Semaphore(1); "
        "var tok := sem.Acquire(); "
        "tok.Dispose(); "   // explicit first dispose — permit released
        "tok.Dispose(); "   // second dispose — must be a no-op
        // Semaphore should have 1 permit now; Acquire again must succeed.
        "using var t2 := sem.Acquire(); "
        "return 5; "
    )), 5);
}

// §28.2: explicit double-dispose of a Semaphore is idempotent.
// After explicit Dispose, the using-var scope-exit auto-dispose must be a no-op.
BEZEL_TEST(codegen_semaphore_6_3_28_2, semaphore_double_dispose_idempotent) {
    BEZEL_EXPECT_EQ(run(prog(
        "var sem := new Semaphore(1); "
        "sem.Dispose(); "   // explicit first dispose
        "sem.Dispose(); "   // second dispose — must be a no-op
        "return 3; "
    )), 3);
}

// ---------------------------------------------------------------------------
// §6.3: Token guard idiom in a loop — using-var releases each iteration without
// double-release at the end of the outer scope.
// ---------------------------------------------------------------------------

// §6.3: a `using var` token inside a loop releases each iteration.
BEZEL_TEST(codegen_semaphore_6_3, guard_in_loop_no_double_release) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sem := new Semaphore(1); "
        "var i := 0; "
        "while i < 3 do "
        "  using var tok := sem.Acquire(); "
        "  i := i + 1; "
        "end; "
        "using var final := sem.Acquire(); "
        "return 1; "
    )), 1);
}

// ---------------------------------------------------------------------------
// ASan / LSan: Memory safety for token idiom, double-dispose, and
// Token-outlives-Semaphore. Each test runs under LeakSanitizer.
// ---------------------------------------------------------------------------

// ASan/LSan §6.3: token-idiom scope is leak-free (both Semaphore and Token freed).
BEZEL_TEST(codegen_semaphore_asan_6_3, acquire_guard_leak_free) {
    int r = runUnderLeakSanitizer(prog(
        "using var sem := new Semaphore(1); "
        "using var tok := sem.Acquire(); "
        "return 0; "
    ));
    if (r == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(r, 0);  // 0 = clean
}

// ASan/LSan §6.3: TryAcquire-success guard is leak-free.
BEZEL_TEST(codegen_semaphore_asan_6_3, try_acquire_success_leak_free) {
    int r = runUnderLeakSanitizer(prog(
        "using var sem := new Semaphore(1); "
        "using var tok := sem.TryAcquire(TimeSpan.FromSeconds(1)); "
        "if tok <> null then "
        "end; "
        "return 0; "
    ));
    if (r == -1) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ASan/LSan §28.2: double-dispose idempotent — no double-free.
BEZEL_TEST(codegen_semaphore_asan_28_2, double_dispose_no_double_free) {
    int r = runUnderLeakSanitizer(prog(
        "var sem := new Semaphore(1); "
        "sem.Dispose(); "
        "sem.Dispose(); "
        "return 0; "
    ), /*detectLeaks=*/false);  // only UAF/DF check
    if (r == -1) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ASan/LSan: Token outliving Semaphore — the refcounted core keeps semaphore
// memory alive until the last Token is released.  No UAF.
BEZEL_TEST(codegen_semaphore_asan_6_3, token_outlives_semaphore_no_uaf) {
    int r = runUnderLeakSanitizer(prog(
        "var sem := new Semaphore(1); "
        "var tok := sem.Acquire(); "
        "sem.Dispose(); "  // explicit Semaphore dispose — core still alive (Token holds ref)
        "tok.Dispose(); "  // Token dispose → release → core_refs → 0 → free
        "return 0; "
    ), /*detectLeaks=*/true);
    if (r == -1) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ASan/LSan: guard-in-loop — 5 iterations, each releasing; no accumulation.
BEZEL_TEST(codegen_semaphore_asan_6_3, guard_in_loop_5x_leak_free) {
    int r = runUnderLeakSanitizer(prog(
        "using var sem := new Semaphore(1); "
        "var i := 0; "
        "while i < 5 do "
        "  using var tok := sem.Acquire(); "
        "  i := i + 1; "
        "end; "
        "return 0; "
    ));
    if (r == -1) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ---------------------------------------------------------------------------
// §6.3 / §30.14: Semaphore sendability — AOT compile probe.
// Verify a program passing Semaphore as Domain.Spawn arg compiles to valid IR.
// ---------------------------------------------------------------------------

// §6.3 / §30.14: Semaphore as Domain.Spawn arg compiles to valid IR.
BEZEL_TEST(codegen_semaphore_6_3_30_14, semaphore_spawn_arg_compiles_to_valid_ir) {
    const std::string source =
        "module M; "
        "class Worker "
        "  public static method Run(owned s: Semaphore) "
        "    do "
        "      using var tok := s.Acquire(); "
        "    end; "
        "    s.Dispose(); "
        "  end Run; "
        "end Worker; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(Worker.Run, new Semaphore(1)); "
        "    await t; "
        "    return 42; "
        "  end Main; "
        "end Program;";

    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(source, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    BEZEL_EXPECT_TRUE(!!unit);

    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::sema::checkDefiniteAssignment(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    // No sendability diagnostic (Semaphore is sendable per §6.3).
    BEZEL_EXPECT_FALSE(diags.hasErrors());

    llvm::LLVMContext llvmCtx;
    auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
    BEZEL_EXPECT_TRUE(!!module);
}

// ---------------------------------------------------------------------------
// §6.3 / #647 / v0.140: AcquireAsync / TryAcquireAsync IR-level tests.
// AcquireAsync → OwningTask<SemaphoreToken>
// TryAcquireAsync → OwningTask<SemaphoreToken?>
// ---------------------------------------------------------------------------

// §6.3 / #647 / v0.140: AcquireAsync compiles to valid IR without errors.
BEZEL_TEST(codegen_semaphore_acquire_async_6_3, acquire_async_compiles_valid_ir) {
    const std::string source =
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var sem := new Semaphore(1); "
        "    using var tok := await sem.AcquireAsync(); "
        "    return 42; "
        "  end Main; "
        "end C;";

    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(source, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    BEZEL_EXPECT_TRUE(!!unit);

    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::sema::checkDefiniteAssignment(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    BEZEL_EXPECT_FALSE(diags.hasErrors());

    llvm::LLVMContext llvmCtx;
    auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
    BEZEL_EXPECT_TRUE(!!module);
}

// §6.3 / #647 / v0.140: TryAcquireAsync compiles to valid IR without errors.
BEZEL_TEST(codegen_semaphore_acquire_async_6_3, try_acquire_async_compiles_valid_ir) {
    const std::string source =
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var sem := new Semaphore(2); "
        "    using var tok := await sem.TryAcquireAsync(TimeSpan.FromSeconds(1)); "
        "    if tok <> null then "
        "      return 7; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;";

    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(source, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    BEZEL_EXPECT_TRUE(!!unit);

    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::sema::checkDefiniteAssignment(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    BEZEL_EXPECT_FALSE(diags.hasErrors());

    llvm::LLVMContext llvmCtx;
    auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
    BEZEL_EXPECT_TRUE(!!module);
}

// ---------------------------------------------------------------------------
// §6.3 / #647 / v0.140 AOT: AcquireAsync uncontended.
// §6.3 verbatim example:
//   using var token := await sem.AcquireAsync();
//   DoCriticalWork();
//   // token disposed at scope end -> semaphore released.
// ---------------------------------------------------------------------------

// §6.3 / #647 / v0.140 AOT: AcquireAsync uncontended — token works and releases.
BEZEL_TEST(codegen_semaphore_acquire_async_aot_6_3, acquire_async_uncontended_guard) {
    const std::string source =
        "module M; "
        "class C "
        // §6.3 verbatim: `using var token := await sem.AcquireAsync();`
        // The token is scoped to the `do` block and disposed at its end.
        // A second acquire proves the permit was released (N=1 semaphore).
        "  public static async method Main(): Task<int> "
        "    using var sem := new Semaphore(1); "
        "    do "
        "      using var tok := await sem.AcquireAsync(); "
        "    end; "
        // Token is now disposed; permit is free — second acquire must succeed.
        "    do "
        "      using var tok2 := await sem.AcquireAsync(); "
        "    end; "
        "    return 42; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(compileAndRunAsyncRetrying(source), 42);
}

// §6.3 / #647 / v0.140 AOT: TryAcquireAsync on available semaphore — non-null token.
BEZEL_TEST(codegen_semaphore_acquire_async_aot_6_3, try_acquire_async_available_nonnull) {
    const std::string source =
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var sem := new Semaphore(1); "
        "    using var tok := await sem.TryAcquireAsync(TimeSpan.FromSeconds(5)); "
        "    if tok <> null then "
        "      return 7; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(compileAndRunAsyncRetrying(source), 7);
}

// §6.3 / #647 / v0.140 AOT: TryAcquireAsync with zero timeout on exhausted semaphore → null.
BEZEL_TEST(codegen_semaphore_acquire_async_aot_6_3, try_acquire_async_zero_timeout_null) {
    const std::string source =
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var sem := new Semaphore(1); "
        // §30.12 (v0.179 / #1522): the blocking Acquire() form is rejected
        // lexically inside async methods — use AcquireAsync() (immediately
        // available on a fresh semaphore, so this still completes synchronously).
        "    using var outer := await sem.AcquireAsync(); "
        // Permit exhausted; TryAcquireAsync with 0-ms → null.
        "    using var tok := await sem.TryAcquireAsync(TimeSpan.FromMilliseconds(0)); "
        "    if tok = null then "
        "      return 99; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(compileAndRunAsyncRetrying(source), 99);
}

// ---------------------------------------------------------------------------
// §6.3 / #647 / v0.140 AOT: pre-cancelled CancellationToken → OCE fault.
// ---------------------------------------------------------------------------

// §6.3 / #647 / v0.140 AOT: pre-cancelled token → AcquireAsync faults with OCE.
BEZEL_TEST(codegen_semaphore_acquire_async_aot_6_3, acquire_async_precancelled_faults) {
    const std::string source =
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var cts := new CancellationTokenSource(); "
        "    cts.Cancel(); "
        "    using var sem := new Semaphore(1); "
        "    try "
        "      using var tok := await sem.AcquireAsync(cts.Token); "
        "      return 0; "
        "    catch OperationCanceledException => return 77; "
        "    end; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(compileAndRunAsyncRetrying(source), 77);
}

// §6.3 / #647 / v0.140 AOT: pre-cancelled token → TryAcquireAsync faults.
BEZEL_TEST(codegen_semaphore_acquire_async_aot_6_3, try_acquire_async_precancelled_faults) {
    const std::string source =
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var cts := new CancellationTokenSource(); "
        "    cts.Cancel(); "
        "    using var sem := new Semaphore(1); "
        "    try "
        "      using var tok := await sem.TryAcquireAsync("
        "          TimeSpan.FromSeconds(5), cts.Token); "
        "      return 0; "
        "    catch OperationCanceledException => return 77; "
        "    end; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(compileAndRunAsyncRetrying(source), 77);
}

// ---------------------------------------------------------------------------
// §6.3 / #647 / v0.140 ASan/LSan: AcquireAsync leak-free.
// ---------------------------------------------------------------------------

// ASan/LSan §6.3 / #647 / v0.140: AcquireAsync uncontended — no leaks or UAF.
BEZEL_TEST(codegen_semaphore_acquire_async_asan_6_3, acquire_async_uncontended_leak_free) {
    int r = runUnderLeakSanitizer(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var sem := new Semaphore(1); "
        "    using var tok := await sem.AcquireAsync(); "
        "    return 0; "
        "  end Main; "
        "end C;"
    );
    if (r == -1) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ASan/LSan §6.3 / #647 / v0.140: TryAcquireAsync uncontended — no leaks or UAF.
BEZEL_TEST(codegen_semaphore_acquire_async_asan_6_3, try_acquire_async_uncontended_leak_free) {
    int r = runUnderLeakSanitizer(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var sem := new Semaphore(1); "
        "    using var tok := await sem.TryAcquireAsync(TimeSpan.FromSeconds(1)); "
        "    if tok <> null then "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"
    );
    if (r == -1) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ---------------------------------------------------------------------------
// §6.3 / #647 / v0.140: Cancel-vs-release race hammer (200×).
// Pre-cancel fast path hammered under ASan detect_leaks=1 — no UAF/leak.
// (See mutex_lock_647_test.cpp for the documented boundary on the pool-worker
//  cancel race — same reasoning applies here.)
// ---------------------------------------------------------------------------

// §6.3 / #647 / v0.140 ASan: pre-cancel fast path hammered 200× — no UAF/leak.
BEZEL_TEST(codegen_semaphore_acquire_async_asan_6_3, precancelled_fast_path_200x_clean) {
    int r = runUnderLeakSanitizer(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var sem := new Semaphore(1); "
        "    using var cts := new CancellationTokenSource(); "
        "    cts.Cancel(); "
        "    var i := 0; "
        "    while i < 200 do "
        "      try "
        "        using var tok := await sem.AcquireAsync(cts.Token); "
        "      catch OperationCanceledException => i := i; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return 42; "
        "  end Main; "
        "end C;"
    );
    if (r == -1) return;
    BEZEL_EXPECT_EQ(r, 0);
}
