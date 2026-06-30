// Direct unit tests for the events/Subscription C runtime (language spec §24,
// stdlib §2.11). These exercise bz_event_* / bz_subscription_* directly rather
// than through the JIT, so they can assert behaviors the codegen relies on but
// the JIT harness cannot easily observe — in particular that disposing a
// Subscription is idempotent AND frees the node (issue #372: the node was
// previously leaked because Dispose unsubscribed but never free()'d it).
//
// The runtime symbols are linked into bezel_tests via --whole-archive (see
// tests/CMakeLists.txt), so these functions resolve at link time.
//
// OWNERSHIP CONTRACT (issue #841): a subscribed handler closure is a
// transferred +1 reference — the event source owns it and bz_subscription_dispose
// RELEASES it on unsubscribe (rc 1→0 → freed).  These tests therefore mint REAL
// heap-allocated closure cells matching the runtime layout
//   { fn@0, env@8, rc@16, envdtor@24 }  (32 bytes, rc := 1 = the producer ref)
// rather than the bare static-address stand-ins used before #841.  Because the
// source releases on dispose, each subscription needs its OWN cell; reusing one
// across subscribe/dispose rounds would be a use-after-free.  fn/env/envdtor are
// null: a null env makes bz_closure_release free only the cell (free(NULL) for
// the env is a safe no-op), which is exactly the behavior under test.

#include <cstdint>
#include <cstdlib>

#include "runtime/bezel_runtime.h"
#include "test_harness.hpp"

namespace {

// Mint a real, releasable closure cell (rc := 1, the producer's reference).
// bz_subscription_dispose releases it on unsubscribe (#841), so callers hand a
// FRESH cell per subscription. Layout: rc lives at byte offset 16.
void* make_closure() {
    void* c = std::calloc(1, 32);                                  // fn/env/envdtor = null
    *reinterpret_cast<int64_t*>(static_cast<char*>(c) + 16) = 1;   // rc := 1
    return c;
}

} // namespace

// §24.4 / §2.11: Subscribe registers a handler (count goes up); Dispose removes
// it (count goes back down). The handle returned by Subscribe is non-null. The
// handler closure is released on dispose (#841), so the fresh cell here is freed
// exactly once.
BEZEL_TEST(runtime_events_24, subscribe_then_dispose_unsubscribes) {
    void* src = bz_event_source_new();
    void* sub = bz_event_subscribe(src, make_closure());
    BEZEL_EXPECT_TRUE(sub != nullptr);
    BEZEL_EXPECT_EQ(bz_event_count(src), (int64_t)1);

    bz_subscription_dispose(sub);
    BEZEL_EXPECT_EQ(bz_event_count(src), (int64_t)0);

    bz_event_source_free(src);
}

// §2.11 / issue #372: Dispose is idempotent — a SECOND call on the same handle
// is a safe no-op. After #372 the first Dispose free()s the node, so the second
// call must not dereference the freed pointer (no use-after-free, no double
// free). The handler stays unsubscribed (count remains 0) across both calls.
// The closure (#841) is released exactly once: the live-set guard makes the
// second/third dispose a no-op, so the cell is never double-released.
// (Codegen relies on this: an explicit `.Dispose()` plus a `using var` / scope
// exit disposal can hand the same pointer here twice.)
BEZEL_TEST(runtime_events_24, double_dispose_is_safe_noop_372) {
    void* src = bz_event_source_new();
    void* sub = bz_event_subscribe(src, make_closure());
    BEZEL_EXPECT_EQ(bz_event_count(src), (int64_t)1);

    bz_subscription_dispose(sub);            // first: unsubscribe + release closure + free node
    BEZEL_EXPECT_EQ(bz_event_count(src), (int64_t)0);

    bz_subscription_dispose(sub);            // second: must be a safe no-op (no double-release)
    bz_subscription_dispose(sub);            // third: still safe
    BEZEL_EXPECT_EQ(bz_event_count(src), (int64_t)0);

    bz_event_source_free(src);
}

// §2.11: Dispose tolerates a null handle (a Subscription? that was never set).
BEZEL_TEST(runtime_events_24, dispose_null_handle_is_safe) {
    bz_subscription_dispose(nullptr);
    BEZEL_EXPECT_TRUE(true);  // reaching here without crashing is the assertion
}

// §24.4 / issue #372: repeatedly subscribing and disposing must not grow the
// active handler set unboundedly and must not leak nodes. We can't read the
// process heap from the harness, but we CAN assert the active count stays
// bounded (returns to 0 each round) — which, combined with the free() in
// Dispose, is the observable half of "no unbounded growth". Each round mints a
// FRESH closure cell because the prior round's dispose released (freed) it.
BEZEL_TEST(runtime_events_24, repeated_subscribe_dispose_stays_bounded_372) {
    void* src = bz_event_source_new();
    for (int i = 0; i < 1000; ++i) {
        void* sub = bz_event_subscribe(src, make_closure());
        BEZEL_EXPECT_EQ(bz_event_count(src), (int64_t)1);
        bz_subscription_dispose(sub);
        BEZEL_EXPECT_EQ(bz_event_count(src), (int64_t)0);
        bz_subscription_dispose(sub);  // idempotent every round (no double-release)
    }
    bz_event_source_free(src);
}

// §24.4 / §2.11: two independent subscriptions on the same source dispose
// independently — disposing one leaves the other live, and each handle's second
// dispose is a no-op. Exercises the live-set with multiple concurrent nodes.
// Each subscription owns its own closure cell, released on its own dispose.
BEZEL_TEST(runtime_events_24, independent_subscriptions_dispose_independently) {
    void* src = bz_event_source_new();
    void* a = bz_event_subscribe(src, make_closure());
    void* b = bz_event_subscribe(src, make_closure());
    BEZEL_EXPECT_EQ(bz_event_count(src), (int64_t)2);

    bz_subscription_dispose(a);
    BEZEL_EXPECT_EQ(bz_event_count(src), (int64_t)1);  // b still live
    bz_subscription_dispose(a);                          // no-op
    BEZEL_EXPECT_EQ(bz_event_count(src), (int64_t)1);

    bz_subscription_dispose(b);
    BEZEL_EXPECT_EQ(bz_event_count(src), (int64_t)0);

    bz_event_source_free(src);
}
