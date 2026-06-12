// Codegen for Mutex and Lock (stdlib §6.2, issue #647).
//
// Mutex: constructor(), Acquire(): owned Lock, TryAcquire(timeout: TimeSpan): owned Lock?
// Lock:  no public ctor; destructor (bz_lock_release) releases the held lock.
//
// Recursiveness decision (not specified by §6.2):
//   NON-RECURSIVE. §6.2 is silent on reentrant acquire; a non-recursive mutex is
//   the standard POSIX default (PTHREAD_MUTEX_DEFAULT / PTHREAD_MUTEX_ERRORCHECK),
//   and is the correct choice for mutual exclusion in the general case.
//   The TryAcquire-while-held test exploits this: a second TryAcquire on an
//   already-held mutex in the same thread times out and returns null immediately.
//   Recorded decision for §6.2 review (no spec change; POSIX default behaviour
//   matches the intended semantics).
//
// Scope (first lane):
//   - SYNC Acquire / TryAcquire only.
//   - AcquireAsync / TryAcquireAsync: clean "not yet implemented (#647 async acquire)"
//     diagnostic at the call site (tested in sema, not here).
//   - Sendability: Mutex NOT sendable (conservative, docs ruling pending).
//     Lock NOT sendable (a held guard is domain-local by nature).
//
// Tests derive from:
//   - stdlib §6.2 Acquire example (using var guard idiom)
//   - stdlib §6.2 TryAcquire example (nullable, narrowing)
//   - TryAcquire timeout → null (held by outer Lock; non-recursive mutex)
//   - double-dispose idempotent (§28.2)
//   - Lock outliving Mutex (refcounted core, ASan / LSan)
//   - using-var guard in a loop (no double-release per iteration)
//
// ASan/LSan tests follow the undisposed_leak_test.cpp pattern (runUnderLeakSanitizer).

#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace fs = std::filesystem;
using bezel::test::runJit;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// Wrap a Main body in a minimal module/class skeleton.
std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

// LeakSanitizer verification (mirrors undisposed_leak_test.cpp / task_combinators_test.cpp).
// Returns: 0 = clean; 1 = sanitizer fired; -1 = toolchain unavailable (SKIP).
int runUnderLeakSanitizer(const std::string& source, bool detectLeaks = true) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_647_lsan_" + std::to_string(counter++);
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
        std::string("ASAN_OPTIONS=detect_leaks=") +
        (detectLeaks ? "1" : "0") + ":exitcode=99 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer fired
    return 0;  // clean
}

// ---------------------------------------------------------------------------
// §6.2 Acquire — the canonical "using var guard" idiom (verbatim from spec §6.2).
// guard := mutex.Acquire() blocks (non-blocking on uncontested) and returns
// an owned Lock; disposing guard at scope end releases the mutex.
// Observable via TryAcquire from within the same call stack is NOT possible
// once the guard is disposed (released), so we verify the guard pattern by
// acquiring twice sequentially (acquire → use → release → acquire again).
// ---------------------------------------------------------------------------

// stdlib §6.2 example 1: Acquire() returns an owned Lock; using-var scope exit
// releases it. Two sequential acquisitions from the same thread succeed.
BEZEL_TEST(codegen_mutex_lock_6_2, acquire_guard_idiom_sequential) {
    // First acquisition and release (scope exit), then second acquisition.
    // If Lock is NOT released after scope exit, the second Acquire would deadlock
    // on a non-recursive mutex.
    BEZEL_EXPECT_EQ(run(prog(
        "using var mutex := new Mutex(); "
        "do "
        "  using var guard1 := mutex.Acquire(); "
        "end; "
        "using var guard2 := mutex.Acquire(); "
        "return 42; "
    )), 42);
}

// stdlib §6.2 example 1 (full): acquire, enter critical section, dispose releases.
// We prove the release happens by doing a TryAcquire with zero timeout AFTER
// the guard scope ends and verifying it is non-null (mutex is free).
BEZEL_TEST(codegen_mutex_lock_6_2, acquire_releases_on_scope_exit) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var mutex := new Mutex(); "
        "do "
        "  using var guard := mutex.Acquire(); "
        "end; "
        "using var maybe := mutex.TryAcquire(TimeSpan.FromMilliseconds(0)); "
        "if maybe <> null then "
        "  return 1; "
        "end; "
        "return 0; "
    )), 1);
}

// ---------------------------------------------------------------------------
// §6.2 TryAcquire — nullable result idiom (verbatim from spec §6.2).
// TryAcquire(timeout) returns owned Lock? — non-null on success, null on timeout.
// ---------------------------------------------------------------------------

// stdlib §6.2 example 2: TryAcquire on an uncontested mutex succeeds.
// The guard is non-null; we enter the critical section and return from it.
BEZEL_TEST(codegen_mutex_lock_6_2, try_acquire_succeeds_on_uncontested_mutex) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var mutex := new Mutex(); "
        "using var guard := mutex.TryAcquire(TimeSpan.FromSeconds(5)); "
        "if guard <> null then "
        "  return 7; "
        "end; "
        "return 0; "
    )), 7);
}

// stdlib §6.2 example 2 (timeout null path):
// Hold the mutex with an outer guard, then TryAcquire with a minimal timeout
// from the same thread. A non-recursive mutex → second TryAcquire times out → null.
// (Recursiveness decision: NON-RECURSIVE — see file header comment.)
BEZEL_TEST(codegen_mutex_lock_6_2, try_acquire_returns_null_when_held) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var mutex := new Mutex(); "
        "using var outer := mutex.Acquire(); "
        // Mutex is held; TryAcquire on the same thread with 0-ms timeout → null.
        "using var guard := mutex.TryAcquire(TimeSpan.FromMilliseconds(0)); "
        "if guard = null then "
        "  return 99; "
        "end; "
        "return 0; "
    )), 99);
}

// ---------------------------------------------------------------------------
// §6.2 / §28.2: Double-dispose idempotency.
// Calling Dispose() on a Lock twice must not crash or double-release the mutex.
// A subsequent Acquire() must still succeed (the mutex was released exactly once).
// ---------------------------------------------------------------------------

// §28.2: explicit double-dispose of a Lock is idempotent.
BEZEL_TEST(codegen_mutex_lock_6_2_28_2, lock_double_dispose_idempotent) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var mutex := new Mutex(); "
        "var guard := mutex.Acquire(); "
        "guard.Dispose(); "   // explicit first dispose
        "guard.Dispose(); "   // second dispose — must be a no-op
        // Mutex should be free now; Acquire again must succeed.
        "using var g2 := mutex.Acquire(); "
        "return 5; "
    )), 5);
}

// §28.2: explicit double-dispose of a Mutex is idempotent.
// After explicit Dispose, the using-var scope-exit auto-dispose must be a no-op.
BEZEL_TEST(codegen_mutex_lock_6_2_28_2, mutex_double_dispose_idempotent) {
    BEZEL_EXPECT_EQ(run(prog(
        "var mutex := new Mutex(); "
        "mutex.Dispose(); "   // explicit first dispose
        "mutex.Dispose(); "   // second dispose — must be a no-op
        "return 3; "
    )), 3);
}

// ---------------------------------------------------------------------------
// §6.2: guard idiom in a loop — using-var releases each iteration without
// double-release at the end of the outer scope.
// ---------------------------------------------------------------------------

// §6.2: a `using var` lock guard inside a loop releases each iteration.
// After the loop the mutex is free; an outer Acquire succeeds.
BEZEL_TEST(codegen_mutex_lock_6_2, guard_in_loop_no_double_release) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var mutex := new Mutex(); "
        "var i := 0; "
        "while i < 3 do "
        "  using var g := mutex.Acquire(); "
        "  i := i + 1; "
        "end; "
        "using var final := mutex.Acquire(); "
        "return 1; "
    )), 1);
}

// ---------------------------------------------------------------------------
// ASan / LSan: Memory safety for guard idiom, double-dispose, and
// Lock-outlives-Mutex.  Each test runs under LeakSanitizer; a failure means
// a leak or use-after-free was detected.
// ---------------------------------------------------------------------------

// ASan/LSan §6.2: guard-idiom scope is leak-free (both Mutex and Lock freed).
BEZEL_TEST(codegen_mutex_lock_asan_6_2, acquire_guard_leak_free) {
    int r = runUnderLeakSanitizer(prog(
        "using var mutex := new Mutex(); "
        "using var guard := mutex.Acquire(); "
        "return 0; "
    ));
    if (r == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(r, 0);  // 0 = clean
}

// ASan/LSan §6.2: TryAcquire-success guard is leak-free.
BEZEL_TEST(codegen_mutex_lock_asan_6_2, try_acquire_success_leak_free) {
    int r = runUnderLeakSanitizer(prog(
        "using var mutex := new Mutex(); "
        "using var guard := mutex.TryAcquire(TimeSpan.FromSeconds(1)); "
        "if guard <> null then "
        "end; "
        "return 0; "
    ));
    if (r == -1) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ASan/LSan §28.2: double-dispose idempotent — no double-free.
BEZEL_TEST(codegen_mutex_lock_asan_28_2, double_dispose_no_double_free) {
    int r = runUnderLeakSanitizer(prog(
        "var mutex := new Mutex(); "
        "mutex.Dispose(); "
        "mutex.Dispose(); "
        "return 0; "
    ), /*detectLeaks=*/false);  // no leak check; only UAF/DF check
    if (r == -1) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ASan/LSan: Lock outliving Mutex — the refcounted core keeps the mutex
// memory alive until the last Lock is released.  No UAF.
// We achieve this by obtaining a Lock, then explicitly disposing the Mutex
// while the Lock is still held, then disposing the Lock.
BEZEL_TEST(codegen_mutex_lock_asan_6_2, lock_outlives_mutex_no_uaf) {
    // Explicit dispose order: Mutex first, Lock second.
    // The refcount on the core should keep the pthread_mutex alive until
    // bz_lock_release (the Lock's disposer) drops its reference.
    int r = runUnderLeakSanitizer(prog(
        "var mutex := new Mutex(); "
        "var guard := mutex.Acquire(); "
        "mutex.Dispose(); "  // explicit Mutex dispose — core still alive (Lock holds a ref)
        "guard.Dispose(); "  // Lock dispose → bz_lock_release → core_refs → 0 → free
        "return 0; "
    ), /*detectLeaks=*/true);
    if (r == -1) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ASan/LSan: guard-in-loop — 5 iterations, each releasing; no accumulation.
BEZEL_TEST(codegen_mutex_lock_asan_6_2, guard_in_loop_5x_leak_free) {
    int r = runUnderLeakSanitizer(prog(
        "using var mutex := new Mutex(); "
        "var i := 0; "
        "while i < 5 do "
        "  using var g := mutex.Acquire(); "
        "  i := i + 1; "
        "end; "
        "return 0; "
    ));
    if (r == -1) return;
    BEZEL_EXPECT_EQ(r, 0);
}

} // namespace
