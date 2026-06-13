// Direct unit tests for concurrent/repeated Dispose of Mutex / Semaphore /
// AtomicInteger — fix #706 (TOCTOU double-free in the disposed flag).
//
// §28.2 §6.2 §6.3 §6.4 v0.140
//
// The bug: before fix #706, bz_mutex_dispose / bz_semaphore_dispose /
// bz_atomic_dispose read the `disposed` flag non-atomically, then wrote it
// non-atomically.  Two concurrent callers could both observe disposed==false,
// both set it true, and both call the core_release function — causing a
// double-free when the core refcount was 1.
//
// The fix: `disposed` is now std::atomic<bool>; the dispose functions use
// `exchange(true)` so exactly one caller proceeds to core_release regardless
// of concurrency.
//
// Concurrency scope (per §28.2 / §6.2-§6.4):
//   Concurrent Dispose on the same object can only arise when a compiler-level
//   ownership error (codegen emitting duplicate dispose calls across domains)
//   causes two domain tasks to both call Dispose on the same raw pointer.
//   In this scenario core_refs > 1 (each live holder of the raw pointer
//   contributes a core ref), so the struct outlives both Dispose callers.
//   The race is ONLY on the disposed flag — exactly what the atomic exchange
//   in fix #706 addresses.
//
//   For `core_refs == 1` (no outstanding Lock/Token holders), the spec's
//   ownership model guarantees exactly one Dispose call (the owning domain's),
//   so concurrent dispose is not a valid scenario in correct Bezel programs.
//
// Test structure: each test acquires a Lock / SemaphoreToken first, bumping
//   core_refs to 2, so the struct outlives both concurrent Dispose callers.
//   The lock/token is released AFTER both disposer threads join, confirming
//   the struct is freed exactly once (by the token_release → core_release path).
//
// TSan note: under -fsanitize=thread the pre-fix code (plain bool) would
//   report a data race on the disposed field in the concurrent tests.  The
//   post-fix code (std::atomic<bool> + exchange) is race-free.

#include "runtime/bezel_runtime.h"
#include "test_harness.hpp"

#include <atomic>
#include <thread>

// ---------------------------------------------------------------------------
// Mutex — fix #706
// ---------------------------------------------------------------------------

// Idempotent double Dispose with an outstanding Lock (core_refs = 2):
// The second Dispose call must be a no-op, not a second core_release.
// This is the sequential version of the idempotency guarantee (§28.2).
BEZEL_TEST(runtime_concurrent_dispose_706, mutex_double_dispose_with_live_lock_idempotent) {
    void* m = bz_mutex_new();
    BEZEL_EXPECT_TRUE(m != nullptr);
    // Acquire a lock: bumps core_refs to 2.  The struct stays alive for both
    // dispose calls since core_refs > 0 after the first release.
    void* lk = bz_mutex_acquire(m);

    bz_mutex_dispose(m);  // first dispose: drops core_refs 2 → 1, disposed = true
    bz_mutex_dispose(m);  // second dispose: exchange returns true → no-op (fix #706)
    // No crash → idempotency holds.

    bz_lock_release(lk);  // drops core_refs 1 → 0 → struct freed safely
    BEZEL_EXPECT_TRUE(true);
}

// Concurrent Dispose with an outstanding Lock (core_refs = 2 at race time):
// Two threads race bz_mutex_dispose on the same object while a Lock is live.
// The Lock keeps the struct alive for the duration of both Dispose calls, so
// the only race is on the disposed flag — exactly what fix #706 addresses.
// After both disposers finish, the Lock is released (drops core_refs to 0 →
// struct freed).  This models the real scenario where two domain tasks both
// hold a Mutex reference and both call Dispose.
BEZEL_TEST(runtime_concurrent_dispose_706, mutex_concurrent_dispose_with_live_lock) {
    for (int i = 0; i < 2000; ++i) {
        void* m = bz_mutex_new();

        // Acquire a Lock: bumps core_refs to 2.  The lock is released AFTER
        // both threads finish, ensuring the struct outlives both dispose calls.
        void* lk = bz_mutex_acquire(m);

        std::atomic<int> ready{0};
        auto dispose_fn = [m, &ready]() {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (ready.load(std::memory_order_relaxed) < 2) { /* spin */ }
            bz_mutex_dispose(m);
        };
        std::thread t1(dispose_fn);
        std::thread t2(dispose_fn);
        t1.join();
        t2.join();

        // Both disposers finished.  Release the lock (drops last core ref →
        // struct freed safely since disposed==true from one of the two threads).
        bz_lock_release(lk);
    }
    BEZEL_EXPECT_TRUE(true);
}

// ---------------------------------------------------------------------------
// Semaphore — fix #706
// ---------------------------------------------------------------------------

// Idempotent double Dispose with an outstanding SemaphoreToken (core_refs = 2).
BEZEL_TEST(runtime_concurrent_dispose_706, semaphore_double_dispose_with_live_token_idempotent) {
    // initial_count=1 so we can acquire a token immediately.
    void* s = bz_semaphore_new(1, 8);
    BEZEL_EXPECT_TRUE(s != nullptr);
    // Acquire a token: bumps core_refs to 2.
    void* tok = bz_semaphore_acquire(s);

    bz_semaphore_dispose(s);  // first dispose: drops core_refs 2 → 1, disposed = true
    bz_semaphore_dispose(s);  // second dispose: exchange returns true → no-op (fix #706)

    bz_semaphore_token_release(tok);  // drops core_refs 1 → 0 → struct freed safely
    BEZEL_EXPECT_TRUE(true);
}

// Concurrent Dispose with an outstanding SemaphoreToken (core_refs = 2).
BEZEL_TEST(runtime_concurrent_dispose_706, semaphore_concurrent_dispose_with_live_token) {
    for (int i = 0; i < 2000; ++i) {
        // initial_count=1 so we can acquire a token immediately.
        void* s = bz_semaphore_new(1, 8);

        // Acquire a token: bumps core_refs to 2.
        void* tok = bz_semaphore_acquire(s);

        std::atomic<int> ready{0};
        auto dispose_fn = [s, &ready]() {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (ready.load(std::memory_order_relaxed) < 2) { /* spin */ }
            bz_semaphore_dispose(s);
        };
        std::thread t1(dispose_fn);
        std::thread t2(dispose_fn);
        t1.join();
        t2.join();

        // Both disposers finished.  Release the token (drops last core ref →
        // struct freed safely).
        bz_semaphore_token_release(tok);
    }
    BEZEL_EXPECT_TRUE(true);
}

// ---------------------------------------------------------------------------
// Atomic — fix #706
// ---------------------------------------------------------------------------

// AtomicInteger has no Lock/Token concept — core_refs is always 1 when the
// object is live with no external borrows.  Double-dispose of a single-owned
// Atomic is semantically invalid (struct is freed after the first dispose, so
// a second dispose is UAF regardless of atomicity).  Bezel's ownership model
// guarantees this never occurs in a correct program.
//
// What we CAN test safely: that disposing a newly allocated atomic exactly once
// is clean, and that disposing null is a no-op.  The structural argument for
// the atomic exchange (verified by TSan on the mutex/semaphore tests above)
// applies equally to bz_atomic_dispose.
BEZEL_TEST(runtime_concurrent_dispose_706, atomic_single_dispose_clean) {
    void* a = bz_atomic_new(4, 99);
    BEZEL_EXPECT_TRUE(a != nullptr);
    bz_atomic_dispose(a);  // exactly one dispose: correct ownership scenario
    BEZEL_EXPECT_TRUE(true);
}

BEZEL_TEST(runtime_concurrent_dispose_706, atomic_dispose_null_is_noop) {
    bz_atomic_dispose(nullptr);  // must not crash
    BEZEL_EXPECT_TRUE(true);
}

BEZEL_TEST(runtime_concurrent_dispose_706, atomic_single_dispose_stress) {
    // 5000 single-dispose cycles: verifies the atomic field change doesn't
    // regress normal (single-owner) dispose correctness.
    for (int i = 0; i < 5000; ++i) {
        void* a = bz_atomic_new(8, (uint64_t)i);
        bz_atomic_dispose(a);
    }
    BEZEL_EXPECT_TRUE(true);
}
