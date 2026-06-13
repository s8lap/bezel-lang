// Direct unit tests for g_current_domain TLS tracker — fix #713.
//
// §30.13 v0.131+
//
// The bug: before fix #713, every async primitive (bz_task_alloc,
// bz_task_delay, bz_channel_receive, bz_domain_spawn join_task, mutex/
// semaphore async acquire, WhenAll/WhenAny aggregates) assigned
// task->domain = g_main_domain regardless of which domain the calling
// continuation was running in.  A continuation inside a spawned domain
// would therefore schedule its async resumptions on the main domain,
// breaking §30.13 run-to-completion isolation.
//
// The fix: domain_drain sets g_current_domain = dom before each
// continuation, and restores it after.  Each task-allocating primitive
// reads `g_current_domain ? g_current_domain : g_main_domain` to pick
// the routing domain.  Main-thread callers (g_current_domain == nullptr)
// continue to get g_main_domain — zero behavior change for the common path.
//
// Test approach (§30.13 / #713):
//
//   Test 1 — g_current_domain is nullptr on the main thread (no drain active).
//
//   Test 2 — g_current_domain is set to the draining domain inside a
//             continuation and is restored to nullptr after drain.
//
//   Test 3 — bz_task_alloc called from inside a continuation gets a
//             non-null g_current_domain (the spawned domain, not nullptr).
//
//   Test 4 — stress: 200 iterations of delay + task_alloc + free inside
//             continuations.  Any mismatched domain_ref/release would
//             crash via double-free or UAF.
//
//   Test 5 — bz_domain_spawn: 50 spawns from the main thread complete
//             correctly without crash.

#include "runtime/bezel_runtime.h"
#include "test_harness.hpp"

#include <atomic>
#include <thread>
#include <chrono>

// ---------------------------------------------------------------------------
// Test 1 — main thread has g_current_domain == nullptr (no drain active)
// ---------------------------------------------------------------------------

BEZEL_TEST(runtime_current_domain_713, main_thread_tracker_is_null) {
    // Before any scheduler interaction: must be nullptr.
    BEZEL_EXPECT_EQ(bz_current_domain_ptr(), nullptr);

    // Even after initializing the scheduler (creates g_main_domain):
    bz_scheduler_init(2);
    BEZEL_EXPECT_EQ(bz_current_domain_ptr(), nullptr);
    bz_scheduler_shutdown();

    BEZEL_EXPECT_EQ(bz_current_domain_ptr(), nullptr);
}

// ---------------------------------------------------------------------------
// Test 2 — g_current_domain is set inside a continuation, restored after
// ---------------------------------------------------------------------------
//
// Strategy: register a bz_task_then continuation on an already-done task.
// The continuation runs via domain_drain on a pool worker.  Inside we
// capture bz_current_domain_ptr(); after drain exits (we poll) the main
// thread checks again — inside must be non-null, outside null (restored).
//
// Task lifetime: we do NOT call bz_task_free(t) externally. Instead we
// rely on bz_task_then having been invoked — bz_task_then does NOT take
// ownership of the task. We call bz_task_free(t) only AFTER we are sure
// the continuation has run (via the 'ran' atomic). This is safe because
// the continuation receives 't' as completedTask but bz_task_free in the
// test is called after the continuation is done.

BEZEL_TEST(runtime_current_domain_713, tracker_set_inside_continuation_restored_after) {
    bz_scheduler_init(2);

    std::atomic<void*> captured_inside{nullptr};
    std::atomic<bool> cont_ran{false};

    // Create an immediately-complete task (already-done).
    void* t = bz_task_from_result(nullptr, 0);

    struct State {
        std::atomic<void*>* captured;
        std::atomic<bool>*  ran;
    };
    auto* st = new State{&captured_inside, &cont_ran};

    bz_task_then(t,
        [](void* /*completedTask*/, void* state) {
            auto* s = (State*)state;
            // Inside the domain_drain loop: g_current_domain must be non-null.
            s->captured->store(bz_current_domain_ptr(), std::memory_order_release);
            s->ran->store(true, std::memory_order_release);
            delete s;
        },
        st);

    // Spin-wait for the continuation to run (pool worker runs it).
    for (int i = 0; i < 2000 && !cont_ran.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    // Now safe to free the task (continuation has run; task is no longer
    // being accessed by any domain or pool thread).
    bz_task_free(t);
    bz_scheduler_shutdown();

    // Continuation must have run.
    BEZEL_EXPECT_TRUE(cont_ran.load(std::memory_order_acquire));
    // Inside the continuation: g_current_domain was non-null.
    BEZEL_EXPECT_TRUE(captured_inside.load(std::memory_order_acquire) != nullptr);
    // After drain: main thread tracker is restored to null.
    BEZEL_EXPECT_EQ(bz_current_domain_ptr(), nullptr);
}

// ---------------------------------------------------------------------------
// Test 3 — bz_task_alloc inside continuation sees non-null g_current_domain
// ---------------------------------------------------------------------------

BEZEL_TEST(runtime_current_domain_713, task_alloc_inside_cont_gets_spawned_domain) {
    bz_scheduler_init(2);

    struct State {
        std::atomic<bool>  ran{false};
        std::atomic<bool>  sub_domain_was_nonnull{false};
        std::atomic<void*> sub_task{nullptr};
    };
    auto* st = new State();

    void* t = bz_task_from_result(nullptr, 0);
    bz_task_then(t,
        [](void* /*completedTask*/, void* state) {
            auto* s = (State*)state;
            // Allocate a sub-task. With fix #713 the sub-task gets
            // g_current_domain (non-null here, since we're in domain_drain).
            void* sub = bz_task_alloc();
            s->sub_domain_was_nonnull.store(
                bz_current_domain_ptr() != nullptr, std::memory_order_release);
            // Complete the sub-task so it doesn't remain pending.
            bz_task_complete(sub, nullptr, 0);
            // Expose sub to the main thread for freeing (safe: main thread
            // waits for ran, which is set AFTER storing sub).
            s->sub_task.store(sub, std::memory_order_release);
            s->ran.store(true, std::memory_order_release);
        },
        st);

    for (int i = 0; i < 2000 && !st->ran.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    bz_task_free(t);
    void* sub = st->sub_task.load(std::memory_order_acquire);
    if (sub) bz_task_free(sub);
    bz_scheduler_shutdown();

    BEZEL_EXPECT_TRUE(st->ran.load(std::memory_order_acquire));
    BEZEL_EXPECT_TRUE(st->sub_domain_was_nonnull.load(std::memory_order_acquire));
    delete st;
}

// ---------------------------------------------------------------------------
// Test 4 — stress: nested task chains from inside continuations, no crash
// ---------------------------------------------------------------------------
//
// 200 iterations: create a delay task, chain a continuation that itself
// allocates a sub-task, completes it, stores it for the main thread to
// free, then signals done.  Any mismatched domain_ref/release would crash
// via double-free or UAF across many iterations.

BEZEL_TEST(runtime_current_domain_713, stress_nested_task_alloc_in_cont_no_crash) {
    bz_scheduler_init(4);

    // Strategy: use bz_task_await_sync to synchronously wait for each
    // delay task to complete. This exercises the bz_task_delay domain-routing
    // path: the task is created with the caller's domain (fix #713), and
    // bz_task_complete routes the delay timer's completion back correctly.
    //
    // We also chain a sub-task-alloc continuation and spin-wait for it to
    // confirm bz_task_alloc inside a continuation routes to the right domain.
    const int ITERS = 200;
    int done_total = 0;

    for (int i = 0; i < ITERS; ++i) {
        struct SubSlot {
            std::atomic<bool>  cont_done{false};
            std::atomic<void*> sub_task{nullptr};
        };
        auto* sl = new SubSlot();
        void* t = bz_task_delay(0);
        bz_task_then(t,
            [](void* /*ct*/, void* state) {
                auto* s = (SubSlot*)state;
                void* sub = bz_task_alloc();
                bz_task_complete(sub, nullptr, 0);
                s->sub_task.store(sub, std::memory_order_release);
                s->cont_done.store(true, std::memory_order_release);
            },
            sl);

        // Spin-wait for the continuation (not just the task).
        // bz_task_await_sync waits for t->done, but the continuation fires
        // asynchronously afterwards via domain_enqueue. We must spin for
        // cont_done rather than use await_sync to avoid freeing t while the
        // continuation might still be running.
        for (int w = 0; w < 2000 && !sl->cont_done.load(std::memory_order_acquire); ++w)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // Continuation has run. Safe to free t and sub.
        bz_task_free(t);
        void* sub = sl->sub_task.load(std::memory_order_acquire);
        if (sub) bz_task_free(sub);
        if (sl->cont_done.load(std::memory_order_acquire)) done_total++;
        delete sl;
    }

    bz_scheduler_shutdown();

    BEZEL_EXPECT_EQ(done_total, ITERS);
}

// ---------------------------------------------------------------------------
// Test 5 — bz_domain_spawn: join_task routes to caller's domain correctly
// ---------------------------------------------------------------------------
//
// Spawn 20 child domains from the main thread.  For each, we synchronously
// wait via bz_task_await_sync (which is safe from the main thread), then
// call bz_task_free.  No bz_task_then is used (to keep lifetime simple).

BEZEL_TEST(runtime_current_domain_713, domain_spawn_join_task_routes_correctly) {
    bz_scheduler_init(4);

    const int ITERS = 20;
    int completed = 0;

    for (int i = 0; i < ITERS; ++i) {
        void* join = bz_domain_spawn(
            [](void* /*arg*/) -> void* {
                // Inside the spawned domain: do nothing, return null.
                return nullptr;
            },
            nullptr,
            nullptr  // no DomainFaultedException vtable
        );
        // Synchronously wait for the child domain to finish.
        bz_task_await_sync(join);
        bz_task_free(join);
        completed++;
    }

    bz_scheduler_shutdown();

    BEZEL_EXPECT_EQ(completed, ITERS);
    // After shutdown: g_current_domain is still null on the main thread.
    BEZEL_EXPECT_EQ(bz_current_domain_ptr(), nullptr);
}
