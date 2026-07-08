#ifndef BEZEL_RUNTIME_INTERNAL_HPP
#define BEZEL_RUNTIME_INTERNAL_HPP
// bezel_runtime_internal.hpp — shared declarations for the per-domain runtime TUs.
// Created by #1414 stage 2 (split of bezel_runtime.cpp). Behavior-preserving:
// the type/constant/inline definitions below are moved verbatim from the old
// single TU; the extern globals + function prototypes promote what were
// file-local statics now referenced across TUs. See the split commit message.

#include "bezel_runtime.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>

// POSIX headers for File I/O, paths, time, threads
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <libgen.h>

// C++ std for the thread-pool implementation
#include <atomic>
#include <algorithm>
#include <cassert>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <thread>
#include <chrono>
#include <functional>
#include <unordered_set>

// ---- shared object-layout constants ----
static const int64_t BZ_EXC_OBJ_SIZE = 40;
static const int64_t BZ_CLOSURE_ENV_OFFSET = 8;

// ---- shared string-header layout constants + rc initializer ----
static const int64_t BZ_STRING_SIZEOF = 32;
// Byte offset of the rc field inside the header (after data/len/width + pad).
static const int64_t BZ_STRING_RC_OFFSET = 24;
// Sentinel refcount for literal/global strings: retain/release are no-ops on it
// so a constant LLVM global is never decremented toward free(). A live heap
// string starts at 1 and could never reach this by retaining (2^63 retains).
static const int64_t BZ_STRING_RC_SENTINEL = 0x7FFFFFFFFFFFFFFFLL;

// Initialize the rc field of a freshly heap-allocated bz_string header to 1.
// The header was calloc'd (rc reads 0); the producer holds the first reference.
static inline void bz_string_init_rc(void* s) {
    if (!s) return;
    int64_t one = 1;
    memcpy((uint8_t*)s + BZ_STRING_RC_OFFSET, &one, sizeof(int64_t));
}

// ---- shared runtime types (used across >1 domain TU) ----
struct bz_array_t {
    void*   data;       // heap buffer holding count*elem_size bytes
    int64_t count;
    int64_t capacity;
    // §28.2 v0.135 / issue #621: checked disposal mode flag. Set to 1 by
    // bz_array_mark_disposed (called instead of bz_array_free in checked builds)
    // so that the indexer guard can detect a use-after-dispose. The struct
    // header is NOT freed in that path (quarantine), keeping this byte valid.
    // Calloc-zeroed on alloc — 0 means live, 1 means disposed.
    int8_t  disposed;
    // §29.12 / issue #1232: modification epoch. Bumped on every structural
    // mutation (Add, Remove, Insert, Clear). Captured by for-in codegen at loop
    // start; checked on each iteration — mismatch → InvalidOperationException.
    int64_t mod_count;
};
typedef struct { char* data; int64_t len; uint8_t width; int64_t rc; } bz_string_hdr;
struct bz_string_struct {
    void*   data;
    int64_t len;
    int8_t  width;
};
struct BzScheduler {
    std::vector<std::thread> threads;
    std::queue<std::function<void()>> queue;
    std::mutex mtx;
    std::condition_variable cv;
    bool shutdown = false;

    void run_worker() {
        for (;;) {
            std::function<void()> work;
            {
                std::unique_lock<std::mutex> lk(mtx);
                cv.wait(lk, [this]{ return !queue.empty() || shutdown; });
                if (shutdown && queue.empty()) return;
                work = std::move(queue.front());
                queue.pop();
            }
            work();
        }
    }

    void submit(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            queue.push(std::move(f));
        }
        cv.notify_one();
    }
};
struct bz_domain_t {
    struct Cont { void (*fn)(void*, void*); void* task; void* state; };

    std::deque<Cont>  queue;          // ready-to-run continuations
    std::mutex        mtx;            // guards queue and active
    bool              active = false; // true while a pool worker is draining
    // per fix #632: refcount — every holder acquires +1 and releases -1;
    // last release (refs==1→0) deletes.  Starts at 0; callers add their refs.
    std::atomic<int>  refs{0};

    // per §6.8 / #652: domain name (stdlib Domain.Name property).
    // Stored as a malloc'd C string; empty string "" if no name was given.
    // Freed in the destructor when the last ref is released.
    char* name = nullptr;

    ~bz_domain_t() {
        free(name);
        name = nullptr;
    }

    // Enqueue a continuation.  If the domain is idle, mark it active and
    // return true so the caller submits a drain to the pool.  If already
    // active, just enqueue and return false — the active runner picks it up.
    bool enqueue_and_claim(Cont c) {
        std::lock_guard<std::mutex> lk(mtx);
        queue.push_back(c);
        if (!active) {
            active = true;
            return true;   // caller must submit domain_drain to pool
        }
        return false;      // active runner will pick up the new item
    }
};
struct bz_task_t {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool is_error = false;
    // §30.5 (v0.162 / #1352): OwningTask single-consumer affine tracking. Set to
    // true after the FIRST await consumes the task (regardless of whether the await
    // succeeded or faulted). A second await on a spent OwningTask is diagnosed at
    // compile time for the straight-line case (ownership pass); for the general
    // cross-method / cross-catch case the codegen reads is_spent at the await site
    // and raises InvalidOperationException if it is already set. Plain Task<T> (not
    // OwningTask<T>) never sets this flag — it can be awaited multiple times.
    bool is_spent = false;
    // §30.10 / #1529: true for a WhenAll/WhenAll<T> AGGREGATE task whose fault
    // was propagated from an input task's OWNED exception — a BORROWED
    // reference, per spec §30.10 "an aggregate task owns no fault of its own".
    // When set, this task's own disposal (scope-end, await/`.Result` fault
    // hand-off) must never reclaim error_obj: the input task remains its sole
    // owner and disposes it at its own, independent scope end (the input
    // tasks are required to outlive the aggregate's settlement and
    // consumption, per the same section). Always false for an ordinarily-
    // faulted task, which does own its exception (§30.9).
    bool fault_borrowed = false;
    char* error_msg = nullptr;
    // §30.9: the ORIGINAL exception object that faulted this task (a Bezel heap
    // object with a leading vtable word). Preserved across the task boundary so
    // an awaiting `try`/`catch` can re-raise it into the handler with the real
    // type (emitIsA needs the object, not just a message). Null when the fault
    // carries only a message (e.g. a runtime-builtin error string). The task
    // does NOT own this object's lifetime: a Bezel exception's ownership
    // transfers to whichever handler catches it (or the unhandled boundary), so
    // bz_task_free must not free it.
    void* error_obj = nullptr;
    void* result = nullptr;
    int64_t result_size = 0;
    // §30.5 / #1332 (v0.162): Pattern A chained slot. When an async method does
    // `return await <Pattern A source>`, the source task transfers into this field
    // so that the outer task keeps the source alive (and its result buffer valid)
    // for as long as the outer task exists. bz_task_free disposes the chain
    // transitively. Null for tasks that own their result directly (most tasks).
    void* chained_source = nullptr;
    // §30.13 (#618): owning execution domain (set at allocation; always
    // g_main_domain for current single-domain programs).
    bz_domain_t* domain = nullptr;
    // Continuation chain (registered by bz_task_then before the task completes)
    struct Cont { void (*fn)(void*, void*); void* state; };
    std::vector<Cont> continuations;
};
struct bz_event_source_t;  // full def in bezel_runtime_events.cpp
struct bz_cancel_state_t {
    std::atomic<int32_t> cancelled;  // 0 = live, 1 = cancelled; one-way latch
    std::atomic<int32_t> rc;         // reference count (source + outstanding tokens)
    // v0.173 / #810 (stdlib §6.1 / spec §30.8): linked cancellation composition.
    // A source created by CreateLinked / CreateLinkedAll registers its state as a
    // CHILD on each parent token's state.  `children` lists the child states this
    // state must cancel when IT cancels (downward cascade); `parents` lists the
    // parent states this state is registered on (kept so the child can UNREGISTER
    // itself at dispose, before it signals its own tokens — the UAF-safe order).
    // Every pointer in either list is a RETAINED reference (bz_cancel_link): a
    // parent keeps each child alive while it may still cascade into it, and a
    // child keeps each parent alive so it can find-and-remove itself at dispose.
    // Both lists are guarded by the file-global g_cancel_link_mu (below), which
    // serialises registration and unregistration against cascade firing so a
    // parent's cascade never writes into a freed child.
    std::vector<bz_cancel_state_t*> children;
    std::vector<bz_cancel_state_t*> parents;
    // #1413 (stdlib §6.1): CancellationToken.Register callback list.  A callback is
    // a Bezel `() -> ()` closure cell (runtime-managed, §28.9 refcounted): Register
    // stores it with the transferred +1, a cancel FIRES it (calls fn(env)) exactly
    // once and RELEASES the +1, and Subscription.Dispose unregisters it (releasing
    // the +1) if it has not yet fired.  This is the closure-list analogue of the
    // event machinery's bz_event_source_t handler list, but guarded by the same
    // g_cancel_link_mu as the cascade lists so the list is mutated under the lock
    // and FIRED via snapshot-and-drain OUTSIDE the lock (the cascade's discipline).
    struct callback_t { void* closure; int64_t token; };
    std::vector<callback_t> callbacks;
    int64_t next_cb_token{0};        // monotonic Subscription token (>0)
    // #1413: CancelAfter / WithCancelAfter reschedule + cancel-on-dispose generation.
    // Each CancelAfter bumps this and captures the new value in its BzTimer entry; at
    // fire time only the entry whose captured generation still matches actually
    // signals — a rescheduled (CancelAfter-twice → later-call-wins) or disposed
    // source's stale timer entry no-ops.  It lives on the STATE cell (not the source)
    // because the timer entry retains the state cell, so it stays readable after the
    // source is freed — no separate shared owner needed.
    std::atomic<uint64_t> timer_gen{0};
};
struct bz_subscription_t {
    bz_event_source_t* src;          // event source (null for a Register callback sub)
    int64_t            token;
    bool               disposed;
    // #1413: non-null → a CancellationToken.Register subscription.  Dispose routes
    // to bz_ctoken_unregister(cancel_state, token) instead of the event path.  The
    // subscription RETAINS this cell (bz_cancel_state_retain) so Dispose can find
    // and remove the callback even after the source itself is gone; it is released
    // here at Dispose.  calloc/zero-init leaves it null for every event Subscribe.
    bz_cancel_state_t* cancel_state;
};

// ---- shared decimal accessors ----
// Helper: extract the unsigned 96-bit mantissa as __uint128_t.
static inline __uint128_t dec_mantissa(const bz_decimal_t* d) {
    return ((__uint128_t)d->hi32 << 64) |
           ((__uint128_t)d->mid32 << 32) |
           (__uint128_t)d->lo32;
}

// Helper: get scale (0..28) from flags.
static inline uint32_t dec_scale(const bz_decimal_t* d) {
    return (d->flags >> 16) & 0xFF;
}

// Helper: get sign bit.
static inline int dec_sign(const bz_decimal_t* d) {
    return (d->flags >> 31) & 1;
}

// ---- promoted globals (defined in bezel_runtime_concurrency.cpp) ----
extern BzScheduler* g_scheduler;
extern bz_domain_t* g_main_domain;
extern thread_local bz_domain_t* g_current_domain;

// ---- promoted file-local helpers (defs stay in their home TU) ----
void  bz_set_exc(void* vtable);                               // decimal_guid
void* bz_make_string(const char* buf, int64_t len);          // time
void  domain_ref(bz_domain_t* dom);                          // concurrency
bz_domain_t* bz_main_domain_get();                           // concurrency
void  bz_cancel_state_retain(bz_cancel_state_t* s);          // concurrency
void  bz_cancel_state_release(bz_cancel_state_t* s);         // concurrency
void  bz_ctoken_unregister(bz_cancel_state_t* s, int64_t tok); // concurrency
std::mutex& bz_sub_live_mutex();                             // events
std::unordered_set<void*>& bz_sub_live_set();                // events
void* bz_task_make_oce(void* oce_vtable);                    // concurrency

#endif // BEZEL_RUNTIME_INTERNAL_HPP
