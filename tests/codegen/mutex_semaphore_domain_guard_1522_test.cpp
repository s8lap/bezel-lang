// #1522 / spec §30.12 (v0.179) — the RUNTIME half of the blocking Mutex/
// Semaphore acquisition discipline.
//
// Spec derivation (language spec §30.12):
//   "... where static rejection cannot see the call (a non-`async` helper
//    reached from a continuation), the runtime raises
//    `InvalidOperationException` when the acquisition would actually park a
//    thread inside a domain continuation — an immediately-available
//    acquisition, and any acquisition on the startup thread or an FFI
//    foreign thread (both outside any domain), proceeds as before."
// Stdlib derivation (§6.2 / §6.3 "Blocking discipline", v0.179):
//   "... where a non-`async` helper reached from a domain continuation would
//    actually park in them, the runtime raises `InvalidOperationException`
//    instead. They remain fully legal on the startup thread and on FFI
//    foreign threads (outside any domain), and when the mutex is immediately
//    available."
//
// The lexical (sema) half of this discipline is covered separately in
// tests/sema/mutex_semaphore_blocking_1522_test.cpp — a call textually
// inside an `async` method/lambda is always a compile error there, so it can
// never reach codegen. This file exercises the DYNAMIC case sema cannot see:
// a plain (non-async) helper method that calls `Acquire()`/`TryAcquire()`,
// invoked from code that is — at runtime — actually running as a domain
// continuation (i.e. reached after a genuine `await` suspension resumes on a
// pool worker, per §30.13's `g_current_domain` mechanism), mirroring the
// #1362/§30.3 Task.Wait()/.Result guard this generalizes
// (tests/codegen/async_sync_wait_941_test.cpp).
//
// Test shape for the "would park" cases: the mutex/semaphore is pre-acquired
// on the (domain-free) startup thread, so any second acquisition attempt is
// genuinely contended. An async method forces a REAL suspension via
// `await Task.Delay(N)` (N > 0, so the fast/already-done path is not taken)
// and then calls the non-async helper from its resumed continuation, which
// runs on a pool worker with `g_current_domain` set (§30.13's domain_drain).
//
// Tests cover:
//   (a) Mutex: busy + reached from a continuation -> InvalidOperationException.
//   (b) Mutex: immediately available + reached from a continuation -> succeeds
//       (the carve-out: "an immediately-available acquisition ... proceeds
//       as before").
//   (c) Mutex.TryAcquire(timeout): busy + reached from a continuation -> IOE
//       WITHOUT actually waiting out the timeout (a bounded park still pins
//       a worker per §30.12).
//   (d) Semaphore: busy + reached from a continuation -> IOE (mirrors a).
//   (e) Semaphore: immediately available + reached from a continuation ->
//       succeeds (mirrors b).
//   (f) Mutex.Acquire()/TryAcquire() called from a plain (non-async) `Main`
//       — the startup-thread escape hatch stays fully legal (mirrors the
//       §30.3/#1362 "wait_in_sync_main_still_accepted" precedent).

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir1522() {
    return wtScratchDir("wt-1522");
}

int compileAndRun(const std::string& source) {
    auto dir = wtDir1522();
    std::string stem = unique_stem("bezel_1522");
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

void expectValue(const std::string& source, int expected) {
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(source), expected);
}

}  // namespace

// ---------------------------------------------------------------------------
// (a) Mutex busy, reached from a continuation -> InvalidOperationException.
// ---------------------------------------------------------------------------
BEZEL_TEST(mutex_semaphore_domain_guard_1522,
           mutex_acquire_busy_from_continuation_raises_ioe_30_12) {
    expectValue(
        "module M; class C "
        "  public static method AcquireHelper(m: Mutex): int "
        "    using var guard := m.Acquire(); "   // non-async: sema can't see this
        "    return 1; "
        "  end AcquireHelper; "
        "  public static async method RunAsync(m: Mutex): Task<int> "
        "    await Task.Delay(5); "               // genuine suspend -> resumes on pool worker
        "    return C.AcquireHelper(m); "         // reached FROM a domain continuation
        "  end RunAsync; "
        "  public static method Main(): int "
        "    using var m := new Mutex(); "
        "    using var held := m.Acquire(); "     // held on the startup thread -> genuinely busy
        "    var r := 0; "
        "    using var t := C.RunAsync(m); "
        "    t.Wait(); "
        "    try "
        "      r := t.Result; "
        "    catch InvalidOperationException => r := 99; "
        "    end; "
        "    return r; "
        "  end Main; end C;",
        99);
}

// ---------------------------------------------------------------------------
// (b) Mutex immediately available, reached from a continuation -> succeeds
//     (no IOE): "an immediately-available acquisition ... proceeds as before."
// ---------------------------------------------------------------------------
BEZEL_TEST(mutex_semaphore_domain_guard_1522,
           mutex_acquire_available_from_continuation_succeeds_30_12) {
    expectValue(
        "module M; class C "
        "  public static method AcquireHelper(m: Mutex): int "
        "    using var guard := m.Acquire(); "
        "    return 7; "
        "  end AcquireHelper; "
        "  public static async method RunAsync(m: Mutex): Task<int> "
        "    await Task.Delay(5); "
        "    return C.AcquireHelper(m); "
        "  end RunAsync; "
        "  public static method Main(): int "
        "    using var m := new Mutex(); "        // NOT held -> immediately available
        "    using var t := C.RunAsync(m); "
        "    t.Wait(); "
        "    return t.Result; "
        "  end Main; end C;",
        7);
}

// ---------------------------------------------------------------------------
// (c) Mutex.TryAcquire(timeout) busy, reached from a continuation -> IOE, and
//     WITHOUT waiting out the (long) requested timeout — a bounded park still
//     pins a worker (§30.12), so the guard probes non-blocking rather than
//     honoring the caller's timeout while inside a domain.
// ---------------------------------------------------------------------------
BEZEL_TEST(mutex_semaphore_domain_guard_1522,
           mutex_try_acquire_busy_from_continuation_raises_ioe_fast_30_12) {
    const std::string source =
        "module M; class C "
        "  public static method TryHelper(m: Mutex): int "
        "    using var guard := m.TryAcquire(TimeSpan.FromSeconds(5)); "
        "    if guard <> null then "
        "      return 1; "
        "    end; "
        "    return 2; "
        "  end TryHelper; "
        "  public static async method RunAsync(m: Mutex): Task<int> "
        "    await Task.Delay(5); "
        "    return C.TryHelper(m); "
        "  end RunAsync; "
        "  public static method Main(): int "
        "    using var m := new Mutex(); "
        "    using var held := m.Acquire(); "
        "    var r := 0; "
        "    using var t := C.RunAsync(m); "
        "    t.Wait(); "
        "    try "
        "      r := t.Result; "
        "    catch InvalidOperationException => r := 99; "
        "    end; "
        "    return r; "
        "  end Main; end C;";
    auto start = std::chrono::steady_clock::now();
    BEZEL_EXPECT_EQ(compileAndRun(source), 99);
    auto elapsed = std::chrono::steady_clock::now() - start;
    // The requested timeout is 5s; a correct guard probes non-blocking and
    // must complete in a small fraction of that (generous bound for CI/ASan
    // slowdown — well under the 5s timeout it must NOT wait out).
    BEZEL_EXPECT_TRUE(elapsed < std::chrono::seconds(3));
}

// ---------------------------------------------------------------------------
// (d) Semaphore busy, reached from a continuation -> InvalidOperationException
//     (mirrors a).
// ---------------------------------------------------------------------------
BEZEL_TEST(mutex_semaphore_domain_guard_1522,
           semaphore_acquire_busy_from_continuation_raises_ioe_30_12) {
    expectValue(
        "module M; class C "
        "  public static method AcquireHelper(s: Semaphore): int "
        "    using var tok := s.Acquire(); "
        "    return 1; "
        "  end AcquireHelper; "
        "  public static async method RunAsync(s: Semaphore): Task<int> "
        "    await Task.Delay(5); "
        "    return C.AcquireHelper(s); "
        "  end RunAsync; "
        "  public static method Main(): int "
        "    using var s := new Semaphore(1); "
        "    using var held := s.Acquire(); "     // sole permit held -> genuinely busy
        "    var r := 0; "
        "    using var t := C.RunAsync(s); "
        "    t.Wait(); "
        "    try "
        "      r := t.Result; "
        "    catch InvalidOperationException => r := 99; "
        "    end; "
        "    return r; "
        "  end Main; end C;",
        99);
}

// ---------------------------------------------------------------------------
// (e) Semaphore immediately available, reached from a continuation ->
//     succeeds (mirrors b).
// ---------------------------------------------------------------------------
BEZEL_TEST(mutex_semaphore_domain_guard_1522,
           semaphore_acquire_available_from_continuation_succeeds_30_12) {
    expectValue(
        "module M; class C "
        "  public static method AcquireHelper(s: Semaphore): int "
        "    using var tok := s.Acquire(); "
        "    return 8; "
        "  end AcquireHelper; "
        "  public static async method RunAsync(s: Semaphore): Task<int> "
        "    await Task.Delay(5); "
        "    return C.AcquireHelper(s); "
        "  end RunAsync; "
        "  public static method Main(): int "
        "    using var s := new Semaphore(1); "   // permit free -> immediately available
        "    using var t := C.RunAsync(s); "
        "    t.Wait(); "
        "    return t.Result; "
        "  end Main; end C;",
        8);
}

// ---------------------------------------------------------------------------
// (f) Mutex.Acquire()/TryAcquire() from a plain (non-async) Main — the
//     startup-thread escape hatch stays fully legal (mirrors the
//     §30.3/#1362 "wait_in_sync_main_still_accepted" precedent).
// ---------------------------------------------------------------------------
BEZEL_TEST(mutex_semaphore_domain_guard_1522,
           mutex_acquire_in_sync_main_still_accepted_30_12) {
    expectValue(
        "module M; class C "
        "  public static method Main(): int "
        "    using var m := new Mutex(); "
        "    using var guard := m.Acquire(); "
        "    using var maybe := m.TryAcquire(TimeSpan.FromMilliseconds(1)); "
        "    if maybe <> null then "
        "      return 1; "
        "    end; "
        "    return 0; "
        "  end Main; end C;",
        // maybe is null (mutex still held by `guard`, non-recursive) -> 0.
        0);
}
