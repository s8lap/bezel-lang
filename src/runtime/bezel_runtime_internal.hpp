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
#include <unordered_map>
#include <unordered_set>

// ---- allocation failure (spec §31.4, v0.226) ----
//
// §31.4 "Allocation failure" is normative and unambiguous: "When a runtime
// allocation cannot obtain memory — a `new` expression, collection growth,
// string construction, boxing, or the allocation of a closure, iterator, or
// async state-machine frame — the program TERMINATES: the runtime reports the
// failure and aborts the process. Allocation failure is not an exception.
// There is no OutOfMemoryException; nothing is catchable; no unwinding runs …
// The report's form is implementation-defined; the termination is not."
//
// Every allocation-failure site in the runtime funnels through here. The two
// alternatives are both wrong, and both were tried:
//   * returning NULL — callers do not check (and per §31.4 have no reason to),
//     so it becomes a null dereference one frame later;
//   * returning a truncated/partial result — silent data corruption, which is
//     strictly worse than the crash it replaces (a 3-line file read back as
//     1 line is indistinguishable from a 1-line file).
// Terminating here is what the spec requires AND the only posture that cannot
// corrupt: a caller that never observes failure cannot mishandle it.
//
// [[noreturn]] lets callers keep their original shape — no status plumbing, no
// post-call checks, and no "grew or not" branch that could write past a buffer
// that was never grown.
[[noreturn]] inline void bz_alloc_fail(const char* what) {
    // Deliberately no allocation on this path (we are already out of memory):
    // fputs of string literals + the caller's static description only.
    fputs("bezel: fatal: allocation failed (", stderr);
    fputs(what ? what : "unspecified", stderr);
    fputs(") — terminating per spec §31.4\n", stderr);
    fflush(stderr);
    abort();
}

// ---- shared object-layout constants ----
static const int64_t BZ_EXC_OBJ_SIZE = 40;
static const int64_t BZ_CLOSURE_ENV_OFFSET = 8;

// §27.7.1 (v0.180, #1544): byte offset of the function-value KIND tag inside
// the closure cell (after fn/env/rc/envdtor). The kind records which §27.7.1
// equality rule the value obeys, since the runtime cannot otherwise tell a
// lambda from a method/property reference (both are {fn, env, rc, envdtor}
// cells) — and, critically, cannot tell a class-receiver reference (which must
// match by fn + receiver across separate evaluations) from a record-receiver
// reference (which must NOT match across separate evaluations, §27.7.1 third
// bullet). Codegen's closureCellType() places this as field index 4.
static const int64_t BZ_CLOSURE_KIND_OFFSET = 32;
// Kind values (must agree with codegen's FnValKind lowering):
//  0 (default, calloc zero): a lambda OR a value-shaped-receiver (record/enum/
//    primitive) method/property reference. Equality is REFERENCE IDENTITY on
//    the closure cell — the value equals only itself (§27.7.1 first & third
//    bullets). Two separately-constructed lambdas / two separate record-receiver
//    evaluations are therefore NOT equal.
//  1: a class/interface-receiver method/property reference. Equality is
//    same-fn + same-receiver-pointer, so repeated `obj.Method` on the same
//    class-typed `obj` are GUARANTEED equal (§27.7.1 second bullet).
//  2: a static (or unbound-extension) method/property reference. Equality is
//    same-fn: every reference to the same static method is equal (§27.7.1
//    second bullet).
static const int64_t BZ_FVK_REF_IDENTITY = 0;
static const int64_t BZ_FVK_FN_PLUS_RECV  = 1;
static const int64_t BZ_FVK_FN_ONLY       = 2;

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

// fix #3026 (spec §28.2 / §28.9 / §8.1, stdlib §4.1 / §4.6-§4.9, v0.240):
// the runtime-struct kind tag. The three heap collection structs
// (bz_array_t / bz_llist_t / bz_dict_t) share no header, so a polymorphic
// collection-interface receiver (IReadOnlyCollection<T> / ICollection<T> /
// IOwningCollection<T>) whose concrete backing is unknown at the call site
// cannot pick the matching bz_*_free statically — see
// LlvmBackend_Calls2.cpp's `isCollectionInterfaceName` arm. The kind field
// (int64, appended at the END of each struct so existing offsets data@0/
// count@8/capacity@16 stay put) records which struct the pointer holds, so
// the runtime helper bz_runtime_dispose can dispatch on it. Set in each
// struct's alloc fn (calloc leaves it 0; we overwrite to the explicit tag);
// 0 is reserved as "untagged" and an untagged pointer is treated as
// unsound by bz_runtime_dispose (returns without doing anything — same
// trade #2967/#2970 record for diagnostic-untagged positions). Codegen does
// NOT read this field directly: it always emits `bz_runtime_dispose(p,
// dispose_elem)` which reads the kind internally.
constexpr int64_t BZ_RUNTIME_KIND_NONE  = 0;
constexpr int64_t BZ_RUNTIME_KIND_ARRAY = -1;  // fix #3026 — sentinel: see comment
constexpr int64_t BZ_RUNTIME_KIND_LLIST = -2;  //   on bz_runtime_dispose in
constexpr int64_t BZ_RUNTIME_KIND_DICT  = -3;  //   bezel_runtime_collections.cpp

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
    // #2608 / §28.11 / stdlib §4.3: the element disposer of an OWNING array-backed
    // collection, recorded at CONSTRUCTION (bz_array_set_elem_disposer) because
    // the concrete type is known only there. Element ownership is otherwise a
    // purely STATIC property, materialized as bz_array_free's dispose_elem
    // argument; that breaks down when the static type is one of the read-only
    // interfaces SHARED by the borrowing and owning hierarchies (stdlib §4.3:
    // IEnumerable/IReadOnlyCollection/IReadOnlyList are common ancestors of both
    // Array<T> and OwningArray<T>), where the same static type can carry either
    // backing. Only bz_array_free_dynamic reads this; every existing call site
    // still passes its statically-derived disposer to bz_array_free and ignores
    // this field. NULL (calloc-zeroed) means "borrowing / no element disposal",
    // i.e. exactly today's behavior — so an untagged array can never be
    // over-disposed by the dynamic path. APPENDED last: data@0/count@8/capacity@16
    // are read at fixed offsets by codegen and must not move.
    void (*elem_disposer)(void*);
    // fix #3026: kind tag, see comment on the BZ_RUNTIME_KIND_* constants
    // above. APPENDED last so every prior field's offset is preserved
    // (existing codegen reads data@0 / count@8 / capacity@16 at fixed
    // offsets; this field lives at the new end-of-struct offset).
    int64_t kind;
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

    // per §8.3 (v0.204, #2055): per-domain deterministic identity-hash sequence.
    // Monotonic counter incremented on each first-request of an object's hash
    // inside this domain; the assigned value is what bz_obj_identity_hash
    // returns (and stores in the side-table). Cross-run determinism follows
    // from starting at 0 every run; the sequence order is the order in which
    // objects' hashes are first requested within the domain. The
    // startup/shutdown thread (no active domain) draws from a separate global
    // sequence, also in bezel_runtime_refcounting.cpp.
    std::atomic<uint64_t> identity_hash_seq{0};

    ~bz_domain_t() = default;

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
    // fix #3460 (§28.3 / §30.1): reference count for the TASK OBJECT ITSELF,
    // separate from the existing per-`result` ownership (which `result_disposer`
    // / `chained_source` track). A task object is created with refcount == 1
    // (the producer / caller's reference, the one they balance with
    // `bz_task_free`). Codegen's async-method entry additionally calls
    // `bz_task_retain` on `outerTask` immediately after storing it into the
    // heap frame's outerTask field; `emitAsyncComplete` (and the fault-boundary
    // path) calls `bz_task_release` immediately before `free(self)`. That makes
    // the FRAME hold a strong reference to `outerTask` for its entire lifetime,
    // so the worker's resume continuation can read `frame[OUTER_TASK_FIELD]`
    // safely even when the caller's `bz_task_free(outerTask)` from `using var t`
    // scope-end disposal has already run. Pre-#3460 this read was a UAF when
    // the caller's scope end won the race against the worker's resume — the
    // `heap-use-after-free in bz$async$M.C$Fill$Resume` of #3460. The retain
    // is unconditional for every async method (named or lambda), so a task that
    // never suspends (synchronous body) simply retains + immediately releases
    // inside `emitAsyncComplete`, leaving the caller's count undisturbed.
    std::atomic<int32_t> refcount{1};
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
    // §30.5 / #1503: per-T disposer for an UNCONSUMED owned class-typed result.
    // Set via bz_task_set_result_disposer, from two producers:
    //   - Pattern A (Task<T:class>, non-chained): the task always owns its
    //     result directly -- every await/`.Result` only ever borrows it, never
    //     consumes it (§30.5) -- so codegen sets this unconditionally at the
    //     async body's non-chained `return <owned>` site.
    //   - Pattern B (OwningTask<T:class>): codegen sets this at the consumer's
    //     `using var` scope-end drop (§30.5 "Disposing an unconsumed
    //     OwningTask<T>"); a successful consuming await/`.Result` nulls
    //     `result` first (bz_task_take_result), so a CONSUMED Pattern B result
    //     naturally makes this a no-op.
    // bz_task_free consults this field (guarded by !is_error and a non-null
    // `result`) and disposes the result's pointee before freeing the result
    // buffer. Because the field lives on the task struct rather than being
    // used only at the moment of the call, this covers BOTH the
    // already-completed case (dispose runs immediately when bz_task_free is
    // called synchronously) and the still-running case (the disposer fires
    // whenever bz_task_free eventually runs -- via bz_task_fire_and_forget's
    // completion continuation -- satisfying §30.5's "the Dispose() call
    // attaches a continuation that disposes the produced result whenever it
    // appears"). Null for tasks with no ownership obligation on their result
    // (`Task`, `Task<non-class T>`) and for a Pattern B task never dropped
    // unconsumed.
    void (*result_disposer)(void*) = nullptr;
    // §30.13 (#618): owning execution domain (set at allocation; always
    // g_main_domain for current single-domain programs).
    bz_domain_t* domain = nullptr;
    // §30.15 (v0.179, #1524): set (to a bz_channel_t*) only for the task
    // returned by bz_channel_receive — read exclusively by
    // bz_channel_receive_abandon (the scope-end drop of an unconsumed
    // Receive() task) to find the channel a still-parked or racily-completing
    // waiter must deregister from / give an item back to. Opaque void* to
    // avoid a forward-declaration cycle (bz_channel_t is defined further down
    // in bezel_runtime_concurrency.cpp, after this struct). Null for every
    // other task.
    void* recv_channel = nullptr;
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
