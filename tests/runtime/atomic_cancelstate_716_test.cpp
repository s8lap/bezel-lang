// Direct unit tests for atomic bz_cancel_state_t — fix #716.
//
// §6.1 §28.2 v0.140
//
// The bug: before fix #716, bz_cancel_state_t::rc was a plain int32_t retained
// on the domain thread but released on pool-worker threads (in
// bz_task_delay_cancelable, bz_mutex_acquire_async, bz_semaphore_acquire_async).
// Concurrent ±1 on a non-atomic int32_t is undefined behaviour — it can produce
// lost-decrement (leak), both-see-1 (double-free), or torn-read (UAF).
//
// Similarly, `cancelled` was a plain int32_t written by one thread (the domain
// thread via bz_cts_cancel / bz_cts_dispose) and read concurrently by pool
// workers (post-sleep / post-acquire checks).  Plain cross-thread flag reads are
// data-race UB.
//
// The fix: both fields are now std::atomic<int32_t>.
//   - rc: retain = fetch_add(1, relaxed); release = fetch_sub(1, acq_rel)==1→free.
//   - cancelled: store via release; load via acquire (worker reads).
//   - cancelled init = relaxed (single-threaded bz_cts_alloc call).
//
// Struct size: std::atomic<int32_t> is guaranteed to have the same size as
// int32_t (4 bytes), so sizeof(bz_cancel_state_t) == 8 bytes both before and
// after the fix.  Codegen accesses cancelled/rc ONLY through function call
// wrappers, never via direct GEP/offset, so no ABI break is possible.
//
// Test approach: simulate the domain-retain + concurrent-worker-release pattern
// that the actual scheduler code exhibits.  We cannot call the scheduler
// directly from a runtime unit test (no JIT session), so we exercise the
// retain/release helpers directly from N threads.  Repeating many iterations
// ensures that a racy non-atomic implementation would reliably double-free or
// corrupt the rc in helgrind / TSan / ASAN runs; under the atomic fix the tests
// must pass with no crash.
//
// We also stress `cancelled` cross-thread: one thread stores 1 (cancel), another
// loops reading until it observes the change, then verifies it reads 1.

#include "runtime/bezel_runtime.h"
#include "test_harness.hpp"

#include <atomic>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers to drive bz_cancel_state_t through its public API
// ---------------------------------------------------------------------------

// Allocate a CTS, get the token (borrow), return the cts and token pointers.
// The token is the bz_cancel_state_t* inside the CTS; the CTS owns rc==1.
static std::pair<void*, void*> make_cts_and_token() {
    void* cts = bz_cts_alloc();
    void* tok = bz_cts_token(cts);
    return {cts, tok};
}

// ---------------------------------------------------------------------------
// Test 1: sequential retain + release cycle — no concurrency.
// Verifies the basic rc path is correct (retain bumps, release frees when ==1).
// Exercises: bz_cts_alloc (rc=1), bz_cts_token (borrow), bz_cancel_state_retain
// (via the internal helpers called from bz_task_delay_cancelable path), then
// bz_cts_dispose (drops the CTS rc), and the final bz_cancel_state_release that
// the worker does.
//
// We use the public-API wrappers that are available to the runtime test harness:
// bz_cts_alloc / bz_cts_dispose / bz_ctoken_is_cancelled.
// ---------------------------------------------------------------------------

BEZEL_TEST(runtime_atomic_cancelstate_716, sequential_alloc_cancel_dispose_clean) {
    // 500 iterations of: alloc CTS, cancel it, check token, dispose CTS.
    for (int i = 0; i < 500; ++i) {
        auto [cts, tok] = make_cts_and_token();
        BEZEL_EXPECT_EQ(bz_ctoken_is_cancelled(tok), 0);
        bz_cts_cancel(cts);
        BEZEL_EXPECT_EQ(bz_ctoken_is_cancelled(tok), 1);
        BEZEL_EXPECT_EQ(bz_cts_is_cancelled(cts), 1);
        bz_cts_dispose(cts);
        // No crash → rc freed correctly.
    }
    BEZEL_EXPECT_TRUE(true);
}

// ---------------------------------------------------------------------------
// Test 2: concurrent retain + release — the core of the #716 data race.
//
// Pattern: domain thread does bz_cts_alloc (rc=1), then N "worker" threads each
// do retain + release (simulating the scheduler's retain-before-submit +
// release-after-complete pattern).  The CTS is disposed after all workers
// finish.  With a plain int32_t rc, concurrent fetch_add/fetch_sub races; with
// atomic<int32_t> it must be correct.
//
// If any worker retains AFTER the CTS is disposed (rc==0 → the memory is freed),
// the retain would UAF — but that's not the pattern here: all retains happen
// before the CTS dispose, and all releases are joined before we dispose.  This
// mirrors the scheduler's lifecycle exactly: retain happens synchronously before
// the pool job is submitted, release happens in the job body, and bz_cts_dispose
// is only called from the domain thread after the jobs complete.
// ---------------------------------------------------------------------------

BEZEL_TEST(runtime_atomic_cancelstate_716, concurrent_retain_release_many_workers) {
    const int kIterations = 300;
    const int kWorkers    = 8;

    for (int iter = 0; iter < kIterations; ++iter) {
        void* cts = bz_cts_alloc();
        void* tok = bz_cts_token(cts);  // borrowed state ptr (the bz_cancel_state_t*)

        // Each worker releases one pre-retained reference, simulating the
        // scheduler's pattern: retain happens synchronously before pool
        // submission (domain thread), release happens inside the lambda on
        // a pool worker thread.
        std::atomic<int> start_gate{0};

        // Retain kWorkers times on the domain thread before launching workers
        // (this matches the actual pattern: retain happens synchronously in the
        // submit call, release in the lambda on the pool thread).
        // We retain here so all kWorkers retains happen before any release,
        // giving us kWorkers+1 references at peak (1 from alloc + kWorkers).
        for (int w = 0; w < kWorkers; ++w)
            bz_cts_retain(tok);

        // Now launch workers that each do one release.
        std::vector<std::thread> threads;
        threads.reserve(kWorkers);
        for (int w = 0; w < kWorkers; ++w)
            threads.emplace_back([tok, &start_gate]() {
                start_gate.fetch_add(1, std::memory_order_relaxed);
                while (start_gate.load(std::memory_order_relaxed) < kWorkers) {}
                bz_cts_release(tok);
            });

        for (auto& t : threads) t.join();

        // All kWorkers releases done; rc is back to 1 (the alloc's ref).
        // Dispose drops the last reference → frees the state cell.
        bz_cts_dispose(cts);
        // No crash / ASAN error → atomic rc is correct.
    }
    BEZEL_EXPECT_TRUE(true);
}

// ---------------------------------------------------------------------------
// Test 3: concurrent cancel (write) + is_cancelled (read) across threads.
//
// One thread writes cancelled=1 (via bz_cts_cancel), another loops reading
// until it observes the change.  With a plain int32_t this is a data race;
// with atomic<int32_t> release/acquire it is sequentially consistent for the
// flag value.
// ---------------------------------------------------------------------------

BEZEL_TEST(runtime_atomic_cancelstate_716, concurrent_cancel_write_and_read) {
    const int kIterations = 500;

    for (int iter = 0; iter < kIterations; ++iter) {
        void* cts = bz_cts_alloc();
        void* tok = bz_cts_token(cts);

        std::atomic<bool> go{false};
        std::atomic<int>  observed{-1};

        // Reader thread: waits for the writer to fire, then reads the flag.
        std::thread reader([tok, &go, &observed]() {
            while (!go.load(std::memory_order_acquire)) {}
            // spin-read until we see the cancellation
            int v;
            do { v = bz_ctoken_is_cancelled(tok); } while (v == 0);
            observed.store(v, std::memory_order_relaxed);
        });

        // Writer thread: cancels the source.
        std::thread writer([cts, &go]() {
            go.store(true, std::memory_order_release);
            bz_cts_cancel(cts);
        });

        writer.join();
        reader.join();

        BEZEL_EXPECT_EQ(observed.load(std::memory_order_relaxed), 1);
        bz_cts_dispose(cts);
    }
    BEZEL_EXPECT_TRUE(true);
}

// ---------------------------------------------------------------------------
// Test 4: dispose-then-retain safety — the bz_cts_dispose path.
//
// bz_cts_dispose: sets cancelled=1, drops rc by 1 (the source's ref).
// If outstanding tokens exist (rc > 1), the state cell survives until the last
// token ref is released.  Test: retain N extra refs, dispose the CTS, then
// release the N retained refs from worker threads — the last release frees the
// state cell cleanly.
// ---------------------------------------------------------------------------

BEZEL_TEST(runtime_atomic_cancelstate_716, dispose_with_outstanding_token_refs) {
    const int kIterations = 200;
    const int kTokenRefs  = 6;

    for (int iter = 0; iter < kIterations; ++iter) {
        void* cts = bz_cts_alloc();
        void* tok = bz_cts_token(cts);  // bz_cancel_state_t*, rc=1

        // Simulate kTokenRefs active workers each holding a retain.
        for (int i = 0; i < kTokenRefs; ++i)
            bz_cts_retain(tok);
        // rc is now kTokenRefs+1.

        // Dispose the CTS (drops rc from kTokenRefs+1 to kTokenRefs).
        // State cell still alive.
        bz_cts_dispose(cts);

        // Workers release concurrently; last one frees.
        std::atomic<int> start_gate{0};
        std::vector<std::thread> threads;
        threads.reserve(kTokenRefs);
        for (int i = 0; i < kTokenRefs; ++i)
            threads.emplace_back([tok, &start_gate]() {
                start_gate.fetch_add(1, std::memory_order_relaxed);
                while (start_gate.load(std::memory_order_relaxed) < kTokenRefs) {}
                bz_cts_release(tok);
            });

        for (auto& t : threads) t.join();
        // No crash / double-free → acq_rel fetch_sub correctly frees exactly once.
    }
    BEZEL_EXPECT_TRUE(true);
}
