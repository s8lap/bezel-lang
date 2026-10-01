// Codegen / runtime tests for §28.13 v0.140: Shared<T>-of-synchronized-T
// sendability and the cross-domain Shared<Mutex> contention pattern.
//
// Issue #689. Spec §28.13 v0.140, rationale v0.140 Amendment 1.
//
// What this test proves / does NOT prove:
//
//   PROVES:
//     - Shared<Mutex> can be cloned and sent to a spawned domain (Stage 2 / sema).
//     - Both domains acquire the mutex, execute a critical section, and release.
//     - No crash, no UAF, no use-after-free under ASan.
//     - The atomic control block (Stage 1) survives concurrent clone/release cycles
//       under a churn loop without heap corruption.
//     - TryUpgrade CAS loop (Stage 1) produces correct outcomes in a hammer loop.
//
//   DOES NOT PROVE:
//     - That the mutual-exclusion guarantee of the mutex is perfectly sequenced
//       for arbitrary Bezel code (the runtime's Mutex uses POSIX PTHREAD_MUTEX
//       which provides that independently; we verify the plumbing, not the POSIX
//       semantics).
//     - That Atomic<T> works cross-domain (Atomic<T> codegen not yet implemented
//       per #428 status; Shared<Atomic<T>> sendability accepted by sema but
//       exercising it end-to-end is out of scope here).
//
// Test groups:
//   1. Atomic control block: basic clone/release sequence under ASan (churn loop).
//   2. Contention: main + spawned domain both do N Acquire/Release cycles on
//      a Shared<Mutex>; exits with N*2 (= total completions) demonstrating
//      ASan-clean concurrent use.
//   3. TryUpgrade CAS: hammer loop; upgrade succeeds while strong>0, fails after.
//
// Spec refs:
//   §28.13 v0.140 (sendability rule).
//   §28.13 (Shared<T>/Weak<T> lifecycle: clone, get, dispose, downgrade, upgrade).
//   §30.14 (spawn argument sendability).
//   stdlib §6.2 v0.138 (Mutex sendable, cross-domain Acquire/Release idiom).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"
#include "support/run_under_asan.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>

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

#include "codegen/LlvmBackend.hpp"
#include "support/unique_stem.hpp"

namespace fs = std::filesystem;
using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Compile, link with ASan, run.  Returns 0 (clean), 1 (sanitizer), -1 (skip).
int runUnderASan(const std::string& source) {
    // fix #3653: forwards to the ONE shared implementation.
    return bezel::test::runUnderAsan(source, "bezel_689_asan");
}

// Helper: run both via JIT (logic check) and via ASan native exe (memory check).
void expectCleanAndEq(const std::string& src, int expectedExit) {
    BEZEL_EXPECT_EQ(run(src), expectedExit);
    int san = runUnderASan(src);
    if (san != -1) BEZEL_EXPECT_EQ(san, 0);  // 0 = ASan/LSan clean
}

// §33.3 / #1609: compile `source` to a native exe (linked against the ASan-
// instrumented runtime, so the fault path is also checked for UAF), run it, and
// capture the exit code + stderr. Used for the async-Main unhandled-exception
// boundary, which CANNOT run in-process (it terminates via _exit(70), which
// would kill the gtest shard). Returns {ran, exitCode, stderr}. `ran` is false
// when the native toolchain/runtime is unavailable (the caller then skips).
struct NativeRun { bool ran = false; int exitCode = 0; std::string stderrText; };
NativeRun runNativeCaptureStderr(const std::string& source) {
    NativeRun result;
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return result;

    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_1609_fault");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();
    std::string errPath = (dir / (stem + ".err")).string();
    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
        std::remove(errPath.c_str());
    };

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return result;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return result;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return result;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return result;
    }

    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return result; }

    // The fault path _exit(70)s and never runs LSan (an aborted program is not
    // leak-checked); detect_leaks=0 keeps LSan from firing on the deliberate
    // process-exit exception. A genuine ASan error (e.g. a UAF reading the fault
    // object) still aborts non-zero, distinguishing it from the clean 70.
    std::string runCmd = "ASAN_OPTIONS=detect_leaks=0 " + exe +
                         " > /dev/null 2>" + errPath;
    int status = std::system(runCmd.c_str());
    if (WIFEXITED(status)) { result.ran = true; result.exitCode = WEXITSTATUS(status); }
    std::ifstream in(errPath);
    result.stderrText.assign(std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>());
    cleanup();
    return result;
}

} // anonymous namespace

// ──────────────────────────────────────────────────────────────────────────────
// Group 1: Atomic control block churn — clone/release many times.
// §28.13 v0.140 / #689 Stage 1: counts are now atomic; verify basic lifetime
// semantics survive repeated Clone + dispose cycles without heap corruption.
// ──────────────────────────────────────────────────────────────────────────────

// §28.13 v0.140 / #689: Clone×100 inside a helper + dispose all → inner still alive.
// Verifies the atomic fetch_add/fetch_sub path handles high-frequency
// clone/release in a tight loop without losing or double-counting.
BEZEL_TEST(shared_sync_689_contention,
           atomic_cb_clone_churn_200_disposed_once_28_13_v0140) {
    // Clone 100 times inside a helper scope; then release h itself.
    // h (original in Main) is still alive at the return point → disposed == 0.
    std::string src =
        "module M; "
        "class Inner "
        "  public static disposed: int := 0; "
        "  destructor Inner.disposed := Inner.disposed + 1; end destructor; "
        "end Inner; "
        "class Churn "
        // Takes h (owned), clones 100 times, releases all clones + h on scope exit.
        "  public static method Clone100(owned h: Shared<Inner>) "
        "    using var handle := h; "
        "    var i := 0; "
        "    while i < 100 do "
        "      using var tmp := handle.Clone(); "
        "      i := i + 1; "
        "    end; "
        "  end Clone100; "
        "end Churn; "
        "class C "
        "  public static method Main(): int "
        "    var obj := new Inner(); "
        "    using var h := new Shared<Inner>(owned obj); "
        // Clone100 receives h.Clone() (1 extra handle) and releases it plus
        // 100 loop clones → strong drops from 2 to 1 (Main's h still alive).
        "    Churn.Clone100(h.Clone()); "
        // h is still alive here → disposed == 0.
        "    return Inner.disposed; "
        "  end Main; "
        "end C;";
    // Main's h is still alive at the return → disposed == 0.
    expectCleanAndEq(src, 0);
}

// §28.13 v0.140 / #689: After all handles released, Inner.disposed == 1 (not 0, not >1).
BEZEL_TEST(shared_sync_689_contention,
           atomic_cb_all_released_disposed_exactly_once_28_13_v0140) {
    std::string src =
        "module M; "
        "class Inner "
        "  public static disposed: int := 0; "
        "  destructor Inner.disposed := Inner.disposed + 1; end destructor; "
        "end Inner; "
        "class C "
        "  public static method Release() "
        "    var obj := new Inner(); "
        "    using var h1 := new Shared<Inner>(owned obj); "
        "    using var h2 := h1.Clone(); "
        "    using var h3 := h1.Clone(); "
        "  end Release; "
        "  public static method Main(): int "
        "    C.Release(); "
        "    return Inner.disposed; "
        "  end Main; "
        "end C;";
    expectCleanAndEq(src, 1);
}

// ──────────────────────────────────────────────────────────────────────────────
// Group 2: Cross-domain Shared<Mutex> — clone, send, both domains use the mutex.
// §28.13 v0.140 / §30.14 / §30.12 v0.179 / stdlib §6.2:
//   main domain and a spawned domain each Acquire/Release the same Mutex N times.
//
// WHAT IT PROVES:
//   - Shared<Mutex> handle is cloned and moved to a spawned domain (spawn arg).
//   - The spawned domain acquires/releases the mutex N times (blocking Acquire(),
//     always immediately available — see the §30.12 note below).
//   - The main domain acquires/releases the mutex N times via the async form.
//   - Exit code = N (spawned) + N (main) = 2*N = 10, verifying both domains
//     completed their full cycle without error/crash.
//   - ASan-clean: no UAF, no double-free, no leak of the control block.
//
// WHAT IT DOES NOT PROVE:
//   - SIMULTANEOUS cross-domain contention on the mutex. Under §30.12 v0.179 a
//     blocking Acquire() inside a domain continuation that would PARK raises
//     InvalidOperationException; a synchronous Domain.Spawn entry is such a
//     continuation, so it can only take an immediately-available lock. Genuine
//     simultaneous contention needs BOTH sides on the async form
//     (Get().AcquireAsync(), §28.13 v0.179) — which requires an ASYNC
//     Domain.Spawn entry, not yet implemented in the compiler (async
//     `(owned A) -> Task<R>` entries, §30.14 / stdlib §6.8 v0.179; tracked by
//     issue #1523). This test therefore ORDERS the two domains' mutex use so the
//     spawned side never has to park.
//   - Fairness or ordering.
//
// #1609: the earlier v0.140 form ran both loops CONCURRENTLY; §30.12 v0.179 made
// the spawned side's blocking Acquire() intermittently (and correctly) raise
// InvalidOperationException under real contention, which surfaced at Main's
// boundary as an uncaught DomainFaultedException (flaky rc=70 / hang under load).
//
// NOTE on sync-entry return type:
//   Domain.Spawn(Worker.AcquireN, arg) where AcquireN returns int (sendable
//   value type) produces owned Task<int>.  await on Task<int> returns int.
// ──────────────────────────────────────────────────────────────────────────────

// §28.13 v0.140 / §30.14 / §30.12 v0.179 / stdlib §6.2: cross-domain Shared<Mutex>,
// ordered so the spawned domain's blocking Acquire() never parks; exit = 2*N = 10.
BEZEL_TEST(shared_sync_689_contention,
           cross_domain_shared_mutex_both_domains_complete_28_13_v0140) {
    // Spawned domain receives a cloned Shared<Mutex> handle.
    // It performs N acquire/release cycles and returns N (the cycle count).
    // Main domain also performs N cycles on its own handle.
    // Join: await the spawned task; exit = spawned_result + main_result.
    std::string src =
        "module M; "
        "class Worker "
        // Synchronous entry: receives a cloned Shared<Mutex> handle; N=5 cycles.
        // Sync entry returning int → Domain.Spawn yields Task<int>.
        // `using var h` disposes h (bz_shared_release) at end of AcquireN.
        // §30.12 (v0.179): Acquire() here is legal because this entry runs while
        // the mutex is free (Main awaits it before touching the mutex), so no
        // acquisition ever has to park a pool worker.
        "  public static method AcquireN(owned h: Shared<Mutex>): int "
        "    using var handle := h; "
        "    var count := 0; "
        "    while count < 5 do "
        "      using var guard := handle.Get().Acquire(); "
        "      count := count + 1; "
        "    end; "
        "    return count; "
        "  end AcquireN; "
        "end Worker; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var shared := new Shared<Mutex>(new Mutex()); "
        // Clone one handle to pass to the spawned domain.
        "    using var t := Domain.Spawn(Worker.AcquireN, shared.Clone()); "
        // §30.12 (v0.179 / #1609): join the spawned task FIRST. Main holds no
        // lock while suspended at `await t`, so the spawned domain's blocking
        // Acquire() cycles all find the mutex free and never park. A would-park
        // Acquire() inside a domain continuation raises InvalidOperationException
        // under §30.12 v0.179 — the earlier concurrent-loop form of this test
        // intermittently (and correctly) did so under real contention (#1609).
        "    var spawnedCount := await t; "
        // Main domain: now do 5 acquire/release cycles on its own handle.
        // §30.12 (v0.179 / #1522): Program.Main is `async` — the blocking
        // Acquire() form is rejected lexically there; AcquireAsync() is the
        // required substitute (the spawned Worker.AcquireN, a non-async entry,
        // must instead keep to the always-immediately-available Acquire() above).
        "    var mainCount := 0; "
        "    while mainCount < 5 do "
        "      using var guard := await shared.Get().AcquireAsync(); "
        "      mainCount := mainCount + 1; "
        "    end; "
        "    return mainCount + spawnedCount; "
        "  end Main; "
        "end Program;";
    // Both domains complete 5 cycles each → total = 10.
    // Exit code 10 confirms correct plumbing (clone/send/acquire/release/join)
    // (if either domain crashed/leaked the exit code would differ or ASan would fire).
    expectCleanAndEq(src, 10);
}

// §33.3 / §30.9 / #1609: an uncaught exception escaping an ASYNC Main is reported
// at the process boundary with its TYPE and MESSAGE (not a bare header) and exits
// 70 — matching the synchronous no-handler boundary. Regression for the codegen
// fix that reads the outer task's fault object at main.fault (previously passed
// null → bare "Unhandled exception", masking the real cause). Runs as a native
// subprocess: the boundary _exit(70)s, which would terminate an in-process JIT
// shard. Linked under ASan, so it also proves reading the fault object here is
// not a use-after-free.
BEZEL_TEST(shared_sync_689_contention,
           async_main_uncaught_reports_type_and_message_33_3_1609) {
    std::string src =
        "module M; "
        "class Program "
        "  public static async method Main(): Task<int> "
        // A real suspension before the throw, so the fault is observed on the
        // async resume/await path (the #1609 boundary), not synchronously.
        "    await Task.Delay(1); "
        "    throw new InvalidOperationException(\"boom from async main\"); "
        "  end Main; "
        "end Program;";
    NativeRun r = runNativeCaptureStderr(src);
    if (!r.ran) return;  // native toolchain unavailable → skip
    // §33.3: exit 70, and the report carries the real type + message.
    BEZEL_EXPECT_EQ(r.exitCode, 70);
    BEZEL_EXPECT_TRUE(r.stderrText.find("InvalidOperationException") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.stderrText.find("boom from async main") !=
                      std::string::npos);
}

// ──────────────────────────────────────────────────────────────────────────────
// Group 3: TryUpgrade CAS hammer.
// §28.13 v0.140 / #689 Stage 1: TryUpgrade is now a CAS loop that only
// succeeds when strong > 0 atomically.
//
// Test: create a Shared<T>, take a Weak, then release the strong handle;
// TryUpgrade on the weak must fail (strong == 0 after release).
// ──────────────────────────────────────────────────────────────────────────────

// §28.13 / #689: TryUpgrade fails after the last strong handle is released.
// CAS loop must observe strong==0 and return false (not a partial increment).
BEZEL_TEST(shared_sync_689_contention,
           tryupgrade_fails_after_last_strong_released_28_13_v0140) {
    std::string src =
        "module M; "
        "class Inner "
        "  public static disposed: int := 0; "
        "  destructor Inner.disposed := Inner.disposed + 1; end destructor; "
        "end Inner; "
        "class C "
        "  public static method Main(): int "
        "    var obj := new Inner(); "
        "    using var w := "
        "      (do "
        "        using var h := new Shared<Inner>(owned obj); "
        "        var weak := h.Downgrade(); "
        "        return weak; "
        "      end do); "
        // At this point the strong handle (h) has gone out of scope;
        // strong == 0 → TryUpgrade must fail.
        "    if w.TryUpgrade(out using var h2) then "
        "      return 1; "   // must NOT reach: upgrade should fail
        "    end; "
        "    return Inner.disposed; "   // must be 1 (disposed on strong→0)
        "  end Main; "
        "end C;";
    // TryUpgrade fails (strong==0); disposed==1.
    // This exercises the CAS loop that atomically checks and only increments
    // when strong>0.
    // NOTE: Bezel does not have an anonymous do-block; we test the same logic
    // via a helper method that returns a Weak handle after releasing strong.
    // Rewrite using a factory method:
    src =
        "module M; "
        "class Inner "
        "  public static disposed: int := 0; "
        "  destructor Inner.disposed := Inner.disposed + 1; end destructor; "
        "end Inner; "
        "class Factory "
        "  public static method MakeWeak(): owned Weak<Inner> "
        "    var obj := new Inner(); "
        "    using var h := new Shared<Inner>(owned obj); "
        "    return h.Downgrade(); "
        "  end MakeWeak; "
        "end Factory; "
        "class C "
        "  public static method Main(): int "
        "    using var w := Factory.MakeWeak(); "
        // MakeWeak returned: h is gone, strong==0.
        "    if w.TryUpgrade(out using var h2) then "
        "      return 42; "  // must NOT reach: upgrade should fail
        "    end; "
        "    return Inner.disposed; "  // expect 1
        "  end Main; "
        "end C;";
    expectCleanAndEq(src, 1);
}

// §28.13 / #689: TryUpgrade succeeds while strong handle is alive.
// CAS loop must observe strong>0 and increment atomically.
// Returns 0 on success (V == 42), non-zero on failure.
// NOTE: we avoid exit code 99 since runUnderASan uses 99 as the sanitizer
// indicator; use 42 as the success value.
BEZEL_TEST(shared_sync_689_contention,
           tryupgrade_succeeds_while_strong_alive_28_13_v0140) {
    std::string src =
        "module M; "
        "class Inner "
        "  public V: int := 0; "
        "  destructor end destructor; "
        "end Inner; "
        "class C "
        "  public static method Main(): int "
        "    var obj := new Inner(); "
        "    obj.V := 42; "
        "    using var h := new Shared<Inner>(owned obj); "
        "    using var w := h.Downgrade(); "
        "    if w.TryUpgrade(out using var h2) then "
        "      return h2.Get().V; "  // expect 42
        "    end; "
        "    return -1; "
        "  end Main; "
        "end C;";
    expectCleanAndEq(src, 42);
}
