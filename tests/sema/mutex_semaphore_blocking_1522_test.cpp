// #1522 / spec §30.12 (v0.179) — blocking Mutex/Semaphore acquisition
// discipline in async/continuation code.
//
// Spec derivation (language spec §30.12, "Blocking acquisition follows the
// §30.3 discipline (v0.179)"):
//   "Mutex.Acquire()/TryAcquire(timeout) and Semaphore.Acquire()/
//    TryAcquire(timeout) block the calling thread. ... The compiler rejects
//    the blocking forms lexically inside `async` methods and `async` lambdas
//    (there the async forms are strictly superior — `await m.AcquireAsync()`
//    / `await m.TryAcquireAsync(timeout)`)."
// Stdlib derivation (§6.2 / §6.3, "Blocking discipline", v0.179):
//   "Acquire() and TryAcquire(timeout) block the calling thread. They are
//    rejected at compile time inside `async` methods and `async` lambdas
//    (use AcquireAsync/TryAcquireAsync); ... They remain fully legal on the
//    startup thread ..., and when the mutex is immediately available."
//
// This generalizes the §30.3/#1362 Task.Wait()/.Result lexical reject
// (tests/codegen/async_sync_wait_941_test.cpp) to Mutex/Semaphore's blocking
// forms. The runtime (would-park) half of the discipline — the non-`async`-
// helper-reached-from-a-continuation case — is exercised end-to-end in
// tests/codegen/mutex_semaphore_domain_guard_1522_test.cpp (this file only
// asserts the STATIC/lexical sema reject, matching #1362's split).
//
// Tests cover:
//   (a) Mutex.Acquire() lexically inside an `async method` — rejected.
//   (b) Mutex.TryAcquire(timeout) lexically inside an `async method` — rejected.
//   (c) Mutex.Acquire() lexically inside an `async` lambda — rejected.
//   (d) Semaphore.Acquire() lexically inside an `async method` — rejected.
//   (e) Semaphore.TryAcquire(timeout) lexically inside an `async method` — rejected.
//   (f) Mutex.Acquire()/TryAcquire() in a plain (non-async) method — accepted
//       (the escape hatch stays legal outside async code, mirroring §30.3's
//       "wait_in_sync_main_still_accepted" precedent).
//   (g) Mutex.AcquireAsync()/TryAcquireAsync() lexically inside an `async
//       method` — accepted (the async forms are exactly what §30.12 asks for).

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// §30.12 diagnostic carries "not available in async code" and cites §30.12.
bool hasBlockingAcquireDiag(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("not available in async code") != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// (a) Mutex.Acquire() lexically inside an `async method` — rejected (§30.12).
// ---------------------------------------------------------------------------
BEZEL_TEST(mutex_semaphore_blocking_1522,
           mutex_acquire_inside_async_method_rejected_30_12) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static async method Go(m: Mutex): Task "
        "    using var guard := m.Acquire(); "   // ERROR: blocking form in async body
        "  end Go; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBlockingAcquireDiag(r));
}

// ---------------------------------------------------------------------------
// (b) Mutex.TryAcquire(timeout) lexically inside an `async method` — rejected.
// ---------------------------------------------------------------------------
BEZEL_TEST(mutex_semaphore_blocking_1522,
           mutex_try_acquire_inside_async_method_rejected_30_12) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static async method Go(m: Mutex): Task "
        "    using var guard := m.TryAcquire(TimeSpan.FromSeconds(1)); "  // ERROR
        "  end Go; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBlockingAcquireDiag(r));
}

// ---------------------------------------------------------------------------
// (c) Mutex.Acquire() lexically inside an `async` lambda — rejected.
// ---------------------------------------------------------------------------
BEZEL_TEST(mutex_semaphore_blocking_1522,
           mutex_acquire_inside_async_lambda_rejected_30_12) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Go(m: Mutex) "
        "    var f := async () => do "
        "      using var guard := m.Acquire(); "   // ERROR: async lambda body
        "    end; "
        "  end Go; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBlockingAcquireDiag(r));
}

// ---------------------------------------------------------------------------
// (d) Semaphore.Acquire() lexically inside an `async method` — rejected.
// ---------------------------------------------------------------------------
BEZEL_TEST(mutex_semaphore_blocking_1522,
           semaphore_acquire_inside_async_method_rejected_30_12) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static async method Go(s: Semaphore): Task "
        "    using var tok := s.Acquire(); "   // ERROR
        "  end Go; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBlockingAcquireDiag(r));
}

// ---------------------------------------------------------------------------
// (e) Semaphore.TryAcquire(timeout) lexically inside an `async method` —
//     rejected.
// ---------------------------------------------------------------------------
BEZEL_TEST(mutex_semaphore_blocking_1522,
           semaphore_try_acquire_inside_async_method_rejected_30_12) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static async method Go(s: Semaphore): Task "
        "    using var tok := s.TryAcquire(TimeSpan.FromSeconds(1)); "  // ERROR
        "  end Go; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBlockingAcquireDiag(r));
}

// ---------------------------------------------------------------------------
// (f) Mutex.Acquire()/TryAcquire() in a plain (non-async) method — STILL
//     ACCEPTED. Stdlib §6.2/§6.3: "They remain fully legal on the startup
//     thread ... and on FFI foreign threads (outside any domain)."
// ---------------------------------------------------------------------------
BEZEL_TEST(mutex_semaphore_blocking_1522,
           mutex_acquire_in_sync_method_still_accepted_30_12) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Go(m: Mutex) "
        "    using var guard := m.Acquire(); "
        "    using var maybe := m.TryAcquire(TimeSpan.FromSeconds(1)); "
        "  end Go; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasBlockingAcquireDiag(r));
}

BEZEL_TEST(mutex_semaphore_blocking_1522,
           semaphore_acquire_in_sync_method_still_accepted_30_12) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Go(s: Semaphore) "
        "    using var tok := s.Acquire(); "
        "    using var maybe := s.TryAcquire(TimeSpan.FromSeconds(1)); "
        "  end Go; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasBlockingAcquireDiag(r));
}

// ---------------------------------------------------------------------------
// (g) Mutex.AcquireAsync()/TryAcquireAsync() lexically inside an `async
//     method` — accepted (the async forms are exactly what §30.12 asks for
//     in place of the blocking forms; must not be caught by the new rule).
// ---------------------------------------------------------------------------
BEZEL_TEST(mutex_semaphore_blocking_1522,
           mutex_acquire_async_inside_async_method_still_accepted_30_12) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static async method Go(m: Mutex): Task "
        "    using var guard := await m.AcquireAsync(); "
        "    using var maybe := await m.TryAcquireAsync(TimeSpan.FromSeconds(1)); "
        "  end Go; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasBlockingAcquireDiag(r));
}
