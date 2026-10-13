// bezel_runtime_concurrency.cpp — task scheduler, timer, domains, cancellation, Channel<T>, Domain.Spawn, TCS, TaskScope.
// Split from bezel_runtime.cpp per #1414 stage 2 (behavior-preserving motion).
#include "bezel_runtime_internal.hpp"

// ---------------------------------------------------------------------------
// Task scheduler — pthread-based thread pool + §30.13 serial-per-domain layer
// ---------------------------------------------------------------------------
//
// §30.13 (v0.131+): every Bezel async continuation belongs to an *execution
// domain*.  Within a domain, at most one continuation runs at any instant
// (run-to-completion semantics): the domain serializes its own work while
// pool threads can still serve different domains concurrently (reserved for
// future Domain.Spawn).
//
// Implementation sketch:
//   bz_domain_t — owns a deque of ready continuations, a mutex, and an
//     `active` flag.  When a continuation is enqueued and the domain is NOT
//     active, the domain is marked active and handed (as a unit) to a pool
//     worker.  The worker drains the deque one continuation at a time under
//     the domain mutex, re-checking under lock after each item; when the
//     deque is empty it clears `active` and releases the domain.  If the
//     domain IS already active when a continuation is enqueued, the enqueue
//     just appends — the active runner will pick it up on its next check.
//     This achieves at-most-one-running-continuation-per-domain with the pool
//     providing the parallelism budget for multi-domain programs.
//
//   g_main_domain — the single domain used by all tasks in current single-
//     domain programs.  All bz_task_alloc() tasks attach to it.
//
//   bz_task_t.domain — pointer to the owning domain (always g_main_domain
//     for now; Domain.Spawn will supply a fresh domain).
//
// Already-done fast path in bz_task_then (§30.10 / #1617):
//   When the task is already settled at bz_task_then time, the continuation is
//   run SYNCHRONOUSLY (inline) rather than deferred through the domain queue.
//   Deferral was the previous design, but it left a use-after-free window: the
//   deferred continuation carried a raw task pointer, while
//   bz_task_fire_and_forget's already-done fast path freed the task struct
//   inline — so a `using var` task disposed without being awaited could be
//   freed while a still-queued WhenAll/WhenAny continuation was pending against
//   it (#1617). Running inline closes the window: the continuation executes
//   while the caller still holds the task alive, before any disposal can race.
//
//   The codegen await path never reaches this branch — emitAwaitSuspend
//   pre-tests bz_task_is_done and takes its own inline fast path, calling
//   bz_task_then only on the suspend (not-done) branch — so the only callers
//   that hit the already-done branch are the runtime's own combinator helpers
//   (WhenAll/WhenAny/WhenAllOwned, TaskScope.Spawn, the async-event raise
//   chain). Those continuations synchronize their own shared state (atomics /
//   per-context mutexes) and do not depend on domain-queue serialization, so
//   running them off the queue is sound. g_current_domain is saved/restored
//   around the inline call (mirroring domain_drain) so nested async allocation
//   routes to the task's own domain.
//
//   The one recursive caller is the async-event raise chain
//   (bz_raise_invoke_next → bz_task_then → bz_raise_chain_step → repeat), which
//   could deepen the C stack once per synchronous handler. g_task_then_depth
//   bounds that recursion: once it reaches the cap, the continuation falls back
//   to domain_enqueue (the old deferred path). That fallback is safe for the
//   only chain that reaches the cap, because the raise-chain context owns each
//   handler task and frees it in the chain step itself — there is no concurrent
//   fire-and-forget to race the deferred continuation. The combinator helpers
//   never recurse through bz_task_then, so they always take the inline path
//   regardless of input count.
//
// bz_task_await_sync (blocking sync-wait for async Main):
//   The main OS thread calls this to block until the top-level Task completes.
//   It parks on the task's condition variable; pool workers running domain
//   drains eventually complete the task (via bz_task_complete → task_notify),
//   which calls notify_all on the task cv, waking the main thread.  The main
//   thread is NOT a domain runner — it holds no domain mutex while waiting —
//   so this cannot deadlock.

// Forward declaration: bz_domain_t drain uses bz_task_t::Cont signature.
struct bz_domain_t;
struct bz_task_t;
static void domain_enqueue(bz_domain_t* dom,
                           void (*fn)(void*, void*), void* task, void* state);

// Forward declaration for the §30.15 Channel<T> core (fully defined much
// further down in this file, after bz_task_free). bz_task_free needs to
// release a channel-receive task's core ref (see bz_task_t::recv_channel,
// #1524) without requiring bz_channel_t's complete type at this point.
struct bz_channel_t;
static void channel_core_release(bz_channel_t* ch);

// ---------------------------------------------------------------------------
// bz_domain_t — serial execution unit (§30.13 / #618)
// ---------------------------------------------------------------------------
//
// Lifetime via reference counting (fix #632 — UAF in domain_drain):
//
//   Every C++ holder that can dereference a bz_domain_t* acquires +1 on
//   the refcount and releases -1 when done; the last release (refs → 0)
//   deletes the domain.
//
//   Holders and their acquire / release sites (per fix #632):
//     - drain active: domain_drain acquires +1 at entry, releases -1 after
//       clearing active=false and returning — so the domain is live for the
//       entire drain loop even if a continuation calls bz_domain_spawn and
//       the child (or its SpawnCtx) completes instantly.
//     - SpawnCtx: bz_domain_spawn acquires +1 for the SpawnCtx; entry_cont
//       releases -1 after completing the join task (before deleting SpawnCtx).
//     - task->domain: each bz_task_t that stores a domain pointer holds one
//       ref (acquired when task->domain is written, released in bz_task_free).
//
//   g_main_domain is exempt from deletion: it receives a permanent +1 at
//   creation so its count never reaches 0 in normal operation.
//   bz_scheduler_shutdown deletes it directly after the pool is torn down.


bz_domain_t* g_main_domain = nullptr;

// fix #713: track the domain currently being drained on this thread so that
// async primitives invoked from inside a spawned domain route their
// await-continuations back to the caller's domain rather than always to
// g_main_domain (§30.13 run-to-completion isolation).
//
// Invariant: g_current_domain is set to the draining domain immediately before
// each c.fn(c.task, c.state) call in domain_drain, and restored afterwards
// (save/restore for correct nesting, mirroring bz_exc's save/restore in
// bz_domain_spawn).  On the main thread (not draining any domain) it is nullptr;
// the nullptr fallback at each allocation site means main-thread callers still
// get g_main_domain — zero behavior change for the common path.
thread_local bz_domain_t* g_current_domain = nullptr;

// #1617 / §30.10: per-thread inline-recursion depth for bz_task_then's
// already-done fast path. Bounds C-stack growth for the only recursive caller
// (the async-event raise chain over synchronous handlers); see the fast-path
// comment above. Combinator helpers (WhenAll et al.) never recurse through
// bz_task_then, so they always take the inline path regardless of this depth.
thread_local int g_task_then_depth = 0;

// #1553 / §22.13 (v0.181): a stable, non-zero per-OS-thread identifier used by
// the static-initialization re-entrancy backstop (emitEnsureInitFns) to
// distinguish a SAME-thread re-entrant touch of an in-progress initializer —
// which must raise a runtime-synthesized TypeInitializationException — from a
// cross-domain touch on a different pool thread, which is the §22.12 rule-1
// blocking case (bz_static_init_wait below, #1391) and is NOT synthesized
// here. The id is drawn from a process-global atomic counter on first use per
// thread, so it is unique per live thread and never 0 (0 marks "no owner
// recorded" in the per-type owner-thread global).
extern "C" uint64_t bz_current_thread_id() {
    static std::atomic<uint64_t> next{1};
    static thread_local uint64_t id =
        next.fetch_add(1, std::memory_order_relaxed);
    return id;
}

// fix #4486 / §22.13, v0.306: the runtime backstop's own text — "the runtime
// attributes each touch to the innermost static initialization in progress
// on the thread; a touch of T is re-entrant when that innermost
// initialization belongs to a different type ... and is ordinary execution
// when it is T's own" — needs a per-thread signal that tells a same-type
// forward-reference escape (T's own init, via a function value / virtual
// dispatch / module boundary, reads T's own later field) apart from genuine
// cross-type re-entrancy (the T -> U -> T signature).
//
// A per-type "currently executing" marker does NOT work: by the time the
// offending read happens, the INTERVENING type U's own sfinit/cctor has
// typically already RUN TO COMPLETION — §22.13 itself requires this ("all of
// [U's] static fields are initialized before any of its methods can run on
// behalf of an outside observer"), so U's state is DONE, not in-progress,
// at the read. (Measured: a save/restore-around-sfinit design, tried first,
// mis-classified static_init_cycle_backstop_1553_test.cpp's own cross-type
// kIntCycle/kStrCycle programs as same-type, because B.GetX()'s static
// method body — which calls the escaping lambda — runs strictly AFTER B's
// sfinit has returned and popped back to A.)
//
// The signal that DOES survive past U's completion is INTERLEAVING: a
// thread-local MONOTONIC epoch, bumped on every type's CAS-win (fresh
// 0->1 transition to in-progress) on this thread. Each type snapshots the
// POST-bump epoch at its own CAS-win (bz$T$initepoch, emitEnsureInitFns,
// LlvmBackend_Setup.cpp). At a read of a not-yet-assigned field of OWNER:
// comparing the CURRENT epoch against OWNER's own snapshot tells whether
// ANY other type's init has EVER started on this thread since OWNER's own
// began (even if that other type has since finished) — equal means none did
// (OWNER never yielded, dynamically, to a different type's own init: a
// same-type escape); greater means at least one did (a T -> U -> ... -> T
// chain: cross-type re-entrancy), regardless of U's CURRENT state.
//
// Known residual (documented, not exercised by any shipped test): if U was
// ALREADY fully initialized before this call chain began (an earlier,
// unrelated touch), calling into U's now-ordinary code does not bump the
// epoch, so a read it triggers back into OWNER is classified same-type. The
// pre-fix behavior for that shape was "always raise TypeInitializationException"
// (unconditionally, for every shape this backstop reaches); this is a strictly
// narrower, spec-aligned classification for the shapes both the existing and
// new regression tests exercise, not a completeness guarantee — matching the
// compile-time approximation's own accepted incompleteness one section up.
extern "C" thread_local uint32_t bz_static_init_epoch = 0;

// Called from the emitted `initB` block (emitEnsureInitFns) at the moment a
// type's CAS wins (0->1): bump this thread's epoch and return the NEW value,
// which the caller stores into that type's own `bz$T$initepoch` snapshot.
extern "C" uint32_t bz_static_init_epoch_bump() {
    return ++bz_static_init_epoch;
}

// Called from emitStaticPartialReadGuard: the CURRENT epoch, no side effect.
extern "C" uint32_t bz_static_init_epoch_get() {
    return bz_static_init_epoch;
}

// ---------------------------------------------------------------------------
// bz_static_init_wait / bz_static_init_notify — §22.12 rule 1 (v0.228) cross-
// domain blocking for lazy static initialization (#1391, the "blocking half";
// the CAS-based double-init/torn-read half landed separately in
// emitEnsureInitFns, LlvmBackend_Setup.cpp).
// ---------------------------------------------------------------------------
//
// §22.12 rule 1: "a type's static initializer runs exactly once process-wide;
// a domain touching a type mid-initialization by another domain blocks until
// initialization completes (or observes the failed state, §22.7)."
//
// emitEnsureInitFns's `tryclaim` block makes the 0(uninit)->1(in-progress)
// transition a single atomic compare-exchange, so at most one thread ever
// becomes a type's initializer. Every OTHER thread whose CAS fails with
// old==1 is routed (in the emitted IR, `chksame`) to one of two cases by
// comparing the CURRENT thread id against the per-type owner-thread global
// the winner recorded in initB:
//   - SAME thread as the owner: the re-entrant self-reference case §22.13
//     documents (a static helper the type's own initializer calls, which
//     touches the type again) — proceeds immediately WITHOUT calling this
//     function. It must not block: the thread IS the initializer, so nothing
//     else could ever complete the initialization and unblock it.
//   - a DIFFERENT thread: a genuine cross-domain touch. This is the only case
//     that reaches bz_static_init_wait.
//
// Design: a process-global registry maps each type's per-type state-global
// ADDRESS (`bz$T$initstate` — stable for the process lifetime, one such
// global per type, emitted once by emitEnsureInitFns) to a lazily-created
// {mutex, condvar} pair, mirroring the existing bz_task_t / condition_variable
// pattern in this file (bz_task_await_sync / bz_task_complete) rather than
// inventing a new primitive. The registry mutex is contended only at a type's
// FIRST cross-domain wait, never while a thread is actually parked; a type
// that is never cross-domain-contended never allocates an entry, and
// notifyFn's registry lookup is a no-op for it (never allocates one just to
// notify). Entries are never freed (a small, growth-bounded — one per
// genuinely-contended type — process-lifetime table, matching this runtime's
// other permanent small registries; still globally reachable, so this is not
// an LSan-visible leak).
//
// Correctness (no lost wakeup): the emitted IR always performs the atomic
// RELEASE store to the state global BEFORE calling bz_static_init_notify (see
// doneB/failB, emitEnsureInitFns). bz_static_init_notify then takes the
// per-type waiter's OWN mutex before calling notify_all, and
// bz_static_init_wait takes that SAME mutex before its predicate-check loop.
// Whichever side acquires the waiter's mutex first, the mutex's own
// lock/unlock happens-before edge (independent of the state global's own
// atomic ordering) guarantees the other side's subsequent state read observes
// the completed transition: either the waiter is already parked in cv.wait()
// (which atomically releases the mutex, so the notifier cannot acquire it and
// call notify_all() without the waiter first being registered to receive it),
// or the notifier's critical section fully precedes the waiter's, in which
// case the waiter's own state re-check under the mutex (not the notify signal
// itself) observes the update and returns without ever calling wait(). This
// is the standard state-plus-condvar-plus-single-mutex pattern used elsewhere
// in this file (bz_task_t::done / bz_task_complete / bz_task_await_sync).
//
// Lock-ordering / deadlock-freedom (verified against domain_drain, above):
// domain_drain runs each continuation (`c.fn(c.task, c.state)`, where
// ensureinit executes when a continuation touches a static) WITHOUT holding
// dom->mtx (see the comment on that call). bz_static_init_wait/notify take
// only the registry mutex (briefly) and a per-type waiter mutex — never a
// bz_domain_t::mtx or BzScheduler::mtx — so this cannot deadlock through lock
// ordering with the scheduler or a domain drain.
//
// A genuine runtime DEADLOCK is still possible here, but only for the case
// §22.13's own text already names and accepts: "an undetected cycle spanning
// two concurrently-initializing domains deadlocks in that blocking rather
// than raising, an accepted bound: no cross-domain waits-for detection is
// promised." Two types whose static initializers touch each other through a
// path the compile-time cycle detector (StaticInitCycles.cpp) cannot see (a
// function value, virtual dispatch, or a module boundary, §22.13 rule 1) and
// that races onto two DIFFERENT threads will block each other forever — this
// is the spec's own documented, accepted bound, not a defect introduced here.
//
// A bounded worker-pool thread that blocks here is unavailable to
// BzScheduler for the wait's duration (its `threads` vector is fixed-size,
// no elastic growth — see run_worker). This can only ever DELAY unrelated
// domain work, not manufacture a NEW deadlock beyond the cycle case above:
// whichever thread actually reaches and wins the CAS for a type keeps making
// progress on its OWN thread without needing a fresh pool worker (static
// initializers cannot `await`, §22.13), so the type genuinely being waited on
// always completes in bounded time absent a cycle, and every blocked waiter
// is woken once it does.
struct StaticInitWaiter {
    std::mutex mtx;
    std::condition_variable cv;
};
static std::mutex g_static_init_registry_mtx;
static std::unordered_map<void*, StaticInitWaiter*> g_static_init_registry;

// Find-or-create the waiter entry for `state_ptr`, fully serialized by
// g_static_init_registry_mtx (no TOCTOU: the lookup, the allocation on first
// miss, and the insertion all happen under the one lock_guard), so concurrent
// first-touches from different threads are guaranteed to converge on the same
// StaticInitWaiter instance.
static StaticInitWaiter* static_init_waiter_for(void* state_ptr) {
    std::lock_guard<std::mutex> lk(g_static_init_registry_mtx);
    StaticInitWaiter*& slot = g_static_init_registry[state_ptr];
    if (!slot) slot = new StaticInitWaiter();
    return slot;
}

// Called from the emitted `crosswait` block (a losing, CROSS-thread CAS,
// old==1) to block until the type's per-type state byte at `state_ptr` (the
// SAME `bz$T$initstate` global address the CAS operated on) reaches done(2)
// or failed(3), returning that terminal value. Never returns 0 or 1.
extern "C" uint8_t bz_static_init_wait(void* state_ptr) {
    auto* state = reinterpret_cast<std::atomic<uint8_t>*>(state_ptr);
    // Fast path: the owner's doneB/failB release store may already have
    // landed between the caller's failed CAS and this call — avoids registry
    // contention entirely on that common race outcome.
    uint8_t s = state->load(std::memory_order_acquire);
    if (s == 2 || s == 3) return s;
    StaticInitWaiter* w = static_init_waiter_for(state_ptr);
    std::unique_lock<std::mutex> lk(w->mtx);
    // Standard condvar predicate loop: re-checks `state` (not just "were we
    // notified") on every wake, including spurious ones — see the no-lost-
    // wakeup argument in this section's header comment.
    while ((s = state->load(std::memory_order_acquire)) == 1) {
        w->cv.wait(lk);
    }
    return s;
}

// Called from the emitted doneB/failB blocks, immediately after the atomic
// RELEASE store that transitions state to done(2) or failed(3) — including on
// the path where the type's initializer THREW (failB runs regardless of
// whether sfinit/cctor completed normally or raised, so a waiter is always
// woken once the failed state is published; see that block's comment) — to
// wake any thread parked in bz_static_init_wait for this type.
extern "C" void bz_static_init_notify(void* state_ptr) {
    StaticInitWaiter* w;
    {
        std::lock_guard<std::mutex> lk(g_static_init_registry_mtx);
        auto it = g_static_init_registry.find(state_ptr);
        if (it == g_static_init_registry.end()) return;  // never contended
        w = it->second;
    }
    std::lock_guard<std::mutex> lk(w->mtx);
    w->cv.notify_all();
}

// ---------------------------------------------------------------------------
// domain_ref / domain_release — refcount helpers (fix #632)
// ---------------------------------------------------------------------------

// Acquire a reference to a domain.  per fix #632.
void domain_ref(bz_domain_t* dom) {
    dom->refs.fetch_add(1, std::memory_order_relaxed);
}

// Release a reference.  per fix #632.
// When the count reaches 0, delete the domain.
// g_main_domain's permanent +1 (from bz_main_domain_get) is released by
// bz_scheduler_shutdown via this same path; the domain is deleted when the
// last outstanding ref (usually the outerTask freed after shutdown) drops to 0.
static void domain_release(bz_domain_t* dom) {
    if (dom->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete dom;
}

// bz_main_domain_get — lazy-init g_main_domain with its permanent +1 ref.
// per fix #632: all lazy-init sites call this instead of bare new bz_domain_t().
// The permanent ref is released by bz_scheduler_shutdown (via domain_release),
// ensuring g_main_domain stays alive until all task refs are also released.
bz_domain_t* bz_main_domain_get() {
    if (!g_main_domain) {
        g_main_domain = new bz_domain_t();
        domain_ref(g_main_domain);  // per fix #632: permanent ref; released at shutdown
    }
    return g_main_domain;
}

// ---------------------------------------------------------------------------
// bz_task_t
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// BzScheduler — POSIX thread pool (workers serve domain drains)
// ---------------------------------------------------------------------------


BzScheduler* g_scheduler = nullptr;

// ---------------------------------------------------------------------------
// BzTimer — dedicated timer thread for Task.Delay (#1378)
//
// Replaces the previous approach of submitting sleep_for jobs to the worker
// pool.  With only 4 pool workers, 4 concurrent delays would occupy all
// workers and block domain work entirely.  A single timer thread owns a
// min-heap of TimerEntry structs sorted by deadline; delays no longer consume
// pool slots.
//
// Protocol:
//   schedule(deadline, callback): insert into min-heap; notify the timer thread
//     only when the new entry is the new earliest deadline (or the heap was
//     empty), so the thread can shorten its wait.
//   run(): wait for a non-empty heap, then wait_until the front deadline.
//     If a new earlier entry arrives (notified), re-examine and re-wait.
//     When the deadline passes, pop all due entries and call their callbacks
//     with the heap mutex released (callbacks invoke bz_task_complete which
//     acquires task->mtx — must not hold heap->mtx simultaneously to avoid
//     lock-ordering cycles).
//   shutdown_and_join(): set stop flag, notify, join.
//
// The timer thread runs with g_current_domain == null, matching the old pool-
// worker behaviour (bz_task_complete does not require a domain context).
//
// Lifetime: g_timer is created lazily on the first bz_task_delay call and
// destroyed in bz_scheduler_shutdown BEFORE domain memory is released.
// ---------------------------------------------------------------------------

struct TimerEntry {
    std::chrono::steady_clock::time_point deadline;
    std::function<void()> callback;
    // min-heap ordering: earlier deadline = higher priority
    bool operator>(const TimerEntry& o) const { return deadline > o.deadline; }
};

struct BzTimer {
    std::priority_queue<TimerEntry,
                        std::vector<TimerEntry>,
                        std::greater<TimerEntry>> heap;
    std::mutex mtx;
    std::condition_variable cv;
    bool stop = false;
    std::thread thread;

    void run() {
        std::unique_lock<std::mutex> lk(mtx);
        for (;;) {
            // Wait until there is at least one entry or stop is set.
            cv.wait(lk, [this]{ return stop || !heap.empty(); });
            if (stop) {
                // Drain all pending entries before exiting.  This matches the
                // old pool-worker behaviour where sleep_for jobs were run to
                // completion during bz_scheduler_shutdown's queue drain: fire-
                // and-forget tasks and using-var tasks register a continuation
                // that calls bz_task_free; if the callback never fires the task
                // struct leaks (detected by LSan, #1378 fix).  We fire
                // callbacks immediately (without waiting for the deadline) for
                // any pending entries.  g_scheduler is still live at this
                // point (timer is shut down BEFORE the pool in bz_scheduler_
                // shutdown), so domain_enqueue -> g_scheduler->submit is safe.
                while (!heap.empty()) {
                    TimerEntry e = heap.top();
                    heap.pop();
                    lk.unlock();
                    e.callback();
                    lk.lock();
                }
                return;
            }

            // Wait until the front deadline passes.  If a new earlier entry is
            // inserted, schedule() calls cv.notify_one() and the predicate
            // becomes true (heap.top().deadline < front), causing early return
            // so we re-examine the new front.  Spurious wakes are harmless.
            auto front = heap.top().deadline;
            cv.wait_until(lk, front, [this, &front]{
                return stop || heap.empty() || heap.top().deadline < front;
            });
            if (stop) continue;  // re-enter loop to drain via the stop path above

            // Pop all entries whose deadline has passed and fire their callbacks.
            // Release the mutex around each callback: the callback calls
            // bz_task_complete (acquires task->mtx), and must not hold heap->mtx
            // at that point to avoid lock-ordering inversion.
            auto now = std::chrono::steady_clock::now();
            while (!heap.empty() && heap.top().deadline <= now) {
                TimerEntry e = heap.top();
                heap.pop();
                lk.unlock();
                e.callback();   // g_current_domain == null, same as old pool workers
                lk.lock();
                now = std::chrono::steady_clock::now();
            }
        }
    }

    // Insert a timer entry.  Notifies the thread only when the new deadline is
    // earlier than the current front (so the thread shortens its wait), or
    // when the heap was empty (thread was blocked in cv.wait).
    void schedule(std::chrono::steady_clock::time_point deadline,
                  std::function<void()> callback) {
        bool needs_notify;
        {
            std::lock_guard<std::mutex> lk(mtx);
            bool was_empty  = heap.empty();
            bool is_earlier = !heap.empty() && deadline < heap.top().deadline;
            heap.push(TimerEntry{deadline, std::move(callback)});
            needs_notify = was_empty || is_earlier;
        }
        if (needs_notify) cv.notify_one();
    }

    // Signal stop and join the thread.  Called from bz_scheduler_shutdown
    // BEFORE domain memory is released.
    void shutdown_and_join() {
        { std::lock_guard<std::mutex> lk(mtx); stop = true; }
        cv.notify_all();
        if (thread.joinable()) thread.join();
    }
};

static BzTimer* g_timer = nullptr;

// Lazily start the timer thread on the first delay request.
// Must be called AFTER bz_scheduler_init (which ensures g_main_domain is set).
// Mutex-guarded (always — the delay path is not hot): the first Task.Delay
// calls can arrive concurrently from continuations of two different domains on
// two pool workers, so a naked check-then-create would race on g_timer.
static std::mutex g_timer_init_mtx;
static void bz_timer_ensure() {
    std::lock_guard<std::mutex> lk(g_timer_init_mtx);
    if (g_timer) return;
    auto* t = new BzTimer();
    t->thread = std::thread([t]{ t->run(); });
    g_timer = t;
}

// ---------------------------------------------------------------------------
// domain_drain — drains one domain's queue on a pool worker (§30.13 / #618)
// ---------------------------------------------------------------------------
//
// Processes continuations one-at-a-time until the queue is empty, then
// atomically clears `active` and returns.  Re-acquires the mutex between
// continuations so late-arriving items (from timer completions or concurrent
// task completions) are picked up without a new pool submission.
//
// Claim-race safety: the check for empty AND the clearing of `active` are
// ONE mutex-held critical section (no gap between them).  A concurrent
// domain_enqueue that sees active=false will atomically push its item and
// set active=true under the SAME mutex, so the item is always eventually
// drained — either by the existing drain (if it hasn't reached empty yet) or
// by the new drain the enqueue submits (if the existing drain already exited).
//
// Memory ordering: every domain_drain exit (active=false + mutex_unlock)
// happens-before any subsequent domain_enqueue claim (mutex_lock + active=true)
// on the same domain.  The mutex provides a sequentially-consistent
// happens-before edge, so a new drain on a different pool worker always sees
// all memory writes from the previous drain's continuations.
//
// Lifetime: domain_drain acquires a refcount on entry and releases on exit,
// per fix #632 (UAF: entry_cont deleted the child_domain then domain_drain
// looped back and re-locked dom->mtx on freed memory).
//
// fix #708 — final release must happen AFTER the lock_guard is destroyed:
// The drain's refcount release (which may delete dom, including dom->mtx) must
// not fire while any lock_guard on dom->mtx is alive.  A `done` flag is set
// under the lock (preserving the active=false-under-lock invariant) and the
// actual domain_release call happens after the lock_guard scope closes.
static void domain_drain(bz_domain_t* dom) {
    domain_ref(dom);  // per fix #632: drain holds a ref for its entire run
    for (;;) {
        bz_domain_t::Cont c;
        bool done = false;
        {
            std::lock_guard<std::mutex> lk(dom->mtx);
            if (dom->queue.empty()) {
                // Atomically clear active under the lock.  Any concurrent
                // enqueue that arrives now must acquire the lock first, will
                // see active=false, will set active=true, and will submit a
                // new drain — so no items are lost.
                dom->active = false;
                done = true;
                // Do NOT call domain_release here — dom->mtx is still locked
                // by this lock_guard.  If this drain holds the last ref,
                // domain_release would delete dom (and dom->mtx); the
                // lock_guard dtor would then unlock a freed mutex (UAF, #708).
                // We break out so the lock_guard destructs first, then release
                // after the mutex is fully unlocked.  per fix #708.
            } else {
                c = dom->queue.front();
                dom->queue.pop_front();
                // c is popped under the lock while active=true.  Any concurrent
                // enqueue sees active=true and pushes to the back of the queue;
                // the next iteration of this loop will pick it up.
            }
        }  // lock_guard destroyed here — dom->mtx is fully unlocked before any
           // domain_release that might delete dom.  per fix #708.
        if (done) {
            domain_release(dom);  // per fix #632: release drain ref on exit
            return;               // safe: dom->mtx is already unlocked above
        }
        // Run the continuation WITHOUT holding the domain mutex so timer
        // completions, WhenAll/WhenAny callbacks, and bz_task_then calls can
        // concurrently enqueue to the domain without blocking.  The invariant
        // active=true holds for the entire duration of c.fn(), so any such
        // concurrent enqueue always appends to the queue (and never triggers
        // a second domain_drain submission).
        //
        // fix #713: set g_current_domain to the draining domain so any async
        // primitive invoked from inside this continuation routes its new task
        // back to this domain (§30.13 run-to-completion isolation).
        // Save/restore for correct nesting (a continuation may itself drain
        // another domain, e.g. via bz_task_await_sync on a sub-task).
        {
            bz_domain_t* prev_cd = g_current_domain;
            g_current_domain = dom;
            c.fn(c.task, c.state);
            g_current_domain = prev_cd;
        }
    }
}

// ---------------------------------------------------------------------------
// domain_enqueue — single path through which all continuations are queued
// ---------------------------------------------------------------------------
//
// Safety argument: no two domain_drain instances can ever be active for the
// same domain simultaneously.
//   - enqueue_and_claim atomically pushes the item AND claims the domain
//     (active=true) under dom->mtx.  It returns true (claim granted) only
//     when active was false; it returns false when active was already true.
//   - domain_drain atomically checks for an empty queue AND clears active
//     (active=false) under dom->mtx.  The only way it exits is through the
//     "queue empty ⇒ active=false" branch, which is entirely under the lock;
//     there is no window between the empty-check and the active-clear where
//     another enqueue could be missed.
//   - Because both enqueue_and_claim and the drain-exit branch of domain_drain
//     are fully serialized by dom->mtx, the transitions active false→true and
//     true→false are mutually exclusive.  At most ONE domain_drain instance
//     holds active=true at any time; the pool worker running that drain is
//     the sole serialized executor for the domain.
//
// The `dom=nullptr` path is a guard against pre-init calls (tasks that are
// allocated before bz_scheduler_init / g_main_domain is set).  In practice
// this MUST NOT occur in correct programs: every task-alloc site that runs
// in a multi-threaded context guards its own `if (!g_main_domain)` init.
// Submitting directly to the pool when dom=null BYPASSES domain serialization
// and is reserved only for the degenerate single-threaded or pre-init case.
static void domain_enqueue(bz_domain_t* dom,
                           void (*fn)(void*, void*), void* task, void* state) {
    if (!dom) {
        // Pre-init / no-domain fallback: no domain serialization available.
        // This path must not fire in a normally-initialized program — every
        // bz_task_alloc / bz_task_delay call ensures g_main_domain is set.
        // Bypass is acceptable only for single-threaded (no pool) usage.
        if (g_scheduler)
            g_scheduler->submit([fn, task, state](){ fn(task, state); });
        else
            fn(task, state);
        return;
    }
    bool need_submit = dom->enqueue_and_claim({fn, task, state});
    if (need_submit && g_scheduler)
        g_scheduler->submit([dom](){ domain_drain(dom); });
    else if (need_submit)
        domain_drain(dom);  // no pool: drain inline (single-threaded fallback)
}

// ---------------------------------------------------------------------------
// Scheduler lifecycle
// ---------------------------------------------------------------------------

void bz_scheduler_init(int num_threads) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    if (g_scheduler) return;
    g_scheduler = new BzScheduler();
    for (int i = 0; i < num_threads; i++)
        g_scheduler->threads.emplace_back([](){ g_scheduler->run_worker(); });
}

void bz_scheduler_shutdown(void) {
    if (!g_scheduler) return;
    // Shut down the timer thread BEFORE the pool workers and BEFORE g_scheduler
    // is deleted.  The timer callback invokes bz_task_complete -> domain_enqueue
    // -> g_scheduler->cv.notify_one().  If the timer thread is still inside
    // notify_one() when bz_scheduler_shutdown deletes g_scheduler (and thus
    // destructs g_scheduler->cv), there is a TSan-detectable use-after-free
    // (#1378 fix).  Joining the timer thread here ensures it has finished all
    // g_scheduler accesses before the BzScheduler is freed.
    if (g_timer) {
        g_timer->shutdown_and_join();
        delete g_timer;
        g_timer = nullptr;
    }
    // Now shut down the pool workers.  Any domain_drain jobs that the timer
    // callbacks enqueued before the timer joined are still in the queue and
    // will be drained by pool workers before they exit (run_worker returns only
    // when shutdown && queue.empty()).
    { std::lock_guard<std::mutex> lk(g_scheduler->mtx); g_scheduler->shutdown = true; }
    g_scheduler->cv.notify_all();
    for (auto& t : g_scheduler->threads) t.join();
    delete g_scheduler;
    g_scheduler = nullptr;
    // per fix #632: release the permanent ref on g_main_domain via domain_release
    // so the domain is deleted when ALL outstanding task refs (+ this permanent ref)
    // are gone.  The generated main calls bz_task_free(outerTask) AFTER shutdown,
    // which releases the outerTask's domain ref — that final release triggers the
    // delete.  Nulling g_main_domain first lets bz_main_domain_get re-init cleanly
    // on the next bz_scheduler_init (process-reuse in JIT test harness).
    bz_domain_t* main_dom = g_main_domain;
    g_main_domain = nullptr;
    if (main_dom) domain_release(main_dom);  // releases permanent ref; may delete
}

void* bz_task_alloc(void) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    auto* task = new bz_task_t();
    // fix #713: route to the caller's domain (g_current_domain when inside a
    // spawned domain drain), falling back to g_main_domain on the main thread.
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      task->domain = d; domain_ref(d); }  // per fix #632: task holds a ref on its domain
    return task;
}

// §30.13 (#618): schedule the initial entry-point continuation for a task (the
// state-machine Resume that starts the async method body).  Route through the
// task's domain so the initial run is also serialized within the domain.
void bz_task_schedule(void* tp, void (*fn)(void* task, void* state), void* state) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    if (!g_scheduler) bz_scheduler_init(4); // lazy init
    auto* task = (bz_task_t*)tp;
    bz_domain_t* dom = task->domain ? task->domain : g_main_domain;
    domain_enqueue(dom, fn, task, state);
}

// stdlib §6.7 / §30.10: Task.Delay(ms).  Allocate a fresh (incomplete) task
// and post a timer entry that fires after `ms` milliseconds then completes it
// with no value.  The task is genuinely incomplete at return, so an
// `await Task.Delay(...)` hits the suspend path (bz_task_then) and resumes
// via the domain queue when the deadline elapses (§30.13 / #618).
//
// Fix #1378: the old implementation submitted a sleep_for job to the 4-worker
// pool.  With 4 concurrent delays the pool was exhausted and domain work
// stalled until timers fired.  The dedicated timer thread (BzTimer / g_timer)
// owns a min-heap of deadlines and fires completions without occupying pool
// workers, so arbitrarily many concurrent delays do not block domain work.
void* bz_task_delay(int64_t milliseconds) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    if (!g_scheduler) bz_scheduler_init(4);   // ensure g_main_domain is set
    bz_timer_ensure();                         // lazily start the timer thread
    auto* task = new bz_task_t();
    // fix #713: route to the caller's domain.
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      task->domain = d; domain_ref(d); }  // per fix #632: task holds a ref on its domain
    int64_t ms = milliseconds < 0 ? 0 : milliseconds;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(ms);
    g_timer->schedule(deadline, [task]() {
        bz_task_complete(task, nullptr, 0);
    });
    return task;
}

// per #700: post-unlock dispatch helper. Receives a pre-snapshotted continuation
// list, domain, and opaque task pointer. The caller has already:
//   (1) set the task's result/error fields and done=true,
//   (2) snapshotted continuations and domain,
//   (3) called cv.notify_all(), and
//   (4) released task->mtx.
// This function ONLY loops domain_enqueue. It must NEVER acquire task->mtx or
// dereference task as bz_task_t* — the task may be freed by a domain worker
// as soon as the first continuation is enqueued. task is passed through as an
// opaque void* to domain_enqueue (which itself treats it as opaque state).
//
// §30.13 (#439 / #618): continuations are routed to the task's DOMAIN queue so
// that run-to-completion serialization is preserved within the domain.
//
// notify_all() is called INSIDE the caller's lock (#445 / §30.5): a
// sync-awaiter (bz_task_await_sync) may call bz_task_free immediately on
// waking, destroying the condition_variable. Holding the mutex during
// notify_all() ensures the awaiter cannot proceed to free the task until AFTER
// notify_all() returns.
static void task_dispatch(bz_domain_t* dom,
                          std::vector<bz_task_t::Cont> conts,
                          void* task_opaque) {
    // task_opaque is passed as opaque state to each continuation; do NOT cast it.
    for (auto& c : conts)
        domain_enqueue(dom, c.fn, task_opaque, c.state);
}

void bz_task_complete(void* tp, const void* result, int64_t result_size) {
    // per #700: snapshot continuations + domain and call notify_all INSIDE the
    // same critical section that sets done=true, eliminating the unlock→re-lock
    // gap that let a concurrent bz_task_then fast-path free the task before
    // task_notify could re-acquire task->mtx.
    auto* task = (bz_task_t*)tp;
    std::vector<bz_task_t::Cont> conts;
    bz_domain_t* dom;
    {
        std::lock_guard<std::mutex> lk(task->mtx);
        if (result_size > 0 && result) {
            task->result = malloc((size_t)result_size);
            memcpy(task->result, result, (size_t)result_size);
            task->result_size = result_size;
        }
        task->done = true;
        conts = task->continuations;
        dom   = task->domain;
        task->cv.notify_all();
    }
    // task may be freed by a woken sync-awaiter or domain worker from this point;
    // do not touch task->mtx or task->cv again.
    task_dispatch(dom, std::move(conts), tp);
}

void bz_task_complete_error(void* tp, const char* error_msg) {
    // per #700: same merged-lock fix as bz_task_complete.
    auto* task = (bz_task_t*)tp;
    std::vector<bz_task_t::Cont> conts;
    bz_domain_t* dom;
    {
        std::lock_guard<std::mutex> lk(task->mtx);
        task->error_msg = strdup(error_msg);
        task->is_error = true;
        task->done = true;
        conts = task->continuations;
        dom   = task->domain;
        task->cv.notify_all();
    }
    task_dispatch(dom, std::move(conts), tp);
}

// §30.9: fault the task with the ORIGINAL exception object (a Bezel heap object).
// The awaiter re-raises this exact object into its enclosing handler so the
// catch arm's type test sees the real exception type. The task does not own the
// object (see bz_task_t::error_obj); ownership stays with the exception itself,
// transferring to the catching handler / unhandled boundary.
void bz_task_complete_error_obj(void* tp, void* error_obj) {
    // per #700: same merged-lock fix as bz_task_complete.
    auto* task = (bz_task_t*)tp;
    std::vector<bz_task_t::Cont> conts;
    bz_domain_t* dom;
    {
        std::lock_guard<std::mutex> lk(task->mtx);
        task->error_obj = error_obj;
        task->is_error = true;
        task->done = true;
        conts = task->continuations;
        dom   = task->domain;
        task->cv.notify_all();
    }
    task_dispatch(dom, std::move(conts), tp);
}

// §30.9: retrieve the original exception object (valid after bz_task_is_error).
// Null when the fault carries only a message.
void* bz_task_error_obj(void* tp) {
    return ((bz_task_t*)tp)->error_obj;
}

// §30.9 / #3030: null the task's error_obj slot. Codegen's scope-end
// fault-disposal preamble (emitTaskDisposal, LlvmBackend_Disposal.cpp) reads
// error_obj via bz_task_error_obj, calls emitDisposeAndReclaim on the result,
// and then calls bz_task_fire_and_forget. Pre-#3030 bz_task_free never read
// error_obj, so leaving the field dangling was fine; post-#3030 the FAST path
// of bz_task_fire_and_forget inspects error_obj to decide whether to dispose
// it (the FAST path is reachable from the explicit `Task.FireAndForget(owned
// t)` user-facing call, which has no codegen preamble), so a non-null
// dangling field would be re-disposed -> double-free. This setter nulls the
// field after codegen's dispose so the FAST path's `if (error_obj && ...)`
// guard sees null and skips. Lock-free: by the time codegen runs the
// preamble the task has settled (the FAST path is the only reachable arm).
void  bz_task_clear_error_obj(void* tp) {
    if (!tp) return;
    ((bz_task_t*)tp)->error_obj = nullptr;
}

// §30.10 / #1529: true if `tp`'s error_obj (if any) is a BORROWED reference
// from an input task's owned fault (set on a WhenAll/WhenAll<T> aggregate that
// propagated an input's fault) rather than owned by `tp` itself. Codegen's
// fault-disposal decisions (scope-end disposal, await/`.Result` fault
// hand-off — see LlvmBackend_Disposal.cpp / LlvmBackend_Async.cpp /
// LlvmBackend_Exprs.cpp) consult this before ever reclaiming error_obj, so a
// borrowing aggregate never double-frees the exception its (still-owning,
// independently-disposed) input task will dispose at its own scope end.
int8_t bz_task_fault_borrowed(void* tp) {
    return ((bz_task_t*)tp)->fault_borrowed ? 1 : 0;
}

// §30.4 / §30.9 (#854): borrowed-exception set. When an `await` of a BOUND
// (multi-awaitable) faulted task re-raises the task's stored error_obj into the
// in-flight exception slot, the catch arm that handles it must NOT reclaim the
// object (it is BORROWED from the task, not owned by the awaiter) — otherwise a
// SECOND `await` of the same faulted task re-reads a freed pointer and crashes.
// Codegen marks the object borrowed at the await-fault hand-off and the
// caught-exception disposal skips a borrowed object. A PER-OBJECT set (not a
// single flag) is required: an arm body may throw/catch a nested NORMAL exception
// between the mark and the borrowed object's own disposal, so membership must be
// keyed by the object pointer. Thread-local because the in-flight exception slot
// (bz_exc) is thread-local (§30.14). Idempotent: mark/unmark of a pointer already
// in/absent from the set is a no-op.
static thread_local std::unordered_set<void*> g_borrowed_exc;

extern "C" void bz_mark_exc_borrowed(void* obj) {
    if (obj) g_borrowed_exc.insert(obj);
}
extern "C" int8_t bz_is_exc_borrowed(void* obj) {
    return (obj && g_borrowed_exc.count(obj)) ? 1 : 0;
}
extern "C" void bz_unmark_exc_borrowed(void* obj) {
    if (obj) g_borrowed_exc.erase(obj);
}

// §30.9 fault ownership (v0.179), in-flight-deferral refinement (a). A faulted
// task OWNS its fault exception and disposes it exactly once when the task is
// disposed — for a `using var`/`var`-bound Task/Task<T> that is at the task
// local's scope-end, for a TCS at the inner-task disposal. But a task disposed
// WHILE its own fault exception is still in flight — being routed to / handled
// by an enclosing catch, as happens when a faulted task bound INSIDE a `try` is
// unwound by its own re-raised fault (#1288 / #1197) — must NOT destroy the
// exception mid-dispatch: the catch arm's type-test and binding would then read
// a freed object (SIGSEGV / double-free). Codegen detects the in-flight case at
// the task's scope-end disposal (the exception is marked BORROWED —
// bz_is_exc_borrowed — because an `await`/`.Result` re-raise put it in flight)
// and, instead of disposing it there, records it here; the disposal is then
// performed as the TERMINAL ACT of exception handling, at the catch arm's normal
// exit (bz_is_exc_deferred → codegen emits the real Dispose + reclaim, then
// bz_clear_exc_deferred). Thread-local like g_borrowed_exc because the in-flight
// exception slot (bz$exc) is thread-local (§30.14).
static thread_local std::unordered_set<void*> g_deferred_exc_dispose;

extern "C" void bz_defer_exc_dispose(void* obj) {
    if (obj) g_deferred_exc_dispose.insert(obj);
}
extern "C" int8_t bz_is_exc_deferred(void* obj) {
    return (obj && g_deferred_exc_dispose.count(obj)) ? 1 : 0;
}
extern "C" void bz_clear_exc_deferred(void* obj) {
    if (obj) g_deferred_exc_dispose.erase(obj);
}

bool bz_task_is_done(void* tp) {
    auto* task = (bz_task_t*)tp;
    std::lock_guard<std::mutex> lk(task->mtx);
    return task->done;
}

bool bz_task_is_error(void* tp) {
    auto* task = (bz_task_t*)tp;
    std::lock_guard<std::mutex> lk(task->mtx);
    return task->is_error;
}

// §30.5 (v0.162 / #1352): OwningTask single-consumer tracking.
// bz_task_mark_spent: record that an OwningTask has been consumed by its first
// await (whether the await succeeded or faulted). A second await on a spent task
// should raise InvalidOperationException (checked by the codegen via
// bz_task_is_spent at the await site).
void bz_task_mark_spent(void* tp) {
    auto* task = (bz_task_t*)tp;
    std::lock_guard<std::mutex> lk(task->mtx);
    task->is_spent = true;
}

// bz_task_is_spent: returns true after the first await consumed the OwningTask.
bool bz_task_is_spent(void* tp) {
    auto* task = (bz_task_t*)tp;
    std::lock_guard<std::mutex> lk(task->mtx);
    return task->is_spent;
}

// §30.5 / #1332 (v0.162): transfer ownership of `source` into `outer`'s chained
// slot. Called when `return await <Pattern A source>` completes the outer async
// method — the source task must outlive the outer task so the borrowed result
// (stored in the outer task's result buffer as a pointer into the source task's
// result buffer) is valid for as long as the outer task is alive. The outer task
// takes exclusive ownership of the source; do not free source separately.
void bz_task_set_chained(void* outer_p, void* source_p) {
    if (!outer_p || !source_p) return;
    auto* outer = (bz_task_t*)outer_p;
    std::lock_guard<std::mutex> lk(outer->mtx);
    // If a previous chained source exists (shouldn't in normal use, but guard
    // defensively), free it to avoid a leak.
    if (outer->chained_source) bz_task_free(outer->chained_source);
    outer->chained_source = source_p;
}

// §30.5 / #1503: attach a per-T disposer for `task`'s produced class-typed
// result, to be run by bz_task_free once the task settles (see bz_task_t::
// result_disposer's doc comment for the full contract). Guarded by the task's
// mutex so the write is visible to whichever thread eventually completes and
// frees the task (mirrors bz_task_set_chained above: the write may race an
// in-flight bz_task_complete on a pool thread, and the SAME mutex is what
// establishes the happens-before edge bz_task_free's later, lock-free read
// relies on -- bz_task_free only ever runs after the task has settled, by
// which point no other thread still touches the struct).
//
// Used from two codegen sites:
//   - the async body's non-chained `return <owned>` completion, for a Pattern
//     A `Task<T:class>` method (the task always owns a fresh result);
//   - the consumer's `using var` scope-end drop of an unconsumed `OwningTask
//     <T:class>` local (Pattern B), replacing an eager-only dispose attempt
//     that missed a task still running at drop time (#1503).
void bz_task_set_result_disposer(void* tp, void (*disposer)(void*)) {
    if (!tp) return;
    auto* task = (bz_task_t*)tp;
    std::lock_guard<std::mutex> lk(task->mtx);
    task->result_disposer = disposer;
}

// fix #4159 (§30.5 "Disposing an unconsumed OwningTask<T>", v0.298): queried
// by emitAsyncPatternAResultDecision's kind-2 (chained) arm, right after
// bz_task_set_chained, to decide whether THIS body's own outer task must
// ALSO get a kind-1 disposer attached for the value it just extracted from
// `source` (an anonymous-producer §30.5 chain source, which may be a
// genuine Pattern A `Task<T:class>` OR a Pattern B `OwningTask<T:class>` --
// detectAsyncPatternAKind's CallExpr detection does not distinguish the two
// [LlvmBackend_Async.cpp]).
//
// Returns 1 ("orphaned -- the caller must attach its own disposer") iff
// `source` has settled with NEITHER a chained_source of its own NOR a
// result_disposer of its own: i.e. nothing else will ever dispose the value
// this call is about to extract from it. Returns 0 ("already handled") when
// EITHER is set -- a genuine Pattern A chain (whose own chained_source
// reaches a real disposer further down, or whose own result_disposer is set
// directly) already disposes the result transitively; attaching a second
// disposer here would double-free it.
//
// Race-safety: this task has SETTLED by the time an awaiter reaches this
// call (that is what `await` resuming past suspension means), and the SAME
// mutex bz_task_set_chained / bz_task_set_result_disposer take to WRITE
// these two fields establishes the happens-before edge for this READ --
// mirroring bz_task_free's own "no other thread still touches the struct"
// invariant, but taken defensively here (unlike bz_task_free, this does not
// run as part of the task's own teardown).
int8_t bz_task_needs_caller_disposal(void* tp) {
    if (!tp) return 0;
    auto* task = (bz_task_t*)tp;
    std::lock_guard<std::mutex> lk(task->mtx);
    return (!task->chained_source && !task->result_disposer) ? 1 : 0;
}

// §30.13 / bz_task_await_sync: the main OS thread (emitted by the async-Main
// wrapper in LlvmBackend_Async.cpp) blocks here until the top-level Task
// completes.  This function is NOT a domain continuation: the main thread is
// not a domain runner, holds no domain mutex while waiting, and just parks on
// the task's condition variable.  Pool workers running domain drains eventually
// complete the task (via bz_task_complete -> task_dispatch), which calls
// notify_all on the task cv (inside the completer's lock, per #445/#700), waking
// this thread.  No deadlock is possible:
//   - The main thread holds only task->mtx during cv.wait (released atomically
//     by wait; re-acquired only for the predicate check, then released before
//     returning).
//   - Domain drains hold only dom->mtx while manipulating the deque, never
//     while running continuations.
//   - Lock ordering is always task->mtx then dom->mtx (in the completers /
//     task_dispatch / domain_enqueue), never reversed.
//
// §30.3 (v0.162 / #1362): the fast-path MUST remain first — WhenAll/WhenAny
// internals and lowerAwait result-reads call this on COMPLETED tasks from
// inside continuations (g_current_domain != null), and those must not abort.
// Only the slow-path (task not yet done while inside a domain continuation)
// would deadlock; that case aborts with a diagnostic message.
void* bz_task_await_sync(void* tp) {
    auto* task = (bz_task_t*)tp;
    std::unique_lock<std::mutex> lk(task->mtx);
    // Fast path: already done — return immediately (no blocking, no domain risk).
    // This path is taken by WhenAll/WhenAny internals and by the result-reads
    // inside continuations (g_current_domain != null, task already complete).
    if (task->done) {
        return task->is_error ? nullptr : task->result;
    }
    // §30.3 (v0.162 / #1362): blocking on an INCOMPLETE task inside a domain
    // continuation is a deterministic deadlock — the domain is frozen, so the
    // task's continuation can never be enqueued and run.  The sema check already
    // rejects Wait()/Result inside `async` methods; this guard covers the
    // non-async-helper case (a sync helper called from a continuation).
    if (g_current_domain != nullptr) {
        lk.unlock();
        fputs("fatal: blocking wait inside a domain continuation; "
              "the domain can never advance — await the task, or query IsCompleted "
              "(§30.3 v0.162)\n", stderr);
        fflush(stderr);
        abort();
    }
    task->cv.wait(lk, [task]{ return task->done; });
    return task->is_error ? nullptr : task->result;
}

const char* bz_task_error(void* tp) {
    return ((bz_task_t*)tp)->error_msg;
}

// §30.10 / §30.13 / #1617: register a continuation on `task`. If the task is
// NOT yet settled, the continuation is appended to the task's continuation list
// and fired by bz_task_complete (which routes it through the domain queue). If
// the task IS already settled, the continuation is run SYNCHRONOUSLY (inline)
// rather than deferred — see the fast-path comment at the top of this file for
// the full rationale and the UAF window this closes vs bz_task_fire_and_forget.
//
// The inline call is made with g_current_domain set to the task's domain (the
// same save/restore domain_drain performs) so any nested async allocation
// routes correctly, and with task->mtx already released so the continuation may
// itself complete tasks / enqueue domain work without deadlock. A per-thread
// depth cap bounds the only recursive shape (the async-event raise chain over
// synchronous handlers); past the cap the continuation falls back to the
// deferred domain_enqueue path, which is safe there because the raise-chain
// context owns the handler task and frees it in the step itself — no external
// fire-and-forget can race it. Combinator helpers never recurse, so they always
// run inline regardless of input count.
void bz_task_then(void* tp, void (*cont)(void* task, void* state), void* state) {
    auto* task = (bz_task_t*)tp;
    bool already_done;
    bz_domain_t* dom;
    {
        std::lock_guard<std::mutex> lk(task->mtx);
        already_done = task->done;
        dom = task->domain;
        if (!already_done) task->continuations.push_back({cont, state});
    }
    if (!already_done) return;
    // #1617: run the continuation inline so it cannot race a concurrent inline
    // free of this task (bz_task_fire_and_forget's already-done fast path). The
    // depth cap bounds recursion for the async-event raise chain; the deferred
    // fallback is safe for that chain (see the function comment above).
    if (g_task_then_depth < 64) {
        ++g_task_then_depth;
        bz_domain_t* prev_cd = g_current_domain;
        g_current_domain = dom;
        cont(task, state);
        g_current_domain = prev_cd;
        --g_task_then_depth;
    } else {
        domain_enqueue(dom, cont, task, state);
    }
}

void bz_task_free(void* tp) {
    // fix #3460 (§28.3 / §30.1): bz_task_free is now a thin wrapper around
    // bz_task_release. The producer's original reference (created with count
    // == 1 by bz_task_alloc) is balanced by either of these. Callers that hold
    // a second reference (the frame's strong reference to outerTask, retained
    // after the outerTask field is stored in the heap frame) release it via
    // bz_task_release when the frame is freed.
    bz_task_release(tp);
}

// fix #3460 (§28.3 / §30.1): increment the task's reference count. Pairs with
// bz_task_release. The producer's original reference (the one balanced by
// bz_task_free) starts at 1; bz_task_retain is the way the heap frame picks
// up a strong reference to its own outerTask so the worker resume continuation
// can safely read frame[OUTER_TASK_FIELD] even after the caller's `bz_task_free`
// has already run from `using var` scope-end disposal. Null-safe (no-op on
// null); safe on a task that has already completed (the refcount outlives
// task completion — only the LAST release runs the destroy body).
void bz_task_retain(void* tp) {
    if (!tp) return;
    auto* task = (bz_task_t*)tp;
    task->refcount.fetch_add(1, std::memory_order_acq_rel);
}

// fix #3460 (§28.3 / §30.1): decrement the task's reference count; when it
// reaches zero, run the destroy body (the former bz_task_free dispose chain).
// The destroy body is split into a static helper so it can be invoked exactly
// once, by the call whose decrement drops the count to zero.
static void bz_task_destroy(bz_task_t* task) {
    // §30.5 / #1503: dispose an UNCONSUMED owned class-typed result before
    // reclaiming the result buffer. Guards mirror bz_task_dispose_owned_result:
    //   • result_disposer == null → no ownership obligation on the result
    //     (Task, Task<non-class T>, or a Pattern B task never dropped
    //     unconsumed) — skip.
    //   • task->is_error          → the body faulted; no result was produced
    //     (§30.5: "has no owned result to dispose; disposal is a no-op") — skip.
    //   • task->result == null    → already consumed: a Pattern B consuming
    //     await/`.Result` runs bz_task_take_result, which nulls the slot —
    //     skip (no double-free of the now-caller-owned object). A Pattern A
    //     borrow-only await never nulls this, so a Pattern A result is
    //     disposed here exactly once regardless of how many borrows it saw.
    // No locking: by the time bz_task_free runs the task has settled (either
    // called synchronously on an already-done task, or via the fire-and-forget
    // completion continuation) and no other thread still touches the struct —
    // the same invariant the pre-existing lock-free reads below already rely on.
    //
    // NOTE on error_obj: bz_task_free does NOT dispose error_obj. The owning
    // task's fault exception is disposed by:
    //   • codegen's scope-end error_obj disposal (LlvmBackend_Disposal.cpp,
    //     when bz_task_is_error(t) was true at scope end), which runs BEFORE
    //     fire-and-forget on the fast path; or
    //   • the slow-path fire-and-forget continuation registered by
    //     bz_task_fire_and_forget / bz_task_fire_and_forget_checked at the
    //     task local's scope end (see §30.9 / #1601 item 4 / #1021 — the
    //     "still running at scope end" arm must dispose when the abandoned
    //     task later faults); or
    //   • the `await` of a faulted task that re-raises the error_obj (the
    //     catch arm's terminal act, deferred from scope end under #1528's
    //     in-flight deferral).
    // Doing it HERE would be wrong for two reasons: (a) WhenAllOwned captures
    // the input task's error_obj into `fault_objs[i]` BEFORE calling
    // bz_task_free on the input (Pattern B — the input task transfers
    // ownership of the exception to the aggregate; if bz_task_free disposed
    // it, the captured pointer would dangle and the aggregate's re-raise
    // would UAF); (b) the fast path already-nulled error_obj path is the
    // SCOPE-END disposal's job, not bz_task_free's.
    //
    // fix #4162 (§30.5 "Disposing an unconsumed OwningTask<T>" / "Pattern A
    // result slots are of three kinds", v0.298): a task whose own result
    // buffer is a genuine Pattern A kind-2 BORROW through its chained_source
    // (not an independently-owned value) must NOT also run its own
    // result_disposer — the chained_source transitively owns disposal (the
    // recursive bz_task_free(chainedSrc) below already reaches it). Two
    // INDEPENDENT producers can each legitimately attach a disposer/chain to
    // the SAME task without either knowing about the other: a `using var`
    // consumer registers a disposer purely from the LOCAL's declared
    // OwningTask<T:class> type (LlvmBackend_Disposal.cpp's
    // attachOwningTaskResultDisposer), while the task's OWN producer body —
    // e.g. an async lambda whose whole body is `return await <Pattern A
    // source>` — independently sets chained_source via bz_task_set_chained.
    // When BOTH are set AND the chained source's result is STILL POPULATED,
    // this task's "result" is that SAME borrowed pointer value, not
    // something this task owns — invoking result_disposer here would
    // dispose it, and the chain recursion below would dispose the identical
    // object again through the chained source's OWN disposer: a double free
    // (measured, issue #4162: `var f := async lambda() => await P(); using
    // var outer := f();`, never awaited, disposed directly rather than
    // through an intermediate async method — ASan: heap-use-after-free in
    // bz$elemDispose, the same Widget3960 freed twice).
    //
    // The chained source's result being NULL (rather than the chain being
    // simply absent) is the fix #4159 case: an ORPHANED Pattern B
    // extraction where THIS task's own result is independently owned (a
    // fresh, just-extracted value) and chained_source is a drained,
    // now-empty container kept only for its own reclaim — there, this
    // task's result_disposer is the ONLY thing that will ever dispose the
    // value, and correctly still fires. Checking the chained source's OWN
    // result (not merely whether chained_source is set) is what
    // distinguishes the two. Reading it here is safe: by the time
    // bz_task_free ever runs on THIS task, chained_source's own producer has
    // already fully settled (the chain is always set, by construction,
    // before this task's own bz_task_complete runs) — the same "no other
    // thread still touches the struct" invariant this function's own top
    // comment already relies on extends transitively to it.
    bool chainedSourceOwnsResult =
        task->chained_source &&
        static_cast<bz_task_t*>(task->chained_source)->result != nullptr;
    if (task->result_disposer && !task->is_error && task->result &&
        task->result_size >= (int64_t)sizeof(void*) &&
        !chainedSourceOwnsResult) {
        void* obj = *(void**)task->result;
        if (obj) task->result_disposer(obj);
    }
    free(task->result);
    free(task->error_msg);
    // §30.5 / #1332 (v0.162): dispose the chained Pattern A source task (if any)
    // transitively — the source owns the result that the outer task borrowed.
    void* chainedSrc = task->chained_source;
    bz_domain_t* dom = task->domain;
    // §30.15 (v0.179, #1524): release the core ref bz_channel_receive acquired
    // for this task's whole lifetime (mirrors fix #634's Channel/ChannelSender
    // handle refcounting). Keeps the channel core alive for as long as ANY
    // receive task from it could still dereference it — including a deferred
    // bz_channel_receive_abandon give-back continuation (scheduled via
    // bz_task_then) that has not yet run at the point the channel's own
    // `using var` disposes it. Null for every task not from bz_channel_receive.
    void* recvChannel = task->recv_channel;
    delete task;
    if (dom) domain_release(dom);  // per fix #632: release task's domain ref
    if (recvChannel) channel_core_release((bz_channel_t*)recvChannel);
    // fix #3460 (§28.3 / §30.1): the chained Pattern A source task is held
    // through its own refcount (the chained_source link is a strong reference
    // established when codegen ran bz_task_set_chained). Use the refcounted
    // release so the chained source's own refcount — possibly inflated by a
    // codegen-side retain we have not yet introduced for chained sources —
    // remains consistent with every other release path.
    if (chainedSrc) bz_task_release(chainedSrc);
}

void bz_task_release(void* tp) {
    if (!tp) return;
    auto* task = (bz_task_t*)tp;
    // acq_rel pairs with bz_task_retain's acq_rel add: a release-decrement must
    // not be reordered before any later reader of the released state on a
    // thread that observed a prior retain. The acquire-load of the post-
    // decrement value (==0 here) ensures the destroy body sees every store
    // that any preceding retain-er made before its release.
    int32_t prev = task->refcount.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) bz_task_destroy(task);
}

// stdlib §6.7 / §30.10: Task.FromResult<T>(value). Allocate a fresh task and
// immediately complete it with the `result_size` bytes at `result`. The task is
// already-done at return; `await` on it takes the already-done fast path (no
// thread-pool submission). result == null / result_size == 0 produces an
// already-completed no-value task (the primitive behind Task.CompletedTask).
void* bz_task_from_result(const void* result, int64_t result_size) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    auto* task = new bz_task_t();
    // fix #713: route to the caller's domain.
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      task->domain = d; domain_ref(d); }  // per fix #632: task holds a ref on its domain
    if (result_size > 0 && result) {
        task->result = malloc((size_t)result_size);
        memcpy(task->result, result, (size_t)result_size);
        task->result_size = result_size;
    }
    task->done = true;
    // No continuations to notify — no one registered yet; the task is complete
    // at allocation time.
    return task;
}

// stdlib §6.7 / §30.10: Task.WhenAll(tasks: Array<Task>): Task.
// Returns a new task that completes (with no value) once ALL input tasks are
// done — WhenAll never abandons laggards (§30.10 "Aggregation contracts").
// Uses a shared context with a refcount (number of registered continuations)
// and a completion counter. Every continuation decrements the refcount on
// exit (freeing the context when it reaches 0) and decrements the completion
// counter; the last completion (counter → 0) fires the aggregate. The context
// is heap-allocated once and shared across all continuations via a raw
// pointer (no C++ shared_ptr to stay extern "C" compatible).
//
// §30.9 / §30.10 / #443 / #1529: fault propagation is by INPUT ORDER, not by
// completion order. The terminal continuation (the one that observes
// `remaining` hit 0 — every input is done at that point, so no race) scans
// `task_arr` in ARRAY ORDER for the first non-successful input and re-derives
// the aggregate's outcome from THAT task alone; an earlier revision instead
// raced a CAS on whichever continuation observed a fault first, which is the
// first task to COMPLETE, not the first in the array — nondeterministic and a
// spec violation ("the first non-successful input in input order", §30.10).
// A faulted input's error_obj is a Bezel heap object the INPUT TASK OWNS
// (§30.9); the aggregate only BORROWS it for re-raise (an aggregate "owns no
// fault of its own", §30.10), so `fault_borrowed` is set on the aggregate —
// codegen's fault-disposal paths consult it to avoid double-freeing an
// exception the (still-owning, independently-disposed) input task will
// dispose at its own scope end. A cancelled input may instead carry only a
// message (no error_obj, e.g. a cancelable Task.Delay) — propagated via
// error_msg so the aggregate re-raises a fresh OperationCanceledException,
// exactly as awaiting that input directly would (§30.10).
struct BzWhenAllCtx {
    bz_task_t* aggregate;
    std::atomic<int64_t> remaining;  // pending completion count
    std::atomic<int64_t> refcount;   // continuation refcount (= n at init)
    std::vector<void*> tasks;        // SNAPSHOT of the input task pointers
                                      // (borrowed), taken at call time — the
                                      // terminal input-order scan reads THIS,
                                      // never the input `Array<Task>`. The
                                      // input array is a BORROWED `using var`
                                      // the caller may dispose at scope end
                                      // before a queued continuation runs; a
                                      // read-back of it there is a
                                      // use-after-free (the not-awaited
                                      // WhenAll aggregate regression, #1529).
                                      // Fully populated before any continuation
                                      // is registered, so the terminal
                                      // continuation (fires only once all n
                                      // have decremented `remaining`) sees an
                                      // immutable snapshot — no data race.
};

void* bz_task_when_all(void* task_arr) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    auto* arr = (bz_array_t*)task_arr;
    int64_t n = bz_array_len(arr);
    auto* aggregate = new bz_task_t();
    // fix #713: route aggregate to the caller's domain.
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      aggregate->domain = d; domain_ref(d); }  // per fix #632: task holds a ref on its domain
    if (n == 0) {
        aggregate->done = true;
        return aggregate;
    }
    auto* ctx = new BzWhenAllCtx{aggregate, {n}, {n}, {}};
    // Snapshot the input task pointers BEFORE registering any continuation, so
    // the terminal input-order scan never reads back the borrowed input array
    // (which the caller may free at scope end while a continuation is still
    // queued — the not-awaited-aggregate use-after-free). §30.10 / #1529.
    ctx->tasks.reserve((size_t)n);
    for (int64_t i = 0; i < n; ++i) {
        void* t = nullptr;
        bz_array_get(arr, i, &t, sizeof(void*));
        ctx->tasks.push_back(t);
    }
    for (int64_t i = 0; i < n; ++i) {
        bz_task_then(ctx->tasks[i],
            [](void* /*completedTask*/, void* state) {
                auto* c = (BzWhenAllCtx*)state;
                // When ALL inputs are done (remaining → 0), scan in INPUT
                // ORDER (§30.10) for the first non-successful task. Every
                // input is done at this point, so each one's is_error /
                // error_obj / error_msg is stable — no race.
                if (c->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    int64_t n2 = (int64_t)c->tasks.size();
                    bool faulted = false;
                    for (int64_t j = 0; j < n2 && !faulted; ++j) {
                        void* tj = c->tasks[j];
                        if (bz_task_is_error(tj)) {
                            faulted = true;
                            void* errObj = bz_task_error_obj(tj);
                            if (errObj) {
                                bz_task_complete_error_obj(c->aggregate, errObj);
                                c->aggregate->fault_borrowed = true;
                            } else {
                                auto* tjt = (bz_task_t*)tj;
                                bz_task_complete_error(
                                    c->aggregate,
                                    tjt->error_msg ? tjt->error_msg
                                                   : "OperationCanceledException");
                            }
                        }
                    }
                    if (!faulted) bz_task_complete(c->aggregate, nullptr, 0);
                }
                if (c->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    delete c;
            },
            ctx);
    }
    return aggregate;
}

// stdlib §6.7 / §30.10: Task.WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>.
// Variant of bz_task_when_all that collects each input task's result (elem_size
// bytes) into a new bz_array_t and completes the aggregate with a ptr-sized
// result (the Array<T> pointer). Fault propagation matches bz_task_when_all
// (#1529): the terminal continuation scans the ORIGINAL input array in INPUT
// ORDER for the first non-successful task, rather than racing on completion
// order; `fault_borrowed` marks the aggregate so codegen never double-frees
// the fault object the winning input task still owns.
//
// Thread safety: each continuation writes to a distinct results[] slot (no
// aliasing), so no mutex is needed for slot stores. The acq_rel fetch_sub on
// `remaining` provides the ordering: the last decrement acquires all prior
// result stores (via the release from each preceding thread's fetch_sub), so
// the final continuation sees the complete results[] AND every input task's
// stable is_error/error_obj/error_msg before deciding the outcome.
struct BzWhenAllResultsCtx {
    bz_task_t* aggregate;
    std::atomic<int64_t> remaining;      // pending completion count
    std::atomic<int64_t> refcount;       // continuation refcount (= n at init)
    int64_t n;                           // total task count
    int64_t elem_size;                   // sizeof(T) — bytes per result element
    void** result_bufs;                  // result_bufs[i]: malloc'd copy of task i's result
    std::vector<void*> tasks;            // SNAPSHOT of input task pointers
                                          // (borrowed), taken at call time —
                                          // the terminal input-order scan reads
                                          // THIS, never the borrowed input
                                          // array (which the caller may free at
                                          // scope end while a continuation is
                                          // still queued: a use-after-free).
                                          // Fully populated before any
                                          // continuation registers. §30.10 /
                                          // #1529.
};

struct BzWhenAllResultsSlot {
    BzWhenAllResultsCtx* ctx;
    int64_t idx;
};

// #2278 / §30.5: NOTE — no `result_disposer` is attached to this aggregate, and
// attaching one is NOT the fix for the aggregate-`Array<T>` leak recorded there.
// The implementation's current owner of a Pattern A intrinsic-collection result
// is the awaiting `using var` (codegen's registerUsingDisposal emits the
// container free), not the task; a task-side disposer therefore double-frees
// `using var rs := await w;` and use-after-frees `using var rs := await
// Task.WhenAll([…]);` (unbound temporary: the task's teardown at end of statement
// would free the array `rs` still points at). Both were measured. Closing the
// leak requires the coordinated change — attach a container disposer at every
// Pattern A collection-result mint AND make the awaiting binding a §30.5 borrow
// (usingVarSourceIsBorrow) — which is a separate lane, not a runtime one-liner.
void* bz_task_when_all_results(void* task_arr, int64_t elem_size) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    auto* arr = (bz_array_t*)task_arr;
    int64_t n = bz_array_len(arr);
    auto* aggregate = new bz_task_t();
    // fix #713: route aggregate to the caller's domain.
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      aggregate->domain = d; domain_ref(d); }  // per fix #632: task holds a ref on its domain
    if (n == 0) {
        // Empty array: complete immediately with an empty Array<T>.
        void* emptyArr = bz_array_alloc(elem_size, 0);
        aggregate->result = malloc(sizeof(void*));
        memcpy(aggregate->result, &emptyArr, sizeof(void*));
        aggregate->result_size = (int64_t)sizeof(void*);
        aggregate->done = true;
        return aggregate;
    }
    // Allocate result buffer slots (one per task). Each slot holds a malloc'd
    // copy of the task's raw result bytes (elem_size bytes).
    void** result_bufs = (void**)calloc((size_t)n, sizeof(void*));
    auto* ctx = new BzWhenAllResultsCtx{
        aggregate, {n}, {n}, n, elem_size, result_bufs, {}};
    // Snapshot the input task pointers BEFORE registering any continuation, so
    // the terminal input-order scan never reads back the borrowed input array
    // (freed at the caller's scope end while a continuation is still queued —
    // the not-awaited-aggregate use-after-free). §30.10 / #1529.
    ctx->tasks.reserve((size_t)n);
    for (int64_t i = 0; i < n; ++i) {
        void* t = nullptr;
        bz_array_get(arr, i, &t, sizeof(void*));
        ctx->tasks.push_back(t);
    }
    for (int64_t i = 0; i < n; ++i) {
        // Per-slot: allocate a BzWhenAllResultsSlot to carry (ctx, idx) into
        // the continuation. The slot is freed by the continuation itself.
        auto* slot = new BzWhenAllResultsSlot{ctx, i};
        bz_task_then(ctx->tasks[i],
            [](void* completedTask, void* state) {
                auto* slot = (BzWhenAllResultsSlot*)state;
                auto* c  = slot->ctx;
                int64_t i = slot->idx;
                delete slot;

                if (!bz_task_is_error(completedTask)) {
                    // Copy this task's result bytes into results[i]. A
                    // faulted task has no result to capture; its outcome is
                    // decided by the input-order scan below instead.
                    void* resPtr = bz_task_await_sync(completedTask);
                    if (resPtr && c->elem_size > 0) {
                        void* buf = malloc((size_t)c->elem_size);
                        memcpy(buf, resPtr, (size_t)c->elem_size);
                        c->result_bufs[i] = buf;
                    }
                }

                // When ALL inputs done (remaining → 0): scan in INPUT ORDER
                // (§30.10 / #1529) for the first non-successful task.
                if (c->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    int64_t n2 = (int64_t)c->tasks.size();
                    bool faulted = false;
                    for (int64_t j = 0; j < n2 && !faulted; ++j) {
                        void* tj = c->tasks[j];
                        if (bz_task_is_error(tj)) {
                            faulted = true;
                            // Free any collected result bufs (discarded).
                            for (int64_t k = 0; k < c->n; ++k) free(c->result_bufs[k]);
                            free(c->result_bufs);
                            void* errObj = bz_task_error_obj(tj);
                            if (errObj) {
                                bz_task_complete_error_obj(c->aggregate, errObj);
                                c->aggregate->fault_borrowed = true;
                            } else {
                                auto* tjt = (bz_task_t*)tj;
                                bz_task_complete_error(
                                    c->aggregate,
                                    tjt->error_msg ? tjt->error_msg
                                                   : "OperationCanceledException");
                            }
                        }
                    }
                    if (!faulted) {
                        // Build Array<T> from collected result bufs.
                        void* resultArr = bz_array_alloc(c->elem_size, c->n);
                        for (int64_t j = 0; j < c->n; ++j) {
                            if (c->result_bufs[j])
                                bz_array_add(resultArr, c->result_bufs[j],
                                             c->elem_size);
                            else {
                                // Zero-result slot (task had 0-byte result).
                                void* zero = calloc(1, (size_t)c->elem_size);
                                bz_array_add(resultArr, zero, c->elem_size);
                                free(zero);
                            }
                            free(c->result_bufs[j]);
                        }
                        free(c->result_bufs);
                        // Complete the aggregate with the Array<T> pointer.
                        bz_task_complete(c->aggregate, &resultArr, sizeof(void*));
                    }
                }
                if (c->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    delete c;
            },
            slot);
    }
    return aggregate;
}

// §30.10 / #1529: dispose a Bezel class instance generically — Dispose() via
// its own vtable slot 2, then free the block. Every Bezel class object has a
// leading vtable word (§8.5) with Dispose at the fixed slot 2 (the same
// layout emitDisposeAndReclaim's codegen dispatches through), so this needs
// no type-specific support: it works for a WhenAllOwned<T> result (an
// arbitrary class T) or a fault Exception alike. Mirrors the existing
// bz_domain_spawn fault-wrap pattern (this file, DomainFaultedException path).
static void bz_dispose_class_obj_impl(void* obj) {
    if (!obj) return;
    void** vtbl = nullptr;
    memcpy(&vtbl, obj, sizeof(void*));
    if (vtbl && vtbl[2]) {
        typedef void (*DispFn)(void*);
        ((DispFn)vtbl[2])(obj);
    }
    // issue 2330 / §31.4 / §30.9: reclaim a §31.4 CauseChain snapshot if `obj`
    // is an exception carrying one. The shared exception Dispose above
    // deliberately does NOT free CauseChain (#811/#1389: a still-borrowed
    // exception's chain can be read after its Dispose has dispatched), so the
    // free belongs at the terminal reclaim — here, immediately before the
    // block goes away, which is the same position codegen's
    // emitDisposeAndReclaim uses. A no-op for every non-exception object that
    // reaches this helper (a WhenAllOwned input's successful result, say): the
    // gate admits only a built-in exception vtable or a descriptor chain
    // carrying a built-in exception ancestor, so the header read at offset 24
    // never happens for a class without the §31.4 header.
    bz_exc_reclaim_causechain_registered(obj);
    free(obj);
}

// EXPERIMENT (#5872): process-wide registry of RUNTIME-OWNED exceptions —
// objects the spec declares live for the life of the process and that no
// handler may ever reclaim. The §22.7 cached TypeInitializationException is
// the motivating case. Unlike g_borrowed_exc this must NOT be thread-local:
// the cached instance is process-global, so a mark made on the domain that
// ran the failing initializer has to be visible to every other domain that
// later catches it. Mutex-guarded (a cross-domain insert/read).
static std::mutex g_runtime_owned_exc_mtx;
static std::unordered_set<void*> g_runtime_owned_exc;
// Non-zero once any runtime-owned instance exists. Every reclaim in the
// program reads this first, so the overwhelming common case (no failed
// static initializer anywhere) costs ONE relaxed atomic load and takes no
// lock — the reclaim path must not serialize every object free in the
// process behind this.
static std::atomic<int> g_runtime_owned_exc_any{0};

extern "C" void bz_exc_mark_runtime_owned(void* obj) {
    if (!obj) return;
    {
        std::lock_guard<std::mutex> lk(g_runtime_owned_exc_mtx);
        g_runtime_owned_exc.insert(obj);
    }
    g_runtime_owned_exc_any.store(1, std::memory_order_release);
}
extern "C" int8_t bz_exc_is_runtime_owned(void* obj) {
    if (!obj) return 0;
    if (!g_runtime_owned_exc_any.load(std::memory_order_acquire)) return 0;
    std::lock_guard<std::mutex> lk(g_runtime_owned_exc_mtx);
    return g_runtime_owned_exc.count(obj) ? 1 : 0;
}
extern "C" void bz_dispose_class_obj(void* obj) {
    // EXPERIMENT (#5872): never reclaim a runtime-owned instance.
    if (obj && bz_exc_is_runtime_owned(obj)) return;
    return bz_dispose_class_obj_impl(obj);
}

// stdlib §6.7 / §30.10: Task.WhenAllOwned<T>(owned tasks: OwningArray<OwningTask<T>>):
//   OwningTask<OwningArray<T>> where T: class
//
// Pattern B aggregator: caller transfers ownership of the array (and its tasks).
// Each OwningTask<T> result is a void* (class pointer, sizeof(void*) bytes); we
// collect them into a new OwningArray<T> (a bz_array_t* of pointers). Each input
// task's STRUCT is freed after its outcome is captured (it is single-consumer,
// Pattern B) — but unlike bz_task_when_all's inputs, nothing else can observe
// these tasks again (ownership was transferred wholesale), so per-index outcome
// (success + result, or fault + error_obj) must be captured into the context
// BEFORE each task struct is freed. The input array container is freed once all
// callbacks are registered (after the loop, before returning).
//
// §30.10 / #1529: fault selection is by INPUT ORDER (the first non-successful
// task in array order), not completion order — an earlier revision raced a CAS
// on whichever continuation observed a fault first. On the winning fault:
// every ALREADY-PRODUCED successful result is disposed with its task (nobody
// else can ever see it — Pattern B consumed the whole array), every OTHER
// faulted input's exception is disposed likewise (only the winner's is
// re-raised), and the winner's error_obj becomes the aggregate's own (Pattern
// B genuinely transfers sole ownership here — unlike the BORROWING WhenAll/
// WhenAll<T>, no caller-side reference to the input survives this call, so
// `fault_borrowed` is NOT set: the aggregate legitimately owns and disposes
// this exception at its own scope end, per §30.9).
//
// Thread safety: same acq_rel protocol as bz_task_when_all_results (each slot
// writes distinct results[]/is_fault[]/fault_objs[] entries; the last
// fetch_sub acquires all prior stores). The element size is always
// sizeof(void*) since T: class.
struct BzWhenAllOwnedCtx {
    bz_task_t* aggregate;
    std::atomic<int64_t> remaining;  // pending completion count
    std::atomic<int64_t> refcount;   // continuation refcount (= n at init)
    int64_t n;                       // total task count
    void** results;                  // results[i]: captured T* on success, else null
    int8_t* is_fault;                // is_fault[i]: 1 if task i completed faulted
    void** fault_objs;               // fault_objs[i]: error_obj if faulted (may be null)
};

struct BzWhenAllOwnedSlot {
    BzWhenAllOwnedCtx* ctx;
    int64_t idx;
};

void* bz_task_when_all_owned(void* task_arr) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    auto* arr = (bz_array_t*)task_arr;
    int64_t n = bz_array_len(arr);
    auto* aggregate = new bz_task_t();
    // fix #713: route aggregate to the caller's domain.
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      aggregate->domain = d; domain_ref(d); }  // per fix #632: task holds a ref on its domain
    if (n == 0) {
        // Empty array: complete immediately with an empty OwningArray<T>.
        void* emptyArr = bz_array_alloc(sizeof(void*), 0);
        aggregate->result = malloc(sizeof(void*));
        memcpy(aggregate->result, &emptyArr, sizeof(void*));
        aggregate->result_size = (int64_t)sizeof(void*);
        aggregate->done = true;
        // Dispose the (empty) input array — ownership was transferred.
        bz_array_free(task_arr, nullptr);
        return aggregate;
    }
    // Allocate per-index outcome slots (one per task, pointer-sized).
    void** results = (void**)calloc((size_t)n, sizeof(void*));
    int8_t* is_fault = (int8_t*)calloc((size_t)n, sizeof(int8_t));
    void** fault_objs = (void**)calloc((size_t)n, sizeof(void*));
    auto* ctx = new BzWhenAllOwnedCtx{aggregate, {n}, {n}, n, results, is_fault, fault_objs};
    for (int64_t i = 0; i < n; ++i) {
        void* t = nullptr;
        bz_array_get(arr, i, &t, sizeof(void*));
        auto* slot = new BzWhenAllOwnedSlot{ctx, i};
        bz_task_then(t,
            [](void* completedTask, void* state) {
                auto* slot = (BzWhenAllOwnedSlot*)state;
                auto* c  = slot->ctx;
                int64_t i = slot->idx;
                delete slot;

                if (bz_task_is_error(completedTask)) {
                    c->is_fault[i] = 1;
                    c->fault_objs[i] = bz_task_error_obj(completedTask);
                } else {
                    // Capture the result pointer (T*) from the OwningTask<T>.
                    // The result is a pointer-sized value stored as void*.
                    void* resPtr = bz_task_await_sync(completedTask);
                    if (resPtr) {
                        void* objPtr = nullptr;
                        memcpy(&objPtr, resPtr, sizeof(void*));
                        c->results[i] = objPtr;
                    }
                }
                // Free the OwningTask<T> STRUCT — ownership was transferred to
                // us (#451 Pattern B: single-consumer, the task is spent after
                // this await). This does not touch error_obj/the captured
                // result object (separate Bezel heap allocations, see
                // bz_task_t::error_obj) — those outcomes were captured above.
                bz_task_free(completedTask);

                // When ALL inputs done (remaining → 0): scan in INPUT ORDER
                // (§30.10 / #1529) for the first non-successful task.
                if (c->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    int64_t firstFault = -1;
                    for (int64_t j = 0; j < c->n; ++j) {
                        if (c->is_fault[j]) { firstFault = j; break; }
                    }
                    if (firstFault >= 0) {
                        // Dispose every already-produced successful result —
                        // no one else will ever see it (Pattern B consumed
                        // the whole input array; the aggregate faults
                        // instead of returning a result).
                        for (int64_t j = 0; j < c->n; ++j) {
                            if (!c->is_fault[j] && c->results[j])
                                bz_dispose_class_obj(c->results[j]);
                        }
                        // Dispose every OTHER faulted input's exception —
                        // same reasoning; only firstFault's is re-raised.
                        for (int64_t j = 0; j < c->n; ++j) {
                            if (c->is_fault[j] && j != firstFault && c->fault_objs[j])
                                bz_dispose_class_obj(c->fault_objs[j]);
                        }
                        void* errObj = c->fault_objs[firstFault];
                        free(c->results);
                        free(c->is_fault);
                        free(c->fault_objs);
                        if (errObj) {
                            // Pattern B: ownership was fully transferred in,
                            // so (unlike the borrowing WhenAll/WhenAll<T>) no
                            // other reference survives — the aggregate is the
                            // sole owner and disposes this exception itself
                            // (fault_borrowed stays false, its default).
                            bz_task_complete_error_obj(c->aggregate, errObj);
                        } else {
                            // Defensive fallback: a message-only fault (no
                            // captured object). Not expected in practice for a
                            // class-typed OwningTask<T> (every user throw path
                            // carries a real exception object), but propagate
                            // the failure rather than silently succeeding.
                            bz_task_complete_error(c->aggregate,
                                                    "OperationCanceledException");
                        }
                    } else {
                        // Build OwningArray<T> from collected result pointers.
                        int64_t esz = (int64_t)sizeof(void*);
                        void* resultArr = bz_array_alloc(esz, c->n);
                        for (int64_t j = 0; j < c->n; ++j) {
                            bz_array_add(resultArr, &c->results[j], esz);
                        }
                        free(c->results);
                        free(c->is_fault);
                        free(c->fault_objs);
                        // Complete the aggregate with the OwningArray<T> pointer.
                        bz_task_complete(c->aggregate, &resultArr, sizeof(void*));
                    }
                }
                if (c->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    delete c;
            },
            slot);
    }
    // Ownership of the input array was transferred — free the container now
    // (the task pointers have been read by bz_task_then, which kept references
    // internally; the array itself is no longer needed).
    bz_array_free(task_arr, nullptr);
    return aggregate;
}

// stdlib §6.7 / §30.10: Task.WhenAny(tasks: Array<Task>): Task<int> (v0.162 / #1332).
// Returns a new task that completes with the INDEX (i32) of the first task to
// finish. (Previously returned Task<Task> — the task pointer — but that form was
// unconstructible under §30.5.) Uses a shared context with a once-flag and a
// refcount. Every continuation decrements the refcount on exit; the first to fire
// completes the aggregate via CAS on the once-flag. Per-slot state carries the
// index so the lambda can complete the aggregate with the right i32 value.
struct BzWhenAnyCtx {
    bz_task_t* aggregate;
    std::atomic<bool> fired;         // once-flag: only the first winner fires
    std::atomic<int64_t> refcount;   // continuation refcount (= n at init)
};

// Per-slot state carrying the shared context and this slot's input index.
struct BzWhenAnySlot {
    BzWhenAnyCtx* ctx;
    int32_t idx;   // i32: WhenAny produces Task<int>, int is 32-bit in Bezel
};

void* bz_task_when_any(void* task_arr) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    auto* arr = (bz_array_t*)task_arr;
    int64_t n = bz_array_len(arr);
    auto* aggregate = new bz_task_t();
    // fix #713: route aggregate to the caller's domain.
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      aggregate->domain = d; domain_ref(d); }  // per fix #632: task holds a ref on its domain
    if (n == 0) {
        // Empty array: complete immediately with index -1 (no winner).
        int32_t noWinner = -1;
        aggregate->result = malloc(sizeof(int32_t));
        memcpy(aggregate->result, &noWinner, sizeof(int32_t));
        aggregate->result_size = (int64_t)sizeof(int32_t);
        aggregate->done = true;
        return aggregate;
    }
    auto* ctx = new BzWhenAnyCtx{aggregate, {false}, {n}};
    for (int64_t i = 0; i < n; ++i) {
        void* t = nullptr;
        bz_array_get(arr, i, &t, sizeof(void*));
        auto* slot = new BzWhenAnySlot{ctx, (int32_t)i};
        bz_task_then(t,
            [](void* /*completedTask*/, void* state) {
                auto* slot = (BzWhenAnySlot*)state;
                auto* c = slot->ctx;
                int32_t winnerIdx = slot->idx;
                delete slot;
                bool expected = false;
                // #1332: complete with the i32 winner index, not the task pointer.
                if (c->fired.compare_exchange_strong(expected, true,
                        std::memory_order_acq_rel))
                    bz_task_complete(c->aggregate, &winnerIdx, sizeof(int32_t));
                if (c->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    delete c;
            },
            slot);
    }
    return aggregate;
}

// stdlib §6.7 / §30.10: Task.FireAndForget(owned task). Ownership of `task`
// transfers; the caller must not free or reference it again. For already-
// complete tasks, free them immediately (inline) without going through the
// scheduler — this avoids a race where the scheduler is shut down before the
// continuation runs. For incomplete tasks (still running on a worker), register
// a continuation via bz_task_then so the memory is freed when the task completes
// (safe: the continuation runs on the pool before or after shutdown drains the
// queue, and the task struct remains live until then).
void  bz_task_fire_and_forget(void* tp) {
    if (!tp) return;
    auto* task = (bz_task_t*)tp;
    // Check if already done under the lock (same as bz_task_is_done).
    bool already_done;
    { std::lock_guard<std::mutex> lk(task->mtx); already_done = task->done; }
    if (already_done) {
        // Fast path: task is complete; free immediately without the scheduler.
        // §30.9 (v0.240) / #3030: the task owns its fault exception and must
        // dispose it exactly once, regardless of which path settled it. The
        // emitTaskDisposal scope-end path (LlvmBackend_Disposal.cpp) runs an
        // `bz_task_is_error` → `bz_task_error_obj` → `bz_dispose_class_obj`
        // preamble BEFORE this call, so arrives here with error_obj already
        // null. The explicit `Task.FireAndForget(owned t)` user-facing call
        // (LlvmBackend_Calls.cpp §6.7 arm) calls bz_task_fire_and_forget
        // directly with NO preamble — a synchronous-throw task reaches this
        // arm with error_obj set, and pre-#3030 the `bz_task_free` reclaim
        // reclaims the task struct + error_msg but NOT the exception object,
        // leaking the entire Bezel exception (and, with it, anything the
        // exception owns — including the §31.4 CauseChain snapshot). Mirror
        // the slow path's guarded disposal before bz_task_free runs. The same
        // `!fault_borrowed` guard skips a WhenAll/WhenAll<T> aggregate that
        // BORROWED its fault from an input task (§30.10 / #1529 — the input
        // remains the sole owner). No-lock read: by the time this runs the
        // task has settled — the same invariant bz_task_free's lock-free
        // reads below already rely on.
        if (task->error_obj && !task->fault_borrowed) {
            bz_dispose_class_obj(task->error_obj);
            task->error_obj = nullptr;
        }
        bz_task_free(tp);
    } else {
        // Slow path: task still running at scope end (the "still-running"
        // arm of #1601 item 4 / #1021); register a continuation that
        // disposes the task's OWNED fault exception (if the abandoned task
        // later settles faulted) AND frees the task struct. Pre-fix the
        // continuation only called bz_task_free, which reclaims the task
        // struct + error_msg but NOT the Bezel exception object — every
        // abandoned-then-faulted task leaked its error_obj (the 16-byte
        // calloc'd exception visible in LSan). Spec §30.9 (v0.179): "a task
        // owns its fault exception and disposes it exactly once, by the
        // task that owns it, when the task is disposed" — an abandoned task
        // (no awaiter) satisfies "disposed when the task is disposed" here
        // via the slow-path fire-and-forget continuation.
        bz_task_then(tp,
            [](void* completedTask, void* /*state*/) {
                auto* t = (bz_task_t*)completedTask;
                // Dispose the OWNED fault exception unless (a) the task
                // succeeded (no error_obj), (b) the error_obj was already
                // reclaimed by the fast-path scope-end disposal (won't
                // happen here — slow path means task was not done at scope
                // end, so codegen's `bz_task_is_error(t)` branch was
                // skipped), or (c) the task is a WhenAll/WhenAll<T>
                // aggregate that BORROWED its fault from an input task
                // (§30.10 / #1529: the input remains the sole owner).
                if (t->error_obj && !t->fault_borrowed) {
                    bz_dispose_class_obj(t->error_obj);
                    t->error_obj = nullptr;
                }
                bz_task_free(completedTask);
            },
            nullptr);
    }
}

// §30.10 (v0.179 / #1520): report+dispose a DisposedAccessException fault
// raised in an ABANDONED continuation — the checked-disposal-mode backstop for
// the `using var t := call();` "unsupervised" drop (spec §30.10: "any
// exceptions [an abandoned task] raises are silently swallowed — except that
// in checked disposal mode a DisposedAccessException raised in an abandoned
// continuation is first reported ... then swallowed"). `disposedAccessVtable`
// is null when the caller compiled without --checked-disposal (no report is
// possible: an unchecked build never emits the guard that raises
// DisposedAccessException, so `task->error_obj`, if any, cannot be one).
//
// Only ever called from bz_task_fire_and_forget_checked's SLOW path (task
// still running at the moment of abandonment) — a task already faulted BY
// scope-end is handled, silently, by codegen's scope-end error_obj disposal
// which runs BEFORE bz_task_fire_and_forget_checked is even called (see
// LlvmBackend_Disposal.cpp); that path already reclaims error_obj, so this
// function must never run against an already-done task or it would inspect
// (and double-dispose) a freed object.
static void bz_task_faf_report_disposed_access(bz_task_t* task,
                                                void* disposedAccessVtable) {
    if (!disposedAccessVtable || !task->is_error || !task->error_obj) return;
    void* vt = nullptr;
    memcpy(&vt, task->error_obj, sizeof(void*));
    if (vt != disposedAccessVtable) return;
    // The implementation-defined diagnostic channel (§30.10): stderr, in the
    // same spirit as the runtime's other exception-to-text renderer
    // (bz_unhandled_exception, §33.3) but WITHOUT terminating the process —
    // this backstop reports and continues; it does not fault the program.
    fputs("DisposedAccessException: an abandoned continuation accessed "
          "disposed state after its task was disposed without being awaited "
          "(checked disposal mode, spec §30.10)\n", stderr);
    fflush(stderr);
    // §30.9: the task owns its fault exception, and — being abandoned — has
    // no awaiter that will ever observe or dispose it. Dispose it here as the
    // terminal act of the swallow (bz_dispose_class_obj, defined above:
    // generic virtual Dispose() + reclaim, the same helper WhenAllOwned uses
    // for its own input faults).
    bz_dispose_class_obj(task->error_obj);
    task->error_obj = nullptr;
}

// §30.10 (v0.179 / #1520): checked-disposal-mode entry point for the `using
// var t := call();` scope-end drop of an unconsumed Task. Codegen selects
// this over the plain bz_task_fire_and_forget only when compiled with
// --checked-disposal (LlvmBackend_Disposal.cpp) — an unchecked build never
// raises DisposedAccessException in the first place (the guard that raises it
// is never emitted), so routing through the extra check there would only
// ever no-op; keeping it out of the unchecked path matches the project's
// zero-cost-when-off convention (§28.2).
void  bz_task_fire_and_forget_checked(void* tp, void* disposedAccessVtable) {
    if (!tp) return;
    auto* task = (bz_task_t*)tp;
    bool already_done;
    { std::lock_guard<std::mutex> lk(task->mtx); already_done = task->done; }
    if (already_done) {
        // NOT an abandoned continuation: the task fully settled before this
        // disposal ran (whether it succeeded, faulted synchronously, or was
        // never suspended at all), so any fault it produced was already
        // handled — silently, per the default swallow rule — by codegen's
        // scope-end error_obj disposal that runs before this call. Touching
        // error_obj here would read/double-dispose an object that disposal
        // already reclaimed. Fall straight through to the ordinary free.
        bz_task_free(tp);
    } else {
        // Slow path: task still running at the moment of abandonment — a
        // genuinely abandoned continuation. Register a continuation that,
        // when the task eventually settles, checks for the
        // DisposedAccessException backstop, then disposes any REMAINING
        // OWNED fault exception (the #1601 item 4 / #1021 arm: an
        // abandoned-then-faulted task's error_obj must be disposed by the
        // fire-and-forget continuation since no awaiter ever will), then
        // frees the task struct.
        //
        // Two-stage disposal: bz_task_faf_report_disposed_access inspects
        // error_obj for the DisposedAccessException vtable and (if matched)
        // reports + disposes + nulls error_obj. The new block below then
        // disposes any NON-DisposedAccessException fault still present
        // (error_obj != null after the report step). The fault_borrowed
        // guard skips WhenAll/WhenAll<T> aggregates that borrowed their
        // fault from an input task (§30.10 / #1529).
        bz_task_then(tp,
            [](void* completedTask, void* state) {
                auto* t = (bz_task_t*)completedTask;
                bz_task_faf_report_disposed_access(t, state);
                if (t->error_obj && !t->fault_borrowed) {
                    bz_dispose_class_obj(t->error_obj);
                    t->error_obj = nullptr;
                }
                bz_task_free(completedTask);
            },
            disposedAccessVtable);
    }
}

// ---------------------------------------------------------------------------
// CancellationToken / CancellationTokenSource — stdlib §6.1 / spec §30.8.
//
// Layout:
//   bz_cancel_state_t  — the shared cancellation flag cell.  Allocated by the
//                        source; borrowed (by pointer) by every derived token.
//                        Kept alive by a simple reference count so a token read
//                        after source disposal is safe (flag remains readable;
//                        the spec requires the source to outlive tokens but
//                        defensive runtime code should not crash on violation).
//   bz_cancel_source_t — the CancellationTokenSource object.  Owns one
//                        bz_cancel_state_t (the initial rc is 1 for the source's
//                        reference; bz_cts_token adds one more for each token
//                        handle handed out).
//
// CancellationToken handle = void* == bz_cancel_state_t*.
// CancellationToken.None  = null pointer.
// ---------------------------------------------------------------------------

// #716: Both fields are std::atomic<int32_t>.
//
// `rc` is retained on the domain thread and released on pool-worker threads
// (bz_task_delay_cancelable, bz_mutex_acquire_async, bz_semaphore_acquire_async).
// A plain int32_t rc causes lost-decrement / double-free / torn-UAF races.
//
// `cancelled` is a one-way 0→1 latch written by the domain thread
// (bz_cts_cancel / bz_cts_dispose) and read by pool workers before/after the
// sleep / blocking acquire.  A plain int32_t cross-thread flag is technically
// data-race undefined behaviour under C++20.
//
// Layout: std::atomic<int32_t> is guaranteed to have the SAME size and
// alignment as int32_t (4 bytes each) per [atomics.types.generic] p1.  Codegen
// only touches bz_cancel_state_t through opaque function calls
// (bz_ctoken_is_cancelled, bz_cts_is_cancelled, bz_cts_cancel, etc.) — no
// direct GEP offsets — so the layout is transparent to codegen and MAY carry
// extra runtime-only fields.  v0.173 / #810 adds the two linked-source adjacency
// lists below; because codegen never GEPs into the cell, growing it is safe.

struct bz_cancel_source_t {
    bz_cancel_state_t* state;
    bool disposed;
};

// Internal helpers — not exported.
// retain: relaxed is sufficient — the retained pointer is already known to the
//   caller (it was acquired while the count was >0), so no acquire is needed.
// release: acq_rel is required — the decrement must acquire all prior stores
//   (from the worker path) before the last-decrement caller frees the cell.
//   fetch_sub returns the OLD value; free when it was 1 (now 0).
void bz_cancel_state_retain(bz_cancel_state_t* s) {
    if (s) s->rc.fetch_add(1, std::memory_order_relaxed);
}
void bz_cancel_state_release(bz_cancel_state_t* s) {
    if (!s) return;
    // #716: use `delete` to match the `new` in bz_cts_alloc.
    if (s->rc.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // #1413: at last-drop no other thread references `s`, so drain any callback
        // closure still installed (never fired, never unregistered — e.g. a leaked
        // Subscription on a token that was never cancelled).  Normal paths (Cancel /
        // Dispose signal, Subscription.Dispose) already emptied the list; this is a
        // defensive leak guard so a closure's +1 is never orphaned.
        for (auto& cb : s->callbacks) bz_closure_release(cb.closure);
        delete s;
    }
}

// v0.173 / #810 (stdlib §6.1 / spec §30.8): linked cancellation composition.
// A single file-global lock guards the children/parents adjacency lists on every
// bz_cancel_state_t.  Choosing ONE global lock (over a per-cell lock) makes
// register / unregister / cascade trivially deadlock-free — there is no per-cell
// lock ordering to invert — and the cascade releases the lock before recursing
// (snapshot-and-drain).  Cancellation linking is rare, so global-lock contention
// is irrelevant.  This matches the runtime's #716 stance: cross-thread safety
// with the simplest structure that is provably correct.
static std::mutex g_cancel_link_mu;

static void bz_cancel_state_signal(bz_cancel_state_t* s);

// #1413 (stdlib §6.1): invoke a Bezel `() -> ()` callback closure from the runtime.
// A closure cell is { fn (off 0), env (off BZ_CLOSURE_ENV_OFFSET), rc, envdtor };
// the trampoline ABI passes env as the first (hidden) argument, so a unit action is
// `void fn(void* env)` (confirmed against codegen's closure-call lowering, which
// always pushes env_ptr first).  This is what lets cancellation callbacks fire on
// the timer thread and the cascade thread — where codegen is NOT the driver — the
// same way the event machinery has codegen drive the (typed) raise loop.
static void bz_cancel_invoke_closure(void* closure) {
    if (!closure) return;
    void* fn = nullptr;
    memcpy(&fn, closure, sizeof(void*));                       // fn at offset 0
    if (!fn) return;
    void* env = nullptr;
    memcpy(&env, (uint8_t*)closure + BZ_CLOSURE_ENV_OFFSET, sizeof(void*));
    typedef void (*action_fn_t)(void*);
    ((action_fn_t)fn)(env);
}

// Set the one-way cancelled latch on `s`, FIRE every registered Register callback,
// and cascade to every child linked to it (spec §30.8: a child cancels when its
// parent cancels).  UAF-safe firing: under the lock we SNAPSHOT-and-CLEAR both
// `s->children` (transferring the parent's retained refs into `kids`) and
// `s->callbacks` (transferring the callback closures' +1 into `cbs`), then fire /
// recurse OUTSIDE the lock.  Snapshotting keeps each child alive across the cascade
// even if a concurrent dispose is unregistering it, and takes exclusive ownership of
// each callback's +1 so a concurrent Subscription.Dispose (which finds the token
// already gone) cannot double-release — whoever wins the lock owns the release,
// exactly the event machinery's "release the transferred +1 exactly once" rule.
// The idempotent latch bounds recursion even for a (degenerate) link cycle, and a
// re-signalled node has already-empty lists after its first drain (so Cancel-then-
// timer, or double-Cancel, fires each callback at most once — no double effects).
static void bz_cancel_state_signal(bz_cancel_state_t* s) {
    if (!s) return;
    // #716: release-store so acquire-loaders (workers, bz_ctoken_is_cancelled)
    // observe the flag.  Idempotent — storing 1 when already 1 is harmless.
    s->cancelled.store(1, std::memory_order_release);
    std::vector<bz_cancel_state_t*> kids;
    std::vector<void*> cbs;              // #1413: snapshot of callback closures to fire
    {
        std::lock_guard<std::mutex> g(g_cancel_link_mu);
        kids.swap(s->children);          // take the parent's retained child refs
        cbs.reserve(s->callbacks.size());
        for (auto& cb : s->callbacks) cbs.push_back(cb.closure);
        s->callbacks.clear();            // take exclusive ownership of each closure's +1
    }
    // Fire callbacks first (on THIS thread — the cancelling thread per §6.1), then
    // cascade to linked children (whose own callbacks fire the same way — piece 3).
    for (void* cl : cbs) {
        bz_cancel_invoke_closure(cl);    // call fn(env) exactly once
        bz_closure_release(cl);          // drop the list's transferred +1
    }
    for (auto* k : kids) {
        bz_cancel_state_signal(k);       // cascade (idempotent)
        bz_cancel_state_release(k);      // drop the parent's ref to this child
    }
}

// Register `child` to be cancelled when `parent` cancels (spec §30.8: the child
// cancels when ANY parent cancels).  `parent` null == CancellationToken.None:
// never cancels, so no edge.  If `parent` is ALREADY cancelled, `child` fires
// SYNCHRONOUSLY (same policy as Register-on-cancelled) and no edge is recorded.
// Otherwise both directions are recorded, each RETAINED: parent→child (so the
// cascade can safely reach the child) and child→parent (so the child can
// unregister at dispose).
static void bz_cancel_link(bz_cancel_state_t* child, bz_cancel_state_t* parent) {
    if (!child || !parent) return;       // None never cancels — no edge
    bool fireNow = false;
    {
        std::lock_guard<std::mutex> g(g_cancel_link_mu);
        if (parent->cancelled.load(std::memory_order_acquire)) {
            fireNow = true;              // already cancelled — fire below, no edge
        } else {
            bz_cancel_state_retain(child);
            parent->children.push_back(child);
            bz_cancel_state_retain(parent);
            child->parents.push_back(parent);
        }
    }
    if (fireNow) bz_cancel_state_signal(child);  // synchronous already-cancelled fire
}

// Remove `child` from every parent it is registered on and drop the paired
// references — done at dispose BEFORE the child signals its own tokens (the §6.1
// dispose order: unregister first, so no parent cascade can fire into the child
// once disposal proceeds).  Removing the child from a parent's children list
// drops that parent's retained ref to the child; swapping out `child->parents`
// drops the child's retained refs to the parents.  All releases run OUTSIDE the
// lock so a last-ref `delete` never re-enters g_cancel_link_mu.
static void bz_cancel_unlink_all(bz_cancel_state_t* child) {
    if (!child) return;
    std::vector<bz_cancel_state_t*> ps;
    int child_refs_to_drop = 0;
    {
        std::lock_guard<std::mutex> g(g_cancel_link_mu);
        ps.swap(child->parents);
        for (auto* p : ps) {
            auto& kids = p->children;
            for (auto it = kids.begin(); it != kids.end(); ) {
                if (*it == child) { ++child_refs_to_drop; it = kids.erase(it); }
                else               ++it;
            }
        }
    }
    for (int i = 0; i < child_refs_to_drop; ++i) bz_cancel_state_release(child);
    for (auto* p : ps) bz_cancel_state_release(p);
}

// #1413 (stdlib §6.1): remove the Register callback identified by `tok` from `s`'s
// callback list and release its transferred +1.  Called by Subscription.Dispose
// (bz_subscription_dispose's cancel-callback branch).  If the callback has already
// fired (Cancel snapshot-and-drained the list under the same lock), the token is
// gone → this is a no-op and the fire path owns the release: the +1 is dropped
// exactly once either way.  The release runs OUTSIDE the lock so a last-drop of the
// closure (which frees its env / captured refs) never re-enters g_cancel_link_mu.
void bz_ctoken_unregister(bz_cancel_state_t* s, int64_t tok) {
    if (!s) return;
    void* cl = nullptr;
    {
        std::lock_guard<std::mutex> g(g_cancel_link_mu);
        for (auto it = s->callbacks.begin(); it != s->callbacks.end(); ++it) {
            if (it->token == tok) { cl = it->closure; s->callbacks.erase(it); break; }
        }
    }
    if (cl) bz_closure_release(cl);   // drop the transferred +1 (outside the lock)
}

// #1413 (stdlib §6.1): build the Subscription that Register hands back.  REUSES the
// event machinery's bz_subscription_t + live-set (bz_subscription_dispose routes the
// cancel_state != null case to bz_ctoken_unregister).  For a real registration the
// subscription retains the state cell so Dispose can unregister even after the source
// is gone; for the already-cancelled / None case it is a no-op handle (§6.1: "a
// Subscription whose Dispose() is a no-op").
static void* bz_make_cancel_subscription(bz_cancel_state_t* s, int64_t tok) {
    auto* sub = (bz_subscription_t*)calloc(1, sizeof(bz_subscription_t));
    sub->src = nullptr;
    sub->token = tok;
    sub->disposed = false;
    sub->cancel_state = s;
    if (s) bz_cancel_state_retain(s);   // the subscription keeps the cell alive
    { std::lock_guard<std::mutex> lk(bz_sub_live_mutex()); bz_sub_live_set().insert(sub); }
    return sub;
}

// §6.1: CancellationToken.Register(callback: () -> ()): owned Subscription.
// `token` is a bz_cancel_state_t* (null == CancellationToken.None).  `closure` is a
// `() -> ()` cell handed over with a transferred +1 (codegen lowers it exactly like
// an event handler, via lowerEventHandlerClosure).  Semantics (stdlib §6.1 verbatim):
//   - not-yet-cancelled token: store the callback; it fires when cancellation is
//     requested (on the cancelling thread — Cancel(), the CancelAfter timer thread,
//     or a linked parent's cascade thread).  Return a real Subscription.
//   - ALREADY-cancelled token (or None, which never cancels and so, C#-style,
//     returns a default no-op registration): invoke the callback SYNCHRONOUSLY on the
//     calling thread before returning, release the +1, and return a no-op Subscription.
// The already-cancelled check and the list insert happen UNDER g_cancel_link_mu, and
// bz_cancel_state_signal snapshots the list under the SAME lock AFTER setting the
// latch, so there is no lost-callback / double-fire window (see the signal comment).
extern "C" void* bz_ctoken_register(void* token, void* closure) {
    auto* s = (bz_cancel_state_t*)token;
    if (!s) {
        // CancellationToken.None: never cancels, so the callback can never fire —
        // drop its +1 now and return a no-op Subscription (matches C#'s default reg).
        bz_closure_release(closure);
        return bz_make_cancel_subscription(nullptr, 0);
    }
    bool alreadyCancelled;
    int64_t tok = 0;
    {
        std::lock_guard<std::mutex> g(g_cancel_link_mu);
        alreadyCancelled = s->cancelled.load(std::memory_order_acquire) != 0;
        if (!alreadyCancelled) {
            tok = ++s->next_cb_token;
            s->callbacks.push_back({closure, tok});   // store the transferred +1
        }
    }
    if (alreadyCancelled) {
        // §6.1: synchronous fire on the calling thread, then a no-op Subscription.
        bz_cancel_invoke_closure(closure);
        bz_closure_release(closure);   // drop the transferred +1 (fired once)
        return bz_make_cancel_subscription(nullptr, 0);
    }
    return bz_make_cancel_subscription(s, tok);
}

extern "C" void* bz_cts_alloc(void) {
    // Use `new` (not calloc) so that std::atomic constructors run.
    auto* src = new bz_cancel_source_t();
    src->state = new bz_cancel_state_t();
    src->state->cancelled.store(0, std::memory_order_relaxed);
    src->state->rc.store(1, std::memory_order_relaxed);  // source holds one reference
    src->disposed = false;
    return src;
}

extern "C" void* bz_cts_token(void* srcp) {
    // §6.1 / #647: returns the bz_cancel_state_t* as a BORROW — the CTS object
    // keeps the state alive (rc=1) for as long as the CTS itself is alive.
    // The caller does NOT retain: the token is valid until the CTS is disposed,
    // which in idiomatic Bezel always outlives the token's use (using-var scope).
    // Async slow paths (bz_mutex_acquire_async / bz_task_delay_cancelable) that
    // capture the token beyond the call boundary do their own retain/release.
    auto* src = (bz_cancel_source_t*)srcp;
    if (!src || !src->state) return nullptr;
    return src->state;   // CancellationToken handle (borrowed, not retained)
}

// #716: public wrappers for token-level retain / release.
// These expose bz_cancel_state_retain / bz_cancel_state_release (which are
// static helpers) as extern "C" functions so that runtime unit tests and any
// future public caller can exercise the same atomic paths used by the scheduler's
// async slow-paths.  token == null → no-op (mirrors the static helpers).
extern "C" void bz_cts_retain(void* token) {
    bz_cancel_state_retain((bz_cancel_state_t*)token);
}
extern "C" void bz_cts_release(void* token) {
    bz_cancel_state_release((bz_cancel_state_t*)token);
}

extern "C" int32_t bz_cts_is_cancelled(void* srcp) {
    auto* src = (bz_cancel_source_t*)srcp;
    if (!src || !src->state) return 0;
    // #716: relaxed load — the flag is a one-way latch; monotonicity is enough
    // for a single-domain query from the same thread that may call Cancel().
    return src->state->cancelled.load(std::memory_order_relaxed);
}

extern "C" void bz_cts_cancel(void* srcp) {
    auto* src = (bz_cancel_source_t*)srcp;
    if (!src || !src->state) return;
    // v0.173 / #810: set the latch AND cascade to any linked child sources.
    // Idempotent (the latch is one-way; a re-signal drains an already-empty list).
    // No upward propagation: signalling this source touches only ITS children.
    bz_cancel_state_signal(src->state);
}

// §6.1 (#1413 / #1378): CancellationTokenSource.CancelAfter(delay) — schedule the
// source to cancel after `ms` milliseconds via the dedicated BzTimer thread (g_timer,
// the #1378 substrate).  Wires to the same cancel path as Cancel() (bz_cancel_state_
// signal → latch + fire callbacks + cascade), fired on the timer thread.
//
// Reschedule (later-call-wins, matching C#'s CancelAfter which RESETS the timer): each
// call bumps the state's timer generation and captures the new value; the timer entry
// only signals if its captured generation still matches at fire time, so an earlier
// pending entry (from a prior CancelAfter) no-ops.  The docs say "set or extend the
// delay" — reschedule is the literal reading.  (Note: even without this the one-way
// latch makes a double-fire harmless; the generation is what makes a LONGER second
// delay actually supersede a shorter first one, which is the observable difference.)
//
// UAF safety: the timer entry RETAINS the state cell, so it stays readable even if the
// source is disposed before the deadline; the callback releases that ref when it fires
// (or is drained at scheduler shutdown).  Dispose bumps the generation first (below),
// so the pending entry no-ops rather than signalling a state we are tearing down.
extern "C" void bz_cts_cancel_after(void* srcp, int64_t ms) {
    auto* src = (bz_cancel_source_t*)srcp;
    if (!src || !src->state) return;
    if (src->disposed) return;                 // no scheduling on a disposed source
    bz_cancel_state_t* s = src->state;
    // Bump the generation and capture the new value (reschedule / later-call-wins).
    uint64_t myGen = s->timer_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (!g_scheduler) bz_scheduler_init(4);
    bz_timer_ensure();
    int64_t d = ms < 0 ? 0 : ms;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(d);
    bz_cancel_state_retain(s);                 // the timer entry holds a ref on the cell
    g_timer->schedule(deadline, [s, myGen]() {
        // Fire only if still the latest scheduled entry (not rescheduled, not disposed).
        if (s->timer_gen.load(std::memory_order_acquire) == myGen)
            bz_cancel_state_signal(s);         // latch + fire callbacks + cascade
        bz_cancel_state_release(s);            // drop the timer entry's ref
    });
}

extern "C" void bz_cts_dispose(void* srcp) {
    auto* src = (bz_cancel_source_t*)srcp;
    if (!src) return;
    if (src->disposed) return;  // idempotent (§28.2)
    src->disposed = true;
    // Per stdlib §6.1 destructor note: signals cancellation to any tokens
    // still observing.  Outstanding token handles retain the state cell via
    // the rc, so reads after dispose stay safe.
    if (src->state) {
        // #1413 (§6.1 / #1378): cancel any pending CancelAfter timer entry BEFORE
        // teardown — bump the generation so a not-yet-fired timer callback no-ops
        // (it will not signal a state we are about to release).  The retained state
        // ref inside that callback keeps the cell alive until it runs, so its own
        // release stays UAF-safe.
        src->state->timer_gen.fetch_add(1, std::memory_order_acq_rel);
        // v0.173 / #810 (§6.1 dispose order): FIRST unregister from every parent,
        // so no parent's cascade can fire into this child once disposal proceeds
        // (the UAF pin), THEN signal our own tokens and cascade to any children
        // we ourselves parent.  The signal also fires + releases any registered
        // Register callbacks (§6.1: the destructor releases the callback list).
        bz_cancel_unlink_all(src->state);
        bz_cancel_state_signal(src->state);
        bz_cancel_state_release(src->state);  // drop the source's reference
        src->state = nullptr;
    }
    // Use `delete` to match the `new` in bz_cts_alloc (#716).
    delete src;
}

// v0.173 / #810 (stdlib §6.1 / spec §30.8): CancellationTokenSource.CreateLinked
// / CreateLinkedAll — a linked cancellation source.  The returned child source
// cancels when ANY parent token cancels, or via its own Cancel()/CancelAfter —
// whichever first.  A parent already cancelled at creation cancels the child
// synchronously before the call returns (bz_cancel_link's already-cancelled
// fire).  CancellationToken.None parents (null handles) are ignored.
//
// Parents are CancellationToken HANDLES (== bz_cancel_state_t*), i.e. the parent
// SOURCE's state cell; linking onto the token links onto that cell, so when the
// parent source's Cancel()/Dispose() calls bz_cancel_state_signal it cascades
// into this child.  The child retains each parent's state cell (bz_cancel_link),
// so a parent source disposed while the child lives keeps its cell readable
// (matching the existing token-outlives-source defensive stance).
extern "C" void* bz_cts_create_linked(void* tokA, void* tokB) {
    auto* src = (bz_cancel_source_t*)bz_cts_alloc();
    bz_cancel_link(src->state, (bz_cancel_state_t*)tokA);
    bz_cancel_link(src->state, (bz_cancel_state_t*)tokB);
    return src;
}

extern "C" void* bz_cts_create_linked_all(void* arrp) {
    auto* src = (bz_cancel_source_t*)bz_cts_alloc();
    if (arrp) {
        auto* arr = (bz_array_t*)arrp;
        auto** elems = (bz_cancel_state_t**)arr->data;   // ptr-element array
        for (int64_t i = 0; i < arr->count; ++i)
            bz_cancel_link(src->state, elems ? elems[i] : nullptr);
    }
    return src;
}

extern "C" int32_t bz_ctoken_is_cancelled(void* token) {
    auto* s = (bz_cancel_state_t*)token;
    if (!s) return 0;  // CancellationToken.None never cancels
    // #716: acquire load — ensures the worker sees stores made before the
    // release-store in bz_cts_cancel / bz_cts_dispose.
    return s->cancelled.load(std::memory_order_acquire);
}

// Shared OperationCanceledException vtable for ThrowIfCancellationRequested.
// We use the same machinery as the built-in exception types in
// LlvmBackend_Setup.cpp: each built-in exception has a static vtable global
// (builtinVtable_["OperationCanceledException"]) and the global exception slot
// stores the object pointer.  We cannot access that vtable from the pure-C
// runtime, so instead we use a sentinel approach: a process-global
// "cancellation exception" object is allocated once and reused for every
// ThrowIfCancellationRequested.  The object layout matches the 16-byte
// built-in exception format (vtable_slot, message_slot) that LlvmBackend_Setup
// allocates for every `new OperationCanceledException()`.  We leave the vtable
// slot null here — the codegen catch machinery matches by the global exception
// slot being non-null AND by type descriptor, which it retrieves from the
// object's vtable_slot.  A null vtable would break type-based catch dispatch.
//
// Therefore we take a different approach: bz_ctoken_throw_if_cancelled sets
// a thread-local "cancellation requested" flag and returns 1; the codegen emits
// a call to bz_ctoken_materialize_exception (below) to construct the real
// OperationCanceledException object before branching to the unwind path.  This
// two-step matches the pattern used by other synthetic throws (e.g. the
// index-out-of-range guard in the array indexer path).
//
// Actually, the simpler approach that mirrors existing code is:
// bz_ctoken_throw_if_cancelled simply returns a bool; the codegen wraps it in
// a conditional block that, on true, executes `throw new OperationCanceledException()`.
// This is the cleanest design — no special materialize call needed.

extern "C" int32_t bz_ctoken_throw_if_cancelled(void* token) {
    auto* s = (bz_cancel_state_t*)token;
    if (!s) return 0;   // None — never cancels
    // #716: acquire load — same reasoning as bz_ctoken_is_cancelled.
    return s->cancelled.load(std::memory_order_acquire) ? 1 : 0;
}

// stdlib §6.7 / spec §30.8: task helpers for CancellationToken-aware Delay.
//
// bz_task_make_oce(oce_vtable): allocate a 16-byte OperationCanceledException
// object (format: {void* vtable, void* msg}; calloc zeros msg). The vtable
// pointer is supplied by codegen so the runtime does not need to know the LLVM
// vtable global's address.
void* bz_task_make_oce(void* oce_vtable) {
    void* obj = calloc(1, (size_t)BZ_EXC_OBJ_SIZE);
    memcpy(obj, &oce_vtable, sizeof(void*));  // slot 0 = vtable
    return obj;
}

// stdlib §6.7 (line 2868) / spec §30.8:
// Task.Delay(milliseconds: int, token: CancellationToken): Task.
//
// Token-aware Delay.  If the token is already cancelled at call time, returns
// an immediately-faulted task (OperationCanceledException).  Otherwise,
// schedules a timer entry and polls the token when the deadline fires: if the
// token is still live, complete normally; if cancelled, complete with OCE.
// Mid-deadline early cancellation (pre-deadline token check) is deferred —
// only the pre-call and post-deadline checks are performed, matching the prior
// pool-worker implementation.
//
// Fix #1378: migrated from pool-worker sleep_for to g_timer (BzTimer).
// oce_vtable is the vtable pointer for OperationCanceledException, supplied
// by codegen so the runtime can construct the exception object.
void* bz_task_delay_cancelable(int64_t milliseconds, void* token, void* oce_vtable) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    auto* s = (bz_cancel_state_t*)token;
    auto* task = new bz_task_t();
    // fix #713: route to the caller's domain.
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      task->domain = d; domain_ref(d); }  // per fix #632: task holds a ref on its domain
    // Pre-check: if already cancelled, fault immediately.
    // #716: acquire load so we see any release-store from bz_cts_cancel.
    if (s && s->cancelled.load(std::memory_order_acquire)) {
        void* excObj = bz_task_make_oce(oce_vtable);
        { std::lock_guard<std::mutex> lk(task->mtx);
          task->error_obj = excObj;
          task->is_error = true;
          task->done = true; }
        return task;
    }
    // Not cancelled: schedule on the dedicated timer thread (fix #1378).
    if (!g_scheduler) bz_scheduler_init(4);
    bz_timer_ensure();
    int64_t ms = milliseconds < 0 ? 0 : milliseconds;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(ms);
    // Retain the token for the timer callback so the bz_cancel_state_t stays
    // alive even if the CTS is disposed before the deadline fires (#647 / §6.1).
    bz_cancel_state_retain(s);
    g_timer->schedule(deadline, [task, s, oce_vtable]() {
        // Post-deadline check: if token was cancelled while waiting, fault.
        // #716: acquire load — synchronizes-with the release-store in bz_cts_cancel.
        if (s && s->cancelled.load(std::memory_order_acquire)) {
            void* excObj = bz_task_make_oce(oce_vtable);
            bz_cancel_state_release(s);  // release the timer's token ref
            bz_task_complete_error_obj(task, excObj);
        } else {
            bz_cancel_state_release(s);  // release the timer's token ref
            bz_task_complete(task, nullptr, 0);
        }
    });
    return task;
}

// ---------------------------------------------------------------------------
// §30.14/§30.15 (#600 phase c): Domain.Spawn and Channel<T> runtime.
//
// Design decisions (recorded per CLAUDE.md):
//   D-c1: bz_domain_spawn allocates a FRESH bz_domain_t for the spawned
//         domain (serial run-to-completion inside the child; independent of
//         the caller's domain — §30.13 isolation). The entry function and
//         argument are passed as type-erased void* and transferred (owned);
//         the result (if any) is a void*-sized heap pointer for class results
//         or a value-in-malloc for value/string results. This mirrors the
//         bz_task_complete convention already used for Task<T>.
//   D-c2: bz_channel_t is a mutex-protected deque of void*-sized items plus
//         a FIFO of pending-receiver tasks (at most one outstanding per §30.15
//         MPSC contract — a second concurrent Receive simply enqueues another
//         pending receiver task). No condvars: receivers are async.
//   D-c3: bz_channel_sender_t is a trivial ref-count wrapper around the shared
//         channel so multiple senders (via NewSender()) see the same channel.
//         Last sender release closes the channel and drains pending receivers
//         with ChannelClosedException.
//   D-c4: DomainFaultedException and ChannelClosedException are raised as
//         16-byte built-in exception objects (same layout as all builtin
//         exceptions: [vtable*, message_string*]). Their vtables are injected
//         at codegen setup time via builtinExceptionHierarchy; the runtime
//         allocates them exactly like OperationCanceledException in
//         bz_ctoken_throw_if_cancelled.
//   D-c5: For bz_domain_spawn, the entry and its args are plain void* values;
//         codegen lowers the static-function reference as a function pointer
//         and moves the argument into a heap allocation (or passes it as a
//         pointer-sized value inline). The spawned domain calls the entry via
//         the trampoline protocol: entry_fn(arg_ptr) -> result_ptr.
//         For a void entry (no return), the join OwningTask is completed with
//         no result; for an owned-result entry the result pointer IS the result.
//   D-c6: Element encoding in channel items: class types pass void* (object
//         pointer, ownership transferred); value types (int/long/double/bool)
//         are bit-cast to uint64_t and stored inline; string items are stored
//         as void* with a +1 reference counted (bz_string_retain on send,
//         bz_string_release on Dispose drain -- same as Array<string> per
//         §28.11 / §28.9). The dispose_elem callback resolves the strategy:
//         null = value/string-ref (no deep disposal), non-null = class
//         (vtable Dispose + free) -- mirrors OwningArray<T> element policy.
//   D-c7: The spawned domain uses the same BzScheduler thread pool as the main
//         domain. The pool must be started (bz_scheduler_init) before spawn;
//         bz_domain_spawn lazy-inits the pool if needed (same as bz_task_delay).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// bz_channel_t -- MPSC channel (§30.15)
//
// Lifetime via reference counting (fix #634 — UAF in bz_channel_dispose):
//
//   Every C++ holder that can dereference a bz_channel_t* acquires +1 on
//   core_refs and releases -1 when done; the last release (core_refs → 0)
//   deletes the channel core.
//
//   Holders and their acquire / release sites (per fix #634):
//     - Channel object: bz_channel_new acquires +1 (starts at 1);
//       bz_channel_dispose releases -1 after closing+draining items.
//     - ChannelSender handle: bz_channel_new_sender acquires +1;
//       bz_channel_sender_release releases -1 after closing/draining.
//
//   This prevents bz_channel_dispose from deleting the core while live
//   sender handles still hold a bz_channel_t* (the UAF root cause).
// ---------------------------------------------------------------------------

struct bz_pending_receiver_t {
    bz_task_t*     task;   // the Task that will receive the item
    bz_domain_t*   domain; // the domain to route the completion through
};

// per §6.9/§30.15 v0.174 (#809): a parked SendAsync awaiting capacity on a
// bounded channel. Mirrors bz_pending_receiver_t in reverse — it carries the
// owned item (handed into the freed slot when a Receive drains one) plus the
// Task that completes when the item is accepted (or faults with
// ChannelClosedException if the channel closes while the send is waiting).
struct bz_pending_sender_t {
    void*          item;   // the owned item awaiting a free slot
    bz_task_t*     task;   // the SendAsync Task that completes on acceptance
    bz_domain_t*   domain; // the domain to route the completion through
};

struct bz_channel_t {
    std::mutex                        mtx;
    std::deque<void*>                 items;   // buffered items (void*-sized)
    std::deque<bz_pending_receiver_t> pending; // pending receivers (FIFO)
    // v0.174 (#809): parked SendAsyncs awaiting capacity on a full bounded
    // channel, oldest-first (FIFO — completes in send order as slots free).
    std::deque<bz_pending_sender_t>   pending_senders;
    void (*dispose_elem)(void*); // null for value/string; non-null for class
    // v0.174 (#809): 0 = unbounded (the nullary ctor); >= 1 = bounded buffer
    // limit. `SendAsync` awaits capacity on a bounded channel; the sync `Send`
    // raises ChannelFullException when a bounded buffer is full.
    int64_t  capacity;
    int64_t  sender_count;
    bool     closed;
    // disposed flag: set atomically by bz_channel_dispose so concurrent Dispose
    // calls from two domains are safe (§28.2 / fix #4357 — TOCTOU double-free).
    // exchange(true) ensures exactly one caller proceeds to channel_core_release.
    std::atomic<bool> disposed{false};
    // per fix #634: refcount — Channel object holds +1, each sender holds +1;
    // last release (core_refs == 1 → 0) deletes.
    std::atomic<int> core_refs{1};
    // §30.15 / stdlib §6.9 v0.186 (#1724): single-consumer invariant for
    // `Channel<T>.ReceiveAll()`. The drain enumerable holds ch->recv_all_active
    // for its lifetime; while set, every other consumption form
    // (a fresh ReceiveAll, a Receive that begins to park, a TryReceive) raises
    // InvalidOperationException. bz_exc carries the IOE vtable at the failing
    // call site, mirroring the ChannelClosedException protocol.
    int32_t  recv_all_active;
    // Set while a `Channel.Receive()` task is parked in ch->pending — the spec
    // says beginning a ReceiveAll while a Receive task is pending also raises
    // IOE (channel is single-consumer). Cleared when the parked task completes
    // (item delivered or fault), or when the channel closes/disposes.
    int32_t  pending_recv_task;
    // If non-null, the parked drain-waiter task from a ReceiveAll MoveNextAsync
    // that found the channel empty + open. At most one exists because of the
    // single-consumer rule. bz_channel_send completes it with the item;
    // channel_close_and_drain_pending completes it with the drained signal
    // (NOT an exception — the spec text's "completes normally" requirement).
    bz_task_t* recv_all_waiter_task;
    // §30.15 / stdlib §6.9 v0.186 (#1724): pointer to the enumerator's
    // `current_yielded` void* slot, set by MoveNextAsync when it parks. On
    // Send arrival, bz_channel_send writes the item pointer through this slot
    // before completing the parked task. Void** to allow direct write.
    void** recv_all_waiter_item_slot;
};

// Forward declaration
static void channel_close_and_drain_pending(bz_channel_t* ch,
                                            void* cce_vtable);

// ---------------------------------------------------------------------------
// channel_core_ref / channel_core_release — refcount helpers (fix #634)
// ---------------------------------------------------------------------------

// Acquire a reference to a channel core.  per fix #634.
static void channel_core_ref(bz_channel_t* ch) {
    ch->core_refs.fetch_add(1, std::memory_order_relaxed);
}

// Release a reference.  per fix #634.
// When the count reaches 0, delete the channel core.
static void channel_core_release(bz_channel_t* ch) {
    if (ch->core_refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete ch;
}

extern "C"
void* bz_channel_new(void (*dispose_elem)(void*), int64_t capacity) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    auto* ch = new bz_channel_t();
    ch->dispose_elem   = dispose_elem;
    // v0.174 (#809): 0 = unbounded; >= 1 = bounded. Codegen guards capacity >= 1
    // (ArgumentException) before the call, so any value < 1 reaching here that is
    // not 0 would be a codegen bug; we still treat <= 0 as unbounded defensively.
    ch->capacity       = capacity > 0 ? capacity : 0;
    ch->sender_count   = 0;
    ch->closed         = false;
    ch->disposed       = false;
    // §30.15 / stdlib §6.9 v0.186 (#1724): drain-singleton state.
    ch->recv_all_active       = 0;
    ch->pending_recv_task     = 0;
    ch->recv_all_waiter_task  = nullptr;
    ch->recv_all_waiter_item_slot = nullptr;
    // core_refs starts at 1 (the Channel object's ref); per fix #634.
    return ch;
}

// bz_channel_sender_t -- a minted sender handle (§30.15 NewSender).
// Each handle increments the channel's sender_count; releasing the last one
// closes the channel.
struct bz_channel_sender_t {
    bz_channel_t* ch;
};

extern "C"
void* bz_channel_new_sender(void* chp) {
    auto* ch = (bz_channel_t*)chp;
    {   std::lock_guard<std::mutex> lk(ch->mtx);
        ch->sender_count++;
    }
    channel_core_ref(ch);  // per fix #634: sender holds a core ref
    auto* tx = new bz_channel_sender_t{ch};
    return tx;
}

// bz_channel_send: append item to channel; if a pending receiver exists,
// complete it immediately with the item (via the receiver's domain queue).
// Per §30.15: Send transfers ownership of the item.
// Per fix #634 / §30.15: if the channel is closed or disposed, dispose the
// item, build a ChannelClosedException object into bz_exc, and return.
// The codegen caller must call emitPendingCheck() after every Send call.
extern "C"
void bz_channel_send(void* chp, void* item, void* cce_vtable, void* cfe_vtable) {
    auto* ch = (bz_channel_t*)chp;
    bz_pending_receiver_t recv{nullptr, nullptr};
    // §30.15 / stdlib §6.9 v0.186 (#1724): a ReceiveAll drain waiter parked in
    // MoveNextAsync. If set, the new item is delivered straight to it
    // (single-consumer invariant; this branch has priority over `pending`).
    bz_task_t* drain_waiter = nullptr;
    {   std::lock_guard<std::mutex> lk(ch->mtx);
        if (ch->closed || ch->disposed) {
            // §30.15 / fix #634: send after close raises ChannelClosedException.
            // Dispose the item first (ownership was transferred to us).
            if (item && ch->dispose_elem)
                ch->dispose_elem(item);
            else if (item && !ch->dispose_elem) {
                // Value/string item: string refs need releasing; value scalars
                // are stored as inttoptr and must not be freed via free().
                // The dispose_elem==null policy means no deep disposal needed
                // (mirrors bz_channel_dispose item drain for value types).
            }
            // Build a ChannelClosedException object and store in bz_exc.
            if (cce_vtable) {
                void* excObj = calloc(1, (size_t)BZ_EXC_OBJ_SIZE);
                memcpy(excObj, &cce_vtable, sizeof(void*));
                extern thread_local void* bz_exc;
                bz_exc = excObj;
            }
            return;
        }
        if (ch->recv_all_waiter_task) {
            drain_waiter = ch->recv_all_waiter_task;
            ch->recv_all_waiter_task = nullptr;
            // Don't push to items; deliver directly to the drain waiter below.
        } else if (!ch->pending.empty()) {
            recv = ch->pending.front();
            ch->pending.pop_front();
            // §30.15 / stdlib §6.9 v0.186 (#1724): the parked-receiver count
            // (the channel-side of the single-consumer rule) drops whenever a
            // parked Receive task is delivered to. The recv_all_active gate on
            // this channel is independent — both gates release independently
            // on their respective endings.
            ch->pending_recv_task = 0;
            // Don't push item to items -- deliver directly via the task below.
        } else if (ch->capacity > 0 &&
                   (int64_t)ch->items.size() >= ch->capacity) {
            // v0.174 (#809) / §6.9 / §30.3: full bounded channel. The sync Send
            // NEVER blocks — it raises ChannelFullException. Ownership of `item`
            // was transferred to us and there is no give-back protocol (TrySend
            // is deferred), so we dispose it — the same discipline as the
            // closed-Send path above (class-T disposes; value/string drops).
            if (item && ch->dispose_elem)
                ch->dispose_elem(item);
            if (cfe_vtable) {
                void* excObj = calloc(1, (size_t)BZ_EXC_OBJ_SIZE);
                memcpy(excObj, &cfe_vtable, sizeof(void*));
                extern thread_local void* bz_exc;
                bz_exc = excObj;
            }
            return;
        } else {
            ch->items.push_back(item);
        }
    }
    if (recv.task) {
        // Complete the pending-receiver task with the item (ptr-sized result).
        // Route through the receiver's domain so the continuation is serialized.
        // bz_task_complete stores result_size bytes; for a void* item we store 8.
        bz_task_complete(recv.task, &item, sizeof(void*));
    }
    // §30.15 / stdlib §6.9 v0.186 (#1724): deliver the new item to the drain
    // waiter. The enumerator set ch->recv_all_waiter_item_slot before parking
    // to point at its own `current_yielded` void* slot; we write `item` there
    // and complete the parked task with the 1-byte status 1 ("got an item").
    if (drain_waiter) {
        if (ch->recv_all_waiter_item_slot) {
            *(void**)ch->recv_all_waiter_item_slot = item;
            ch->recv_all_waiter_item_slot = nullptr;
        }
        uint8_t one = 1;
        bz_task_complete(drain_waiter, &one, sizeof(one));
    }
}

// bz_channel_sender_send: convenience wrapper used by codegen for ChannelSender.Send.
// per fix #634: passes cce_vtable through to bz_channel_send so closed-channel
// sends raise ChannelClosedException (stored in bz_exc; codegen checks via
// emitPendingCheck after the call).
extern "C"
void bz_channel_sender_send(void* txp, void* item, void* cce_vtable,
                            void* cfe_vtable) {
    auto* tx = (bz_channel_sender_t*)txp;
    bz_channel_send(tx->ch, item, cce_vtable, cfe_vtable);
}

// bz_channel_send_async: §6.9 / §30.15 v0.174 (#809). Returns a Task that
// completes when `item` has been accepted into the channel:
//   - closed/disposed        → dispose the item, fault the task with
//                              ChannelClosedException (immediate).
//   - a receiver is parked    → deliver directly; task already-done (void).
//   - unbounded or space free  → buffer the item; task already-done (void).
//   - full bounded             → park {item, task} in the send-waiter FIFO; the
//                                task completes when a later Receive/TryReceive
//                                frees a slot and admits this item, or faults with
//                                ChannelClosedException if the channel closes
//                                first (the item is disposed on close — the
//                                buffered-undelivered-items rule).
// cce_vtable is the ChannelClosedException vtable (from codegen). The mirror of
// the receiver-parking machinery in bz_channel_receive.
extern "C"
void* bz_channel_send_async(void* chp, void* item, void* cce_vtable) {
    auto* ch = (bz_channel_t*)chp;
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    auto* task = new bz_task_t();
    // fix #713: route completion to the caller's domain (as bz_channel_receive).
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      task->domain = d; domain_ref(d); }  // task holds a ref on its domain

    bz_pending_receiver_t recv{nullptr, nullptr};
    bool complete_now  = false;  // accepted immediately (void completion)
    bool closed_fault  = false;  // channel closed → fault the task

    {   std::lock_guard<std::mutex> lk(ch->mtx);
        if (ch->closed || ch->disposed) {
            if (item && ch->dispose_elem)
                ch->dispose_elem(item);
            closed_fault = true;
        } else if (!ch->pending.empty()) {
            // A receiver is already waiting: hand the item straight across.
            recv = ch->pending.front();
            ch->pending.pop_front();
            // §30.15 / stdlib §6.9 v0.186 (#1724): parked-receiver count
            // drops when this Receive task is delivered to.
            ch->pending_recv_task = 0;
            complete_now = true;
        } else if (ch->capacity == 0 ||
                   (int64_t)ch->items.size() < ch->capacity) {
            // Unbounded, or a bounded channel with a free slot: buffer + done.
            ch->items.push_back(item);
            complete_now = true;
        } else {
            // Full bounded channel: park the sender (FIFO — completes in send
            // order as Receive frees slots).
            ch->pending_senders.push_back({item, task, task->domain});
        }
    }

    // Complete outside the lock (mirrors bz_channel_send / bz_channel_receive):
    // task completion dispatches continuations through the domain and may
    // re-enter the channel, so ch->mtx must not be held.
    if (recv.task) {
        // Deliver the item to the waiting receiver (ptr-sized result).
        bz_task_complete(recv.task, &item, sizeof(void*));
    }
    if (closed_fault) {
        void* excObj = nullptr;
        if (cce_vtable) {
            excObj = calloc(1, (size_t)BZ_EXC_OBJ_SIZE);
            memcpy(excObj, &cce_vtable, sizeof(void*));
        }
        bz_task_complete_error_obj(task, excObj);
    } else if (complete_now) {
        bz_task_complete(task, nullptr, 0);  // void Task — no result payload
    }
    // else: parked; a later Receive or a close completes it.
    return task;
}

// bz_channel_sender_send_async: codegen wrapper for ChannelSender.SendAsync.
extern "C"
void* bz_channel_sender_send_async(void* txp, void* item, void* cce_vtable) {
    auto* tx = (bz_channel_sender_t*)txp;
    return bz_channel_send_async(tx->ch, item, cce_vtable);
}

// bz_channel_receive: return a task that will complete with the next item.
// If items are available, the task is already-done (fast path). If the channel
// is empty + closed, the task faults with ChannelClosedException.
// If empty + open, the task is parked in the pending-receiver FIFO.
//
// The codegen-supplied cce_vtable is the ChannelClosedException vtable global.
extern "C"
void* bz_channel_receive(void* chp, void* cce_vtable) {
    auto* ch = (bz_channel_t*)chp;
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    auto* task = new bz_task_t();
    // fix #713: route to the caller's domain.
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      task->domain = d; domain_ref(d); }  // per fix #632: task holds a ref on its domain
    // §30.15 (v0.179, #1524): stamp the owning channel so an abandoned (never
    // consumed) Receive() task can deregister from / give an item back to it —
    // see bz_channel_receive_abandon. Acquire a core ref for the task's whole
    // lifetime (fix #634's refcounting protocol, extended here): without it, a
    // channel disposed by its own `using var` while this task's abandonment is
    // still deferred (bz_task_then routes even an already-settled outcome
    // through the domain queue, never inline — see bz_channel_receive_abandon)
    // could free the core out from under that later give-back. Released in
    // bz_task_free, symmetrically, whenever this task is finally reclaimed.
    task->recv_channel = ch;
    channel_core_ref(ch);

    void* item = nullptr;
    bool  have_item = false;
    bool  is_closed_empty = false;
    // v0.174 (#809): a sender parked on a full bounded channel, admitted into
    // the slot this Receive just freed. Its SendAsync task completes below.
    bz_pending_sender_t woken{nullptr, nullptr, nullptr};

    {   std::lock_guard<std::mutex> lk(ch->mtx);
        if (!ch->items.empty()) {
            item = ch->items.front();
            ch->items.pop_front();
            have_item = true;
            // v0.174 (#809): a slot just freed — admit the OLDEST parked sender
            // (FIFO) into it and wake its SendAsync task below. Because senders
            // only park when the buffer is full (items.size() == capacity >= 1),
            // this keeps the buffer at capacity and never reorders deliveries.
            if (!ch->pending_senders.empty()) {
                woken = ch->pending_senders.front();
                ch->pending_senders.pop_front();
                ch->items.push_back(woken.item);
            }
        } else if (ch->closed || ch->disposed) {
            is_closed_empty = true;
        } else {
            // Park in pending-receiver FIFO.
            ch->pending.push_back({task, task->domain});
            // §30.15 / stdlib §6.9 v0.186 (#1724): mark a Receive task as
            // pending so a subsequent ReceiveAll raises IOE (single-consumer).
            ch->pending_recv_task = 1;
        }
    }

    // Wake an admitted sender outside the lock (task completion may re-enter).
    if (woken.task)
        bz_task_complete(woken.task, nullptr, 0);  // void SendAsync Task

    if (have_item) {
        // Fast path: already-done (item available).
        bz_task_complete(task, &item, sizeof(void*));
    } else if (is_closed_empty) {
        // Fault: ChannelClosedException.
        void* excObj = nullptr;
        if (cce_vtable) {
            excObj = calloc(1, (size_t)BZ_EXC_OBJ_SIZE);
            memcpy(excObj, &cce_vtable, sizeof(void*));
        }
        bz_task_complete_error_obj(task, excObj);
    }
    // else: task is parked; bz_channel_send or sender_release will complete it.
    return task;
}

// channel_giveback_item: §30.15 v0.179 -- return an item to the channel's
// head after an abandoned Receive() task is found to already own one (the
// "internal give-back" the spec text describes; items remain exactly-once).
// If another receiver is already parked (FIFO -- it necessarily queued AFTER
// the one being abandoned, since that one had already been popped off
// `pending` to receive this very item), hand the item to it directly rather
// than leaving it buffered -- mirrors bz_channel_send's immediate-delivery
// path and preserves per-sender FIFO order. Otherwise requeue at the FRONT of
// `items` (the channel's head), so it is redelivered before anything sent
// after it.
//
// Bounded-channel note: an item delivered straight from a sender to a parked
// receiver (bz_channel_send's `recv.task` path, and this function's own
// direct-redelivery path) never occupies a buffer slot, so give-back can
// transiently leave `items.size()` one above `capacity` when no receiver is
// waiting to take it immediately. This does not lose or duplicate the item
// (the exactly-once invariant §30.15 requires) and self-heals on the next
// Receive/TryReceive; it is an accepted bounded cost of the give-back
// protocol, the same kind of documented edge as the displaced-admitted-
// mutable-static cost in §22.12 v0.181, rather than a capacity violation a
// caller can observe as a lost or corrupted item.
// Also guards against a narrow cross-domain race: the channel's OWN `using
// var` may dispose it (bz_channel_dispose) concurrently with this give-back —
// the core-ref bz_channel_receive/bz_task_free hold (per #1524, mirroring fix
// #634) keeps `ch` itself alive, but a Channel object already disposed has no
// Bezel-visible reference left that could ever Receive() the buffered item
// again. Disposing it here instead of queueing it into `ch->items` avoids an
// item stranded in a deque nothing will ever drain (§28.2 exactly-once).
static void channel_giveback_item(bz_channel_t* ch, void* item) {
    bz_pending_receiver_t recv{nullptr, nullptr};
    bool disposeInstead = false;
    {   std::lock_guard<std::mutex> lk(ch->mtx);
        if (ch->disposed) {
            disposeInstead = true;
        } else if (!ch->pending.empty()) {
            recv = ch->pending.front();
            ch->pending.pop_front();
        } else {
            ch->items.push_front(item);
        }
    }
    if (recv.task)
        bz_task_complete(recv.task, &item, sizeof(void*));
    else if (disposeInstead && item && ch->dispose_elem)
        ch->dispose_elem(item);
}

// bz_channel_receive_abandon: §30.15 v0.179 (#1524) -- the scope-end drop of
// a Channel<T>.Receive() task whose result was never consumed (awaited or
// `.Result`-read). Spec: "Abandoning a pending receive deregisters, never
// consumes... no message is consumed or lost... An item being handed to the
// waiter at the instant of disposal is returned to the channel's head." This
// REPLACES the general §30.5 "Disposing an unconsumed OwningTask<T>"
// dispose-continuation semantics (which would eventually DISPOSE a produced
// item, or silently drop one not yet produced) for this one task shape.
//
// Two cases, both resolved race-safely against a concurrent Send/close:
//
//   (a) The task is still parked in ch->pending: remove it from the FIFO
//       under ch->mtx and free it directly -- no item was ever dequeued into
//       it, so there is nothing to give back or dispose.
//
//   (b) The task is NOT found in ch->pending. Either it already settled (an
//       item was delivered to it earlier, or the channel closed and faulted
//       it), or a concurrent bz_channel_send/close just popped it from
//       `pending` under ch->mtx and is about to call bz_task_complete*
//       outside that lock (the narrow window bz_channel_send documents
//       between the pop and the completion call). bz_task_then resolves this
//       race exactly the way bz_task_fire_and_forget's already-done check
//       does: it inspects task->done under the task's OWN mutex and either
//       runs the continuation immediately (already settled) or registers it
//       to run the moment the racing completion call finishes -- so the
//       outcome is always "handled after settlement," never a lost update.
//       A successful outcome gives the item back (channel_giveback_item)
//       instead of disposing it; a faulted outcome (ChannelClosedException)
//       has no item to give back and is freed like any other task (its
//       error_obj, if the fault is still in flight when genuinely abandoned
//       this way, is the same documented #1021 unobserved-fault residual as
//       every other abandoned task -- not a regression introduced here).
//
// Either case frees the task struct exactly once via bz_task_free, which is
// itself a no-op on the ITEM here regardless of task->result's contents:
// result_disposer is never set for a channel-receive task (see
// LlvmBackend_Setup.cpp's using-var registration), so bz_task_free's
// disposer branch never fires for one. Case (b)'s continuation therefore
// leaves task->result untouched after reading it -- bz_task_free's own
// free(task->result) still reclaims that (8-byte, pointer-sized) buffer
// normally; only the ITEM IT POINTED TO was redirected via
// channel_giveback_item, not the buffer itself.
extern "C"
void bz_channel_receive_abandon(void* tp) {
    if (!tp) return;
    auto* task = (bz_task_t*)tp;
    auto* ch = (bz_channel_t*)task->recv_channel;
    if (!ch) { bz_task_free(tp); return; }  // defensive: should not happen

    bool removed = false;
    {   std::lock_guard<std::mutex> lk(ch->mtx);
        for (auto pit = ch->pending.begin(); pit != ch->pending.end(); ++pit) {
            if (pit->task == task) { ch->pending.erase(pit); removed = true; break; }
        }
    }
    if (removed) {
        // Case (a): deregistered before anything was ever dequeued into it.
        bz_task_free(tp);
        return;
    }

    // Case (b): settled already, or settling concurrently -- resolve via the
    // task's own completion machinery (race-safe, see doc comment above).
    bz_task_then(tp, [](void* completedTask, void* /*state*/) {
        auto* t = (bz_task_t*)completedTask;
        if (!t->is_error && t->result && t->result_size >= (int64_t)sizeof(void*)) {
            void* item = *(void**)t->result;
            channel_giveback_item((bz_channel_t*)t->recv_channel, item);
            // Deliberately NOT nulling t->result: result_disposer is always
            // null for a channel-receive task (see the doc comment above), so
            // bz_task_free's disposer branch never touches the item below --
            // leaving t->result as-is just lets bz_task_free's own
            // free(task->result) reclaim the (pointer-sized) result BUFFER
            // normally, exactly like any other task. Nulling it here would
            // leak that buffer (an earlier version of this function did).
        }
        bz_task_free(t);
    }, nullptr);
}

// bz_channel_try_receive: non-blocking receive. If an item is available,
// stores it in *out_item and returns 1. Otherwise returns 0.
// fix #667 / §30.15 / stdlib §6.9: null-guard for the disposed case.
// After Channel.Dispose() the codegen nulls the ch slot; calling methods on
// the null represents "fully disposed and cleared" = empty+closed.  Return 0
// (no item) rather than crashing — per stdlib §6.9 "TryReceive → false when
// the channel is empty or closed".
extern "C"
int32_t bz_channel_try_receive(void* chp, void** out_item) {
    if (!chp) { *out_item = nullptr; return 0; }  // fix #667: null = disposed = empty
    auto* ch = (bz_channel_t*)chp;
    // v0.174 (#809): like bz_channel_receive, a successful drain frees a slot and
    // admits the oldest parked sender (FIFO) whose SendAsync task we wake below.
    bz_pending_sender_t woken{nullptr, nullptr, nullptr};
    {   std::lock_guard<std::mutex> lk(ch->mtx);
        if (ch->items.empty()) return 0;
        *out_item = ch->items.front();
        ch->items.pop_front();
        if (!ch->pending_senders.empty()) {
            woken = ch->pending_senders.front();
            ch->pending_senders.pop_front();
            ch->items.push_back(woken.item);
        }
    }
    if (woken.task)
        bz_task_complete(woken.task, nullptr, 0);  // void SendAsync Task
    return 1;
}

// bz_channel_count: number of buffered (not-yet-received) items.
extern "C"
int64_t bz_channel_count(void* chp) {
    auto* ch = (bz_channel_t*)chp;
    std::lock_guard<std::mutex> lk(ch->mtx);
    return (int64_t)ch->items.size();
}

// bz_channel_is_closed: true after the last sender has released OR after
// Channel.Dispose().
// fix #667 / §30.15 / stdlib §6.9: null-guard for the disposed case.
// After Channel.Dispose() the codegen nulls the ch slot; a null channel
// represents "fully disposed and cleared" = closed.  Return 1 (true) per
// §30.15 "Disposing the Channel<T> itself closes it immediately."
extern "C"
int32_t bz_channel_is_closed(void* chp) {
    if (!chp) return 1;  // fix #667: null = disposed = closed
    auto* ch = (bz_channel_t*)chp;
    std::lock_guard<std::mutex> lk(ch->mtx);
    return ch->closed ? 1 : 0;
}

// Close the channel and fault any pending receivers with ChannelClosedException.
// Called under external locking (the sender_count reaches 0).
// cce_vtable may be null (best-effort, no exception object).
static void channel_close_and_drain_pending(bz_channel_t* ch,
                                            void* cce_vtable) {
    std::deque<bz_pending_receiver_t> pending;
    std::deque<bz_pending_sender_t>   senders;  // v0.174 (#809)
    // §30.15 / stdlib §6.9 v0.186 (#1724): a ReceiveAll drain waiter parked in
    // MoveNextAsync when the channel was empty + open. On close, complete it
    // NORMALLY with the "drained" sentinel (NOT ChannelClosedException — the
    // spec text's "completes normally" requirement overrides the Receive
    // faulting convention for this single waiter slot).
    bz_task_t* recv_all_waiter = nullptr;
    {   std::lock_guard<std::mutex> lk(ch->mtx);
        ch->closed = true;
        pending.swap(ch->pending);
        senders.swap(ch->pending_senders);
        recv_all_waiter = ch->recv_all_waiter_task;
        ch->recv_all_waiter_task = nullptr;
        ch->recv_all_waiter_item_slot = nullptr;  // close = no item handoff
        // §30.15 / stdlib §6.9 v0.186 (#1724): the parked-receiver count
        // drops — every pending Receive task is being faulted with
        // ChannelClosedException below.
        ch->pending_recv_task = 0;
    }
    for (auto& r : pending) {
        void* excObj = nullptr;
        if (cce_vtable) {
            excObj = calloc(1, (size_t)BZ_EXC_OBJ_SIZE);
            memcpy(excObj, &cce_vtable, sizeof(void*));
        }
        bz_task_complete_error_obj(r.task, excObj);
    }
    // §30.15 / stdlib §6.9 v0.186 (#1724): drain-waiter normal completion
    // (1-byte result = 0 = "exhausted, no item"). The task is freed when the
    // caller drops it; we don't add a result_disposer because the result is a
    // one-byte sentinel, not an owned class value.
    if (recv_all_waiter) {
        uint8_t zero = 0;
        bz_task_complete(recv_all_waiter, &zero, sizeof(zero));
    }
    // v0.174 (#809): a SendAsync still awaiting capacity when the channel closes.
    // Dispose its pending item (the buffered-undelivered-items rule — class-T
    // disposes, value/string drops) and fault its task with
    // ChannelClosedException. dispose_elem is set once at construction, so it is
    // safe to read here without the lock.
    for (auto& s : senders) {
        if (s.item && ch->dispose_elem)
            ch->dispose_elem(s.item);
        void* excObj = nullptr;
        if (cce_vtable) {
            excObj = calloc(1, (size_t)BZ_EXC_OBJ_SIZE);
            memcpy(excObj, &cce_vtable, sizeof(void*));
        }
        bz_task_complete_error_obj(s.task, excObj);
    }
}

// bz_channel_close: §30.15 (v0.334) / stdlib §6.9 (#1807) — the RECEIVER-
// initiated graceful close, `Channel<T>.Close()`.
//
// "The receiving end may close first-class: Close() puts the channel in the
// same closed state" (spec §30.15, v0.334) as disposing the last sender:
// buffered items remain receivable, subsequent `Send`/`SendAsync` raise
// ChannelClosedException (disposing the offered item), and a `SendAsync`
// pending at that instant has its item disposed by the channel and its task
// faulted — all three of which `channel_close_and_drain_pending` above
// already does. This is the ENTIRE close state transition, shared verbatim
// with the last-sender path, so the two cannot drift.
//
// WHAT MAKES Close() NOT DISPOSE (stdlib §6.9, the destructor comment and the
// "Closing" paragraph; spec §30.15, v0.334):
//
//   "Disposing the Channel<T> itself closes it immediately and disposes every
//    undelivered class-typed item in reverse insertion order"
//
// Close() closes WITHOUT that drain, and this function is deliberately the
// dispose path MINUS two things:
//
//   (1) `ch->items` is left intact — the buffered undelivered items stay the
//       channel's to release, EXACTLY ONCE, when the channel itself is later
//       disposed (`bz_channel_dispose` drains them in reverse insertion order,
//       §28.11's owned column). A Close() that drained here would DOUBLE-FREE
//       every buffered class-typed item at the channel's own disposal.
//   (2) `channel_core_release(ch)` is NOT called. The core_ref the Channel
//       OBJECT holds is dropped by `bz_channel_dispose`, not here; releasing it
//       here would free the core out from under a channel that is still open
//       for `Receive`/`TryReceive` — the #634 UAF, reintroduced.
//
// Idempotent (§30.15: "Close() is idempotent: on an already-closed channel it
// is a no-op"): `closed` is set ONLY under the lock by
// `channel_close_and_drain_pending`, which drains pending receivers / pending
// senders / the ReceiveAll waiter in the SAME critical section that sets it —
// so `closed == true` already implies there is nothing left to drain, and the
// early return below is a genuine no-op rather than a skipped obligation.
// A later sender disposal stays a no-op with respect to the closed state:
// `bz_channel_sender_release` only reaches this state machine when
// `sender_count` hits 0, and re-entering it would find the deques already
// swapped out.
extern "C"
void bz_channel_close(void* chp, void* cce_vtable) {
    if (!chp) return;  // disposed + slot-nulled = already closed (§30.15)
    auto* ch = (bz_channel_t*)chp;
    {   std::lock_guard<std::mutex> lk(ch->mtx);
        if (ch->closed) return;  // idempotent no-op (see above)
    }
    channel_close_and_drain_pending(ch, cce_vtable);
}

// bz_channel_sender_release: release one sender handle. If this was the last
// sender, close the channel. cce_vtable is the ChannelClosedException vtable.
// per fix #634: also drops the sender's core_refs reference so the channel
// core can be freed when Channel.Dispose has already run.
extern "C"
void bz_channel_sender_release(void* txp, void* cce_vtable) {
    if (!txp) return;
    auto* tx = (bz_channel_sender_t*)txp;
    bz_channel_t* ch = tx->ch;
    delete tx;

    bool last_sender = false;
    {   std::lock_guard<std::mutex> lk(ch->mtx);
        ch->sender_count--;
        if (ch->sender_count <= 0)
            last_sender = true;
    }
    if (last_sender)
        channel_close_and_drain_pending(ch, cce_vtable);
    // per fix #634: release sender's core ref; frees core when Channel.Dispose
    // already dropped its ref and this was the last outstanding sender.
    channel_core_release(ch);
}

// bz_channel_dispose: §30.15 §28.11 dispose -- close the channel and dispose
// any buffered (undelivered) items in REVERSE insertion order (§28.11 owned
// column; per §28.11 v0.135 ordered-disposal-of-owned row). Then drop the
// Channel object's core ref (fix #634: does NOT directly delete — the core
// is freed by channel_core_release when the last ref drops, which may be
// here or when the last ChannelSender releases).
// cce_vtable is passed to close pending receivers.
extern "C"
void bz_channel_dispose(void* chp, void* cce_vtable) {
    if (!chp) return;
    auto* ch = (bz_channel_t*)chp;
    if (ch->disposed.exchange(true)) return;  // already disposed → no-op (fix #4357)
    // Drain pending receivers with closed error.
    channel_close_and_drain_pending(ch, cce_vtable);
    // Dispose undelivered items in REVERSE insertion order (§28.11).
    {   std::lock_guard<std::mutex> lk(ch->mtx);
        ch->disposed = true;
        if (ch->dispose_elem) {
            // Reverse the deque items for disposal.
            std::vector<void*> rev(ch->items.rbegin(), ch->items.rend());
            ch->items.clear();
            for (void* item : rev)
                if (item) ch->dispose_elem(item);
        } else {
            ch->items.clear();
        }
    }
    // per fix #634: drop the Channel object's core ref; if no senders are
    // alive this is the last ref and the core is freed here.  If senders are
    // still alive their refs keep the core alive until they release.
    channel_core_release(ch);
}

// ---------------------------------------------------------------------------
// §30.15 / stdlib §6.9 v0.186 (#1724): Channel<T>.ReceiveAll()
//
// Implements the non-exceptional drain form: an async enumerable
// (IOwningAsyncEnumerable<T> for class-T, IAsyncEnumerable<T> for value-T)
// that yields each item with Receive's exact transfer semantics, suspends
// while the channel is empty + open, and completes NORMALLY on
// drained-and-closed. Single-consumer enforced via the channel-side flag
// recv_all_active; pending Receive task check via pending_recv_task.
//
// The runtime shape mirrors WhenEach: a vtable-backed struct implementing
// the IAsyncEnumerable<T> / IOwningAsyncEnumerable<T> protocol. The struct
// itself lives at the address the vtable points back to (vtable[0] = self),
// so the enumerable and the enumerator are the SAME object — a fresh drain
// frame is minted per call (v0.258 §28.3 calls-mint), not the channel re-
// exposed, per the spec's positive frame-identity statement.
//
// The bz_channel_t side stores three pieces of drain state:
//   recv_all_active        : 1 while an enumerator is alive (counts receivers,
//                            not waiters; cleared on dispose / finalization).
//   recv_all_waiter_task   : the parked bz_task_t from MoveNextAsync when the
//                            channel was empty + open. Sent items route here;
//                            a normal-complete-with-0 on close goes here.
//   recv_all_waiter_item_slot : pointer to the enumerator's current_yielded
//                            void* slot — bz_channel_send writes the item
//                            through this slot before completing the parked
//                            task with status=1.
//   pending_recv_task      : set while a Channel<T>.Receive() task is parked
//                            in ch->pending. A subsequent ReceiveAll raises
//                            IOE (single-consumer rule). Cleared on delivery
//                            or close-fault.
// ---------------------------------------------------------------------------

struct bz_channel_recv_all_enumerator_t {
    void*     vtable;            // slot 0 (self-reference for GetAsyncEnumerator)
    // Owner ref count. The owner (the fresh caller-owned frame) holds 1; no
    // other holders are registered today (the MoveNextAsync parked task is
    // owned by bz_channel_t's recv_all_waiter_task and completes in
    // bz_chrecvall_move_next's caller — it does not hold a ref here).
    std::atomic<int64_t> refs{1};
    // The channel (with a core_refs +1 acquired at init, released at dispose).
    bz_channel_t* ch;
    // Current item slot (set by MoveNextAsync fast path OR by bz_channel_send
    // writing through ch->recv_all_waiter_item_slot). Valid when
    // pending_advance is non-null or the synchronous fast-path completed with
    // status=1. The owning variant's TakeCurrent nulls it; the borrowing
    // variant's Current reads it as-is.
    void*     current_yielded;
    // The parked bz_task_t when MoveNextAsync found empty+open; null when no
    // advance is pending. Used to disambiguate "1 advance outstanding" from
    // "no advance outstanding" for the channel-side drain waiter hand-off
    // and for the bz_channel_t's recv_all_waiter_item_slot pointer (only set
    // when this is non-null).
    bz_task_t* pending_advance;
    // 1 if the enumerator was disposed (using-var scope-end / explicit
    // Dispose); drives the post-dispose defensive checks.
    int32_t   disposed;
    // 1 once the channel is drained-and-closed (MoveNextAsync returned false
    // in the synchronous path, or the parked task completed via close signal).
    int32_t   exhausted;
    // 1 = owning variant (TakeCurrent transfers ownership, sets null); 0 =
    // borrowing variant (Current reads directly).
    int32_t   is_owning;
};

// Forward declarations of the vtable-slot functions. Each takes a single
// `void* self` (and slot 4 a second token arg, per §30.7 v0.228 #1525).
static void  bz_chrecvall_dispose(void* self);
static void* bz_chrecvall_move_next(void* self);
static void* bz_chrecvall_current(void* self);
static void* bz_chrecvall_take_current(void* self);
static void* bz_chrecvall_self_enumerator(void* self, void* /*token*/);

static void* g_chrecvall_vtable_borrowing[5] = {nullptr, nullptr, nullptr,
                                               nullptr, nullptr};
static void* g_chrecvall_vtable_owning[5]    = {nullptr, nullptr, nullptr,
                                               nullptr, nullptr};
static std::once_flag g_chrecvall_vtables_once;

static void bz_chrecvall_vtables_init(void) {
    std::call_once(g_chrecvall_vtables_once, []() {
        g_chrecvall_vtable_borrowing[0] = g_chrecvall_vtable_borrowing;  // self
        g_chrecvall_vtable_borrowing[1] = (void*)bz_chrecvall_dispose;
        g_chrecvall_vtable_borrowing[2] = (void*)bz_chrecvall_move_next;
        g_chrecvall_vtable_borrowing[3] = (void*)bz_chrecvall_current;
        g_chrecvall_vtable_borrowing[4] = (void*)bz_chrecvall_self_enumerator;
        g_chrecvall_vtable_owning[0]    = g_chrecvall_vtable_owning;     // self
        g_chrecvall_vtable_owning[1]    = (void*)bz_chrecvall_dispose;
        g_chrecvall_vtable_owning[2]    = (void*)bz_chrecvall_move_next;
        g_chrecvall_vtable_owning[3]    = (void*)bz_chrecvall_take_current;
        g_chrecvall_vtable_owning[4]    = (void*)bz_chrecvall_self_enumerator;
    });
}

static void bz_chrecvall_release(bz_channel_recv_all_enumerator_t* e) {
    if (e->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (e->ch) channel_core_release(e->ch);
        delete e;
    }
}

// bz_chrecvall_dispose: explicit `e.Dispose()` or using-var scope-end
// disposal. Idempotent (second call is a no-op). Clears the channel-side
// recv_all_active flag so a fresh ReceiveAll can begin (single-consumer
// obligation releases on any enumeration-ending edge — normal completion,
// fault, or enumerator disposal, per the spec text). The parked advance, if
// any, is freed without affecting the channel (the receiver has stopped
// observing; the channel's "give back" protocol doesn't apply because no
// item was ever transferred to this enumerator's current_yielded slot).
static void bz_chrecvall_dispose(void* self) {
    auto* e = (bz_channel_recv_all_enumerator_t*)self;
    if (!e) return;
    bool wasDisposed = false;
    bz_task_t* parked = nullptr;
    {
        std::lock_guard<std::mutex> lk(e->ch->mtx);
        wasDisposed = (e->disposed != 0);
        if (!wasDisposed) {
            e->disposed = 1;
            // Clear recv_all_active on the channel. If a waiter is parked,
            // also clear the channel-side recv_all_waiter_task / item-slot
            // pointer (the enumerator no longer observes them); COMPLETE the
            // parked task with status=0 (exhausted) so the consumer's await
            // wakes up — mirroring WhenEach's bz_we_dispose. The consumer's
            // await reads status=0 and exits the loop; the parked task is
            // freed by the consumer's `freeTaskAfter=true` after the await
            // returns. We must NOT free the parked task here — the consumer
            // still holds a reference to it (its state machine owns the
            // advance variable).
            if (e->pending_advance &&
                e->pending_advance == e->ch->recv_all_waiter_task) {
                parked = e->pending_advance;
                e->ch->recv_all_waiter_task = nullptr;
                e->ch->recv_all_waiter_item_slot = nullptr;
            }
            e->ch->recv_all_active = 0;
        }
    }
    if (!wasDisposed) {
        if (parked) {
            uint8_t zero = 0;
            bz_task_complete(parked, &zero, sizeof(zero));
        }
        // Dispose current_yielded if it was transferred in by Send but never
        // consumed by TakeCurrent. BORROWING variant: value-typed scalars and
        // string-references don't need deep disposal. OWNING variant: dispose
        // any class-typed item we still hold.
        if (e->current_yielded && e->is_owning && e->ch->dispose_elem) {
            e->ch->dispose_elem(e->current_yielded);
            e->current_yielded = nullptr;
        }
    }
    bz_chrecvall_release(e);
}

// bz_chrecvall_move_next: returns a bz_task_t* whose 1-byte result is
//   1 → got an item, e->current_yielded is valid
//   0 → exhausted, no item
// (mirrors WhenEach's 1-byte MoveNextAsync status; the borrowed current
// value sits in e->current_yielded regardless of variant.)
//
// Three cases:
//   (a) Fast path: channel has items → take one, store in current_yielded,
//       return an already-completed task with status=1.
//   (b) Empty + closed/disposed → return an already-completed task with
//       status=0 (NORMAL completion; NO ChannelClosedException).
//   (c) Empty + open → park a bz_task_t on the channel (recv_all_waiter_task),
//       set recv_all_waiter_item_slot = &e->current_yielded so a future Send
//       can hand the item through, store the parked task in
//       e->pending_advance, and return it (still-pending). The Send or
//       close path completes the parked task with status=1 (item written
//       to *item_slot) or status=0 (close signal; *item_slot untouched).
static void* bz_chrecvall_move_next(void* self) {
    auto* e = (bz_channel_recv_all_enumerator_t*)self;
    if (!e) return nullptr;
    bz_channel_t* ch = e->ch;

    // Already disposed or exhausted → return a freshly-completed task with 0.
    if (e->disposed || e->exhausted) {
        auto* t = new bz_task_t();
        { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
          t->domain = d; domain_ref(d); }
        uint8_t zero = 0;
        bz_task_complete(t, &zero, sizeof(zero));
        return t;
    }

    auto* task = new bz_task_t();
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      task->domain = d; domain_ref(d); }

    bool fast_completed = false;
    bool exhausted_now  = false;
    // Wake admitted-sender task outside the channel lock (mirrors
    // bz_channel_receive's pattern: lock + state mutation, release, then
    // bz_task_complete which may re-enter via the domain queue).
    bz_task_t* wokenSender = nullptr;
    {
        std::lock_guard<std::mutex> lk(ch->mtx);
        if (!ch->items.empty()) {
            e->current_yielded = ch->items.front();
            ch->items.pop_front();
            fast_completed = true;
            // v0.174 (#809): like Receive, admit the oldest parked sender.
            if (!ch->pending_senders.empty()) {
                auto woken = ch->pending_senders.front();
                ch->pending_senders.pop_front();
                ch->items.push_back(woken.item);
                wokenSender = woken.task;
            }
        } else if (ch->closed || ch->disposed) {
            exhausted_now = true;
            e->exhausted = 1;
        } else {
            // Park the task as the channel's drain waiter.
            ch->recv_all_waiter_task = task;
            ch->recv_all_waiter_item_slot = &e->current_yielded;
            e->pending_advance = task;
        }
    }

    if (fast_completed) {
        uint8_t one = 1;
        bz_task_complete(task, &one, sizeof(one));
    } else if (exhausted_now) {
        uint8_t zero = 0;
        bz_task_complete(task, &zero, sizeof(zero));
    }
    // Wake any admitted sender outside the lock.
    if (wokenSender) bz_task_complete(wokenSender, nullptr, 0);
    // else: parked; bz_channel_send or channel_close_and_drain_pending
    // will complete the task.
    return task;
}

// bz_chrecvall_current: borrow the current item (borrowing variant).
static void* bz_chrecvall_current(void* self) {
    auto* e = (bz_channel_recv_all_enumerator_t*)self;
    if (!e) return nullptr;
    return e->current_yielded;
}

// bz_chrecvall_take_current: transfer ownership of the current item
// (owning variant). Nulls the slot so a subsequent TakeCurrent / Dispose
// can't double-free.
static void* bz_chrecvall_take_current(void* self) {
    auto* e = (bz_channel_recv_all_enumerator_t*)self;
    if (!e) return nullptr;
    void* item = e->current_yielded;
    e->current_yielded = nullptr;
    return item;
}

// bz_chrecvall_self_enumerator: slot 4 — the enumerable and enumerator are
// the same object, so GetAsyncEnumerator returns self. The §29.11 single-
// consumption guard is enforced by the channel-side recv_all_active flag,
// not by get_enum_called (an imported WhenEach-style flag, deferred per
// #2112).
static void* bz_chrecvall_self_enumerator(void* self, void* /*token*/) {
    return self;
}

// bz_channel_recv_all_init: §30.15 / stdlib §6.9 v0.186 (#1724).
// Allocate a fresh drain enumerable over the channel. Returns the
// IAsyncEnumerable<T>* / IOwningAsyncEnumerable<T>* to the caller (same
// pointer for both views — the difference is the vtable-slot 3 dispatch:
// borrowing Current vs owning TakeCurrent).
//
// is_owning = 1 → TakeCurrent variant (class T); the caller receives
// ownership of each yielded item via the iterator variable.
// is_owning = 0 → Current variant (value/string T); the item is a borrowed
// inttoptr-cast value or string ref.
//
// On the single-consumer violation (recv_all_active OR pending_recv_task),
// returns nullptr AND stores an IOE exception object in bz_exc. The codegen
// arm checks the return value AND calls emitPendingCheck() right after to
// surface the IOE.
extern "C"
void* bz_channel_recv_all_init(void* chp, void* ioe_vtable, int32_t is_owning) {
    auto* ch = (bz_channel_t*)chp;
    if (!ch) return nullptr;
    bz_main_domain_get();

    // Single-consumer pre-check.
    bool violation = false;
    {   std::lock_guard<std::mutex> lk(ch->mtx);
        if (ch->recv_all_active || ch->pending_recv_task)
            violation = true;
        else
            ch->recv_all_active = 1;
    }
    if (violation) {
        if (ioe_vtable) {
            void* excObj = calloc(1, (size_t)BZ_EXC_OBJ_SIZE);
            memcpy(excObj, &ioe_vtable, sizeof(void*));
            extern thread_local void* bz_exc;
            bz_exc = excObj;
        }
        return nullptr;
    }

    bz_chrecvall_vtables_init();
    auto* e = new bz_channel_recv_all_enumerator_t();
    e->vtable          = is_owning ? g_chrecvall_vtable_owning
                                   : g_chrecvall_vtable_borrowing;
    // Channel owns the enumerator's view; we hold a core ref so the channel
    // core survives until the enumerator is disposed (mirrors WhenEach's
    // refs pattern). Released in bz_chrecvall_release.
    e->ch              = ch;
    e->current_yielded = nullptr;
    e->pending_advance = nullptr;
    e->disposed        = 0;
    e->exhausted       = 0;
    e->is_owning       = is_owning;
    channel_core_ref(ch);
    return e;
}

// ---------------------------------------------------------------------------
// §30.14 (#600 phase c): Domain.Spawn
//
// bz_domain_spawn(entry_fn, arg_ptr, dfe_vtable) -- spawn a new domain that
// runs entry_fn(arg_ptr) and returns an already-allocated join task.
//
// entry_fn signature: void* (*)(void*) -- takes the arg pointer, returns
//   the result pointer (or null for void entry).  The codegen wraps the
//   actual Bezel entry method in a static trampoline that matches this ABI.
//
// arg_ptr: pointer to the (owned) argument value (pointer for class types;
//   address of a stack/heap copy for value types; null for no-arg entry).
//   Ownership is transferred to the spawned domain.
//
// dfe_vtable: DomainFaultedException vtable (injected by codegen; null if
//   the exception vtable is not available).
//
// Returns: a bz_task_t* join task (OwningTask<R> or OwningTask) that completes
//   when the spawned domain finishes (or faults).
// ---------------------------------------------------------------------------

extern "C"
void* bz_domain_spawn(void* (*entry_fn)(void*), void* arg_ptr,
                      void* dfe_vtable) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    if (!g_scheduler) bz_scheduler_init(4);

    // Allocate the join task -- the caller awaits this.
    auto* join_task = new bz_task_t();
    // The join task belongs to the CALLER's domain (main or current domain).
    // fix #713: use g_current_domain when called from inside a spawned domain.
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      join_task->domain = d; domain_ref(d); }  // per fix #632: join_task holds a ref on its domain

    // Allocate a FRESH domain for the spawned entry (§30.13 isolation).
    // per fix #632: start with refs=0; domain_drain and SpawnCtx each add their own.
    auto* child_domain = new bz_domain_t();

    struct SpawnCtx {
        bz_domain_t*   child_domain;
        bz_task_t*     join_task;
        void* (*entry_fn)(void*);
        void*          arg_ptr;
        void*          dfe_vtable;
    };
    auto* ctx = new SpawnCtx{child_domain, join_task, entry_fn, arg_ptr, dfe_vtable};
    domain_ref(child_domain);  // per fix #632: SpawnCtx holds a ref on child_domain

    // Schedule the entry as a continuation on the child domain.
    auto entry_cont = [](void* /*task_unused*/, void* state) {
        auto* sc = (SpawnCtx*)state;

        // Per D-c5: call entry; detect fault via bz_exc.
        // Save and clear bz_exc so a fault in the caller domain doesn't
        // confuse the fault check.
        extern thread_local void* bz_exc;
        void* prev_exc = bz_exc;
        bz_exc = nullptr;
        void* result = sc->entry_fn(sc->arg_ptr);
        void* fault_exc = bz_exc;
        bz_exc = prev_exc;

        if (fault_exc) {
            // The entry faulted. Per §30.14: wrap in DomainFaultedException.
            // Copy message from the original exception (offset 8 = message string*).
            void* dfe_obj = nullptr;
            if (sc->dfe_vtable) {
                void* msg_ptr = nullptr;
                if (fault_exc) {
                    // Read message string pointer from the exception's slot 8.
                    memcpy(&msg_ptr, (char*)fault_exc + 8, sizeof(void*));
                    if (msg_ptr) bz_string_retain(msg_ptr);
                }
                dfe_obj = calloc(1, (size_t)BZ_EXC_OBJ_SIZE);
                memcpy(dfe_obj, &sc->dfe_vtable, sizeof(void*));
                if (msg_ptr) memcpy((char*)dfe_obj + 8, &msg_ptr, sizeof(void*));
                // Dispose the original fault exception. This block was a
                // hand-inlined duplicate of bz_dispose_class_obj_impl (vtable
                // slot 2 Dispose, then issue 2330's CauseChain reclaim, then
                // free) — it is now routed through the §31.4-gated entry point
                // so a runtime-owned instance is skipped rather than freed.
                //
                // #5872 fix-forward: an entry function that touches a type in
                // the §22.7 failed state faults with the CACHED
                // TypeInitializationException, not a fresh one, so this free
                // was reclaiming the process-lifetime instance out from under
                // excGV — the next touch of that type re-read freed heap
                // (heap-use-after-free, measured). §31.4 makes that instance
                // runtime-owned for the life of the process, and bz_
                // dispose_class_obj is the one place that exemption is
                // honoured. The DomainFaultedException is still built and
                // completed below; only the reclaim of the ORIGINAL is skipped.
                bz_dispose_class_obj(fault_exc);
            }
            bz_task_complete_error_obj(sc->join_task, dfe_obj);
        } else {
            // Normal completion. result is the ptr-sized result (class ptr or
            // null for void entry).
            bz_task_complete(sc->join_task, &result, sizeof(void*));
        }

        // per fix #632: release the SpawnCtx's ref on the child domain.
        // domain_drain still holds its own drain-ref (acquired at domain_drain
        // entry), so the domain remains live until the drain loop exits.
        bz_domain_t* dom = sc->child_domain;
        delete sc;
        domain_release(dom);  // per fix #632: release SpawnCtx ref
    };

    domain_enqueue(child_domain, entry_cont, nullptr, ctx);
    return join_task;
}
// ---------------------------------------------------------------------------
// §6.6 / issue #974: TaskCompletionSource<T> / OwningTaskCompletionSource<T> /
// TaskCompletionSource (non-generic).
// bz_tcs_t: holds a pre-allocated inner task and an atomic completion flag.
// ---------------------------------------------------------------------------

struct bz_tcs_t {
    void*                inner_task; // bz_task_t* owned by this TCS
    std::atomic<int32_t> completed;  // 0 = pending, 1 = terminal state set
    // §28.3 / #1250: disposer for the owned result stored inside the inner task.
    // Non-null only for OwningTaskCompletionSource<T:class>. Called by
    // bz_tcs_dispose_owned_result when the TCS is disposed with a completed,
    // unconsumed owned result (the inner task's result buffer holds the class ptr).
    void (*owned_result_disposer)(void*) = nullptr;
    bz_tcs_t() : inner_task(nullptr), completed(0), owned_result_disposer(nullptr) {}
};

extern "C" {

// Allocate a new TCS struct and its inner task.
void* bz_tcs_new(void) {
    auto* tcs = new bz_tcs_t();
    tcs->inner_task = bz_task_alloc();
    return tcs;
}

// Return the inner task pointer (borrowed — TCS still owns it).
void* bz_tcs_task(void* t) {
    if (!t) return nullptr;
    return static_cast<bz_tcs_t*>(t)->inner_task;
}

// Attempt to complete with a result. Returns 1 on success, 0 if already done.
int32_t bz_tcs_try_set_result(void* t, const void* result, int64_t result_size) {
    if (!t) return 0;
    auto* tcs = static_cast<bz_tcs_t*>(t);
    int32_t expected = 0;
    if (!tcs->completed.compare_exchange_strong(expected, 1,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return 0;
    bz_task_complete(tcs->inner_task, result, result_size);
    return 1;
}

// Attempt to complete with an exception object. Returns 1 on success, 0 if done.
int32_t bz_tcs_try_set_exception(void* t, void* exc_obj) {
    if (!t) return 0;
    auto* tcs = static_cast<bz_tcs_t*>(t);
    int32_t expected = 0;
    if (!tcs->completed.compare_exchange_strong(expected, 1,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return 0;
    bz_task_complete_error_obj(tcs->inner_task, exc_obj);
    return 1;
}

// Attempt to complete with OperationCanceledException. Returns 1/0.
int32_t bz_tcs_try_set_cancelled(void* t, void* oce_vtable) {
    if (!t) return 0;
    auto* tcs = static_cast<bz_tcs_t*>(t);
    int32_t expected = 0;
    if (!tcs->completed.compare_exchange_strong(expected, 1,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return 0;
    // Build an OperationCanceledException object: 16-byte {vtable, null-msg}.
    if (oce_vtable) {
        void* exc = calloc(1, (size_t)BZ_EXC_OBJ_SIZE);
        if (exc) {
            *(void**)exc = oce_vtable;
            bz_task_complete_error_obj(tcs->inner_task, exc);
            return 1;
        }
    }
    // Fallback: plain error message (no typed exception object).
    bz_task_complete_error(tcs->inner_task, "OperationCanceledException");
    return 1;
}

// Fault the inner task with InvalidOperationException if not yet completed.
void bz_tcs_fault_if_pending(void* t, void* ioe_vtable) {
    if (!t) return;
    auto* tcs = static_cast<bz_tcs_t*>(t);
    int32_t expected = 0;
    if (!tcs->completed.compare_exchange_strong(expected, 1,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return;
    // Build an InvalidOperationException: 16-byte {vtable, null-msg}.
    if (ioe_vtable) {
        void* exc = calloc(1, (size_t)BZ_EXC_OBJ_SIZE);
        if (exc) {
            *(void**)exc = ioe_vtable;
            bz_task_complete_error_obj(tcs->inner_task, exc);
            return;
        }
    }
    bz_task_complete_error(tcs->inner_task,
        "source disposed before completion");
}

// Free the TCS struct itself. The inner task must have already been
// dealt with (fire-and-forget'd) before this call.
void bz_tcs_free(void* t) {
    if (!t) return;
    delete static_cast<bz_tcs_t*>(t);
}

// §28.3 / #1250: register the owned-result disposer for OwningTaskCompletionSource<T:class>.
// `disposer` is a JIT/AOT thunk: void(*)(void*) that calls vtable Dispose and frees
// the Bezel class object.  Called immediately after bz_tcs_new() for every
// OwningTaskCompletionSource<T:class> allocation; no-op for value-type variants.
void bz_tcs_set_owned_disposer(void* t, void (*disposer)(void*)) {
    if (!t || !disposer) return;
    static_cast<bz_tcs_t*>(t)->owned_result_disposer = disposer;
}

// §28.3 / #1250: dispose the inner task's owned class result exactly once if it was
// set but never consumed via .Task.Result or await.
// Called just before bz_task_fire_and_forget in the TCS disposal sequence (while the
// inner task is still alive and accessible via tcs->inner_task).
// Guards:
//   • owned_result_disposer == null → not an OwningTaskCompletionSource<T:class>; skip.
//   • task->is_error == true       → result was never set (TrySetException / fault); skip.
//   • task->result == null          → result was already consumed (bz_task_take_result
//                                     zeroed the buffer at the consume site); skip.
// On dispose: extracts the class pointer, frees the malloc'd result buffer, calls the
// disposer (runs the class destructor + frees the object), leaves task->result null so
// the subsequent bz_task_free calls free(nullptr) harmlessly.
void bz_tcs_dispose_owned_result(void* t) {
    if (!t) return;
    auto* tcs = static_cast<bz_tcs_t*>(t);
    if (!tcs->owned_result_disposer) return;
    auto* task = static_cast<bz_task_t*>(tcs->inner_task);
    if (!task) return;
    void* result_to_dispose = nullptr;
    void* buf_to_free = nullptr;
    {
        std::lock_guard<std::mutex> lk(task->mtx);
        if (!task->is_error && task->result &&
            task->result_size >= static_cast<int64_t>(sizeof(void*))) {
            // The result buffer holds the owned class pointer (written by bz_task_complete).
            result_to_dispose = *static_cast<void**>(task->result);
            buf_to_free = task->result;
            // Null the slot so bz_task_free's free(task->result) becomes free(nullptr).
            task->result = nullptr;
            task->result_size = 0;
        }
    }
    free(buf_to_free);
    if (result_to_dispose)
        tcs->owned_result_disposer(result_to_dispose);
}

} // extern "C" (TCS)

// ---------------------------------------------------------------------------
// stdlib §6.10 / spec §28.14 (v0.175, #808): TaskScope — the structured-
// concurrency nursery.
//
// bz_task_scope_t: an internal cancellation state cell (the same
// bz_cancel_state_t the CTS machinery uses — Token/Cancel/CancelAfter reuse
// the #810/#1413 paths verbatim), a pending-children counter, a closed flag,
// and the first-fault record. There is NO stored child list: each spawned
// child registers a completion continuation (bz_task_then → domain_enqueue,
// so it runs domain-serialized) that records the fault (first-fault-wins,
// cancelling siblings via the internal token), frees the child task, releases
// the callback closure's +1, and decrements `pending`; the LAST completion
// finalizes whichever join/dispose waiter is parked.
//
// Lifecycle (§6.10):
//   Spawn  — invoke the callback synchronously on the calling (domain) thread
//            (intra-domain; the child runs to its first suspension), retain
//            nothing beyond the closure +1 (released at child completion).
//            After close: InvalidOperationException via bz_exc (deterministic,
//            never a silent extension of the drain).
//   Join   — returns a task that completes when pending reaches 0 (drain-
//            complete: children spawned DURING the drain increment `pending`
//            before their spawner completes, so the counter never reaches 0
//            early); completing Join marks the scope CLOSED and re-raises the
//            FIRST fault (later faults were swallowed+disposed as they
//            arrived).
//   DisposeAsync (normal exit) — cancel the token, drain remaining children,
//            SWALLOW their faults (Join is the sole re-raise site), then:
//            if the scope was never Joined (missing await Join() on the
//            normal path, not compile-provable) fault the dispose task with
//            InvalidOperationException; else complete it. Frees the scope.
//   Dispose (unwind edge / sync fallback) — cancel + NON-BLOCKING zombie
//            drain: a blocking wait inside a domain continuation would
//            deadlock (bz_task_await_sync aborts on it by design), so the
//            scope is marked zombie and the LAST child completion frees it
//            (faults swallowed+disposed; no leak, no UAF — the callbacks'
//            captured boxes are runtime-managed). Residual: on this edge the
//            enclosing frame's other disposals are not ordered after the
//            drain (the §28.12 awaiting-across-unwind deferral); checked
//            disposal mode's DisposedAccessException is the §28.2 backstop.
// Quiescence (§28.14/§6.10 normative): the NORMAL-path DisposeAsync task
// completes only after every child completed and was reclaimed; no callback
// runs after it completes.
// ---------------------------------------------------------------------------

struct bz_task_scope_t {
    std::mutex mtx;
    bz_cancel_state_t* state;      // internal CTS state cell (scope holds rc 1)
    int64_t pending = 0;           // children not yet completed
    bool closed = false;           // Join completed or disposal began
    bool joined = false;           // a Join re-raise point was reached
    bool zombie = false;           // unwind-disposed; free at last completion
    bool disposed = false;         // a dispose path ran (idempotence guard)
    bool any_fault = false;        // first-fault latch
    void* first_error_obj = nullptr;   // owned here until re-raised via Join
    char* first_error_msg = nullptr;   // strdup'd when the fault carried only
                                       // a message; owned here
    std::vector<bz_task_t*> join_waiters;   // pending Join() aggregates
    bz_task_t* dispose_waiter = nullptr;    // pending DisposeAsync task
    bool dispose_missing_join = false;      // fault dispose_waiter with IOE
    void* ioe_vtable = nullptr;    // InvalidOperationException vtable (codegen)
};

// Invoke a Bezel `() -> Task` closure from the runtime: fn at offset 0, env at
// BZ_CLOSURE_ENV_OFFSET, env passed as the hidden first argument (the same ABI
// bz_cancel_invoke_closure documents for `() -> ()`); returns the task ptr.
static void* bz_scope_invoke_task_closure(void* closure) {
    if (!closure) return nullptr;
    void* fn = nullptr;
    memcpy(&fn, closure, sizeof(void*));
    if (!fn) return nullptr;
    void* env = nullptr;
    memcpy(&env, (uint8_t*)closure + BZ_CLOSURE_ENV_OFFSET, sizeof(void*));
    typedef void* (*task_fn_t)(void*);
    return ((task_fn_t)fn)(env);
}

// Dispose a swallowed Bezel exception object: dynamic dispatch through vtable
// slot 2 (Dispose) then free — the bz_domain_spawn fault-wrap pattern.
static void bz_scope_dispose_exc(void* exc) {
    if (!exc) return;
    // #5872 fix-forward: a scope.Spawn child whose entry touches a type in the
    // §22.7 failed state faults with the CACHED TypeInitializationException.
    // On the first-fault-wins path every LATER faulting child's errObj is
    // swallowed here, and that cached instance is the same one every later
    // touch of the failed type re-raises — freeing it was a heap-use-after-free
    // (measured, ASan). §31.4 makes it runtime-owned for the life of the
    // process; bz_dispose_class_obj is the existing holder of that exemption.
    // Guard the WHOLE dispose, not just the free: running the vtable Dispose
    // would set the per-instance disposed flag, so the next access would fault
    // with DisposedAccessException instead of re-raising a live instance.
    if (bz_exc_is_runtime_owned(exc)) return;
    void** vtbl = nullptr;
    memcpy(&vtbl, exc, sizeof(void*));
    if (vtbl && vtbl[2]) {
        typedef void (*DispFn)(void*);
        ((DispFn)vtbl[2])(exc);
    }
    free(exc);
}

// Build a 40-byte InvalidOperationException object with an optional message.
static void* bz_scope_make_ioe(void* ioe_vtable, const char* msg) {
    if (!ioe_vtable) return nullptr;
    void* obj = calloc(1, (size_t)BZ_EXC_OBJ_SIZE);
    if (!obj) return nullptr;
    memcpy(obj, &ioe_vtable, sizeof(void*));
    if (msg) {
        void* s = bz_make_string(msg, (int64_t)strlen(msg));
        memcpy((uint8_t*)obj + 8, &s, sizeof(void*));
    }
    return obj;
}

// Finalize helpers — called with sc->mtx HELD; they collect the completions
// to perform and the caller performs them OUTSIDE the lock (completing a task
// runs task_dispatch / domain_enqueue, which must not run under sc->mtx).
namespace {
struct BzScopeFinalActions {
    std::vector<bz_task_t*> join_ok;      // complete with success
    bz_task_t* join_fault = nullptr;      // complete with first_error_obj/msg
    void* fault_obj = nullptr;
    char* fault_msg = nullptr;            // ownership moves to this struct
    bz_task_t* dispose_task = nullptr;    // complete (or IOE-fault) this
    void* dispose_ioe = nullptr;          // non-null → fault dispose_task
    void* swallow_obj = nullptr;          // dispose path: fault to dispose
    bz_task_scope_t* free_scope = nullptr;  // zombie/dispose: free the struct
};

// Drain-complete (pending == 0): settle the parked waiters. Join waiters take
// the fault (first waiter) / success; the dispose waiter swallows any
// remaining fault and completes (or IOE-faults on the missing-Join edge).
// Marks the scope CLOSED. Caller performs the returned actions off-lock.
BzScopeFinalActions bz_scope_settle_locked(bz_task_scope_t* sc) {
    BzScopeFinalActions act;
    if (!sc->join_waiters.empty()) {
        sc->closed = true;
        sc->joined = true;
        // First waiter re-raises the first fault; the rest see success.
        act.join_fault = sc->join_waiters.front();
        if (sc->any_fault) {
            act.fault_obj = sc->first_error_obj;
            act.fault_msg = sc->first_error_msg;
            sc->first_error_obj = nullptr;
            sc->first_error_msg = nullptr;
            sc->any_fault = false;
        }
        for (std::size_t i = 1; i < sc->join_waiters.size(); ++i)
            act.join_ok.push_back(sc->join_waiters[i]);
        sc->join_waiters.clear();
    }
    if (sc->dispose_waiter) {
        // Dispose path: swallow any un-re-raised fault (dispose it), then
        // complete — or IOE-fault when Join was provably skipped at runtime.
        act.swallow_obj = sc->first_error_obj;
        sc->first_error_obj = nullptr;
        free(sc->first_error_msg);
        sc->first_error_msg = nullptr;
        sc->any_fault = false;
        act.dispose_task = sc->dispose_waiter;
        sc->dispose_waiter = nullptr;
        if (sc->dispose_missing_join)
            act.dispose_ioe = bz_scope_make_ioe(
                sc->ioe_vtable,
                "TaskScope disposed without `await Join()` on the normal "
                "path (stdlib \xC2\xA7""6.10)");
        act.free_scope = sc;
    } else if (sc->zombie) {
        // Unwind edge: swallow the fault and free at last completion.
        act.swallow_obj = sc->first_error_obj;
        sc->first_error_obj = nullptr;
        free(sc->first_error_msg);
        sc->first_error_msg = nullptr;
        sc->any_fault = false;
        act.free_scope = sc;
    }
    return act;
}

void bz_scope_perform(BzScopeFinalActions& act) {
    if (act.join_fault) {
        if (act.fault_obj)
            bz_task_complete_error_obj(act.join_fault, act.fault_obj);
        else if (act.fault_msg)
            bz_task_complete_error(act.join_fault, act.fault_msg);
        else
            bz_task_complete(act.join_fault, nullptr, 0);
        free(act.fault_msg);
        act.fault_msg = nullptr;
    }
    for (bz_task_t* t : act.join_ok) bz_task_complete(t, nullptr, 0);
    if (act.swallow_obj) bz_scope_dispose_exc(act.swallow_obj);
    if (act.dispose_task) {
        if (act.dispose_ioe)
            bz_task_complete_error_obj(act.dispose_task, act.dispose_ioe);
        else
            bz_task_complete(act.dispose_task, nullptr, 0);
    }
    if (act.free_scope) {
        // §6.10 assert-no-children destructor: the scope is reclaimed only
        // when every child has completed and been reclaimed.
        assert(act.free_scope->pending == 0 &&
               "TaskScope freed with live children");
        if (act.free_scope->state) {
            bz_cancel_state_release(act.free_scope->state);
            act.free_scope->state = nullptr;
        }
        delete act.free_scope;
    }
}
}  // namespace

extern "C" {

// stdlib §6.10: new TaskScope() — fresh internal CancellationTokenSource.
void* bz_task_scope_new(void) {
    auto* sc = new bz_task_scope_t();
    sc->state = new bz_cancel_state_t();
    sc->state->cancelled.store(0, std::memory_order_relaxed);
    sc->state->rc.store(1, std::memory_order_relaxed);
    return sc;
}

// Token property: the scope's CancellationToken handle (borrowed — the scope
// keeps the cell alive; same contract as bz_cts_token).
void* bz_task_scope_token(void* scp) {
    auto* sc = (bz_task_scope_t*)scp;
    if (!sc) return nullptr;
    return sc->state;
}

// Cancel(): cancel all children now (latch + callbacks + cascade — the same
// signal path as CancellationTokenSource.Cancel, #810).
void bz_task_scope_cancel(void* scp) {
    auto* sc = (bz_task_scope_t*)scp;
    if (!sc || !sc->state) return;
    bz_cancel_state_signal(sc->state);
}

// CancelAfter(delay): deadline for the whole scope — the #1413 timer path
// (reschedule/later-call-wins via the state cell's generation counter).
void bz_task_scope_cancel_after(void* scp, int64_t ms) {
    auto* sc = (bz_task_scope_t*)scp;
    if (!sc || !sc->state) return;
    bz_cancel_state_t* s = sc->state;
    uint64_t myGen = s->timer_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (!g_scheduler) bz_scheduler_init(4);
    bz_timer_ensure();
    int64_t d = ms < 0 ? 0 : ms;
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(d);
    bz_cancel_state_retain(s);
    g_timer->schedule(deadline, [s, myGen]() {
        if (s->timer_gen.load(std::memory_order_acquire) == myGen)
            bz_cancel_state_signal(s);
        bz_cancel_state_release(s);
    });
}

// Child-completion continuation state.
struct BzScopeChildCtx {
    bz_task_scope_t* sc;
    void* closure;  // the spawned callback's +1, released at completion
};

// Spawn(scoped body: () -> Task): start the child immediately on the current
// domain. `closure` arrives with a transferred +1 (the lowerEventHandlerClosure
// contract); it is released when the child completes. After close: IOE via
// bz_exc (codegen emits the pending-exception check after the call).
void bz_task_scope_spawn(void* scp, void* closure, void* ioe_vtable) {
    auto* sc = (bz_task_scope_t*)scp;
    if (!sc) { bz_closure_release(closure); return; }
    {
        std::lock_guard<std::mutex> lk(sc->mtx);
        if (sc->ioe_vtable == nullptr) sc->ioe_vtable = ioe_vtable;
        if (sc->closed || sc->disposed) {
            // §6.10: Spawn after close — deterministic IOE, never a silent
            // extension of the drain.
            bz_closure_release(closure);
            void* exc = bz_scope_make_ioe(
                ioe_vtable,
                "TaskScope.Spawn after the scope closed (Join returned or "
                "disposal began) (stdlib \xC2\xA7""6.10)");
            extern thread_local void* bz_exc;
            bz_exc = exc;
            return;
        }
        sc->pending += 1;  // counted BEFORE the body runs: a child that
                           // spawns a grandchild does so while its own
                           // completion is still pending, so the counter
                           // never reaches 0 mid-drain (drain completeness).
    }
    // Invoke OUTSIDE the lock: the body runs arbitrary Bezel code (to its
    // first suspension) and may itself Spawn.
    void* child = bz_scope_invoke_task_closure(closure);
    if (!child) {
        // No task produced (defensive): undo the pending count.
        bool settle = false;
        {
            std::lock_guard<std::mutex> lk(sc->mtx);
            sc->pending -= 1;
            settle = sc->pending == 0;
        }
        bz_closure_release(closure);
        if (settle) {
            BzScopeFinalActions act;
            {
                std::lock_guard<std::mutex> lk(sc->mtx);
                if (sc->pending == 0) act = bz_scope_settle_locked(sc);
            }
            bz_scope_perform(act);
        }
        return;
    }
    auto* ctx = new BzScopeChildCtx{sc, closure};
    bz_task_then(
        child,
        [](void* completedTask, void* state) {
            auto* c = (BzScopeChildCtx*)state;
            bz_task_scope_t* sc = c->sc;
            // Record the fault: first-fault-wins; the winner cancels the
            // siblings via the scope's token; later faults are swallowed
            // and their exception objects disposed (§6.10 no-aggregation).
            bool cancelSiblings = false;
            void* swallow = nullptr;
            if (bz_task_is_error(completedTask)) {
                void* errObj = bz_task_error_obj(completedTask);
                const char* errMsg = bz_task_error(completedTask);
                std::lock_guard<std::mutex> lk(sc->mtx);
                if (!sc->any_fault) {
                    sc->any_fault = true;
                    sc->first_error_obj = errObj;
                    if (!errObj && errMsg)
                        sc->first_error_msg = strdup(errMsg);
                    cancelSiblings = true;
                } else {
                    swallow = errObj;
                }
            }
            if (cancelSiblings) bz_cancel_state_signal(sc->state);
            if (swallow) bz_scope_dispose_exc(swallow);
            // The scope owned the child task; it is complete and its fault
            // (if any) has been taken/disposed — reclaim it. error_obj
            // ownership was transferred above, so clear it first (bz_task_
            // free never frees error_obj, but no dangling pointer either).
            ((bz_task_t*)completedTask)->error_obj = nullptr;
            bz_task_free(completedTask);
            bz_closure_release(c->closure);
            BzScopeFinalActions act;
            {
                std::lock_guard<std::mutex> lk(sc->mtx);
                sc->pending -= 1;
                if (sc->pending == 0) act = bz_scope_settle_locked(sc);
            }
            delete c;
            bz_scope_perform(act);
        },
        ctx);
}

// await Join(): a task that completes when every child (including children
// spawned during the drain) has completed; re-raises the FIRST fault; marks
// the scope closed on completion.
void* bz_task_scope_join(void* scp) {
    bz_main_domain_get();
    auto* sc = (bz_task_scope_t*)scp;
    auto* task = new bz_task_t();
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      task->domain = d; domain_ref(d); }
    if (!sc) { bz_task_complete(task, nullptr, 0); return task; }
    BzScopeFinalActions act;
    bool completeNow = false;
    void* faultObj = nullptr;
    char* faultMsg = nullptr;
    {
        std::lock_guard<std::mutex> lk(sc->mtx);
        if (sc->pending == 0) {
            // Drain already complete (or nothing spawned): settle inline.
            sc->closed = true;
            sc->joined = true;
            completeNow = true;
            if (sc->any_fault) {
                faultObj = sc->first_error_obj;
                faultMsg = sc->first_error_msg;
                sc->first_error_obj = nullptr;
                sc->first_error_msg = nullptr;
                sc->any_fault = false;
            }
        } else {
            sc->join_waiters.push_back(task);
        }
    }
    if (completeNow) {
        if (faultObj) bz_task_complete_error_obj(task, faultObj);
        else if (faultMsg) { bz_task_complete_error(task, faultMsg);
                             free(faultMsg); }
        else bz_task_complete(task, nullptr, 0);
    }
    bz_scope_perform(act);
    return task;
}

// DisposeAsync (normal exit): cancel, drain, swallow; IOE iff Join was never
// reached. Returns the dispose task; the scope struct is freed when it
// completes. `normal_exit` distinguishes the source-visible normal path
// (codegen's !isUnwind) from an explicit early-exit use.
// fix #4353 (stdlib §6.10 / spec §30.14, v0.298): bump `sc->state->timer_gen`
// BEFORE any signaling that can release `sc->state`. A `CancelAfter` timer
// scheduled after `Join()` retains `sc->state` with rc=2; without the
// generation bump, that timer's callback fires into the freed
// `bz_cancel_state_t` once the scope's reference is dropped. Mirrors
// `bz_cts_dispose` (line 2253).
void* bz_task_scope_dispose_async(void* scp, int32_t normal_exit,
                                  void* ioe_vtable) {
    bz_main_domain_get();
    auto* sc = (bz_task_scope_t*)scp;
    auto* task = new bz_task_t();
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      task->domain = d; domain_ref(d); }
    if (!sc) { bz_task_complete(task, nullptr, 0); return task; }
    BzScopeFinalActions act;
    {
        std::lock_guard<std::mutex> lk(sc->mtx);
        if (sc->disposed) {  // idempotent (§28.2)
            bz_task_complete(task, nullptr, 0);
            return task;
        }
        sc->disposed = true;
        sc->dispose_missing_join = normal_exit != 0 && !sc->joined;
        if (sc->ioe_vtable == nullptr) sc->ioe_vtable = ioe_vtable;
        sc->closed = true;
        sc->dispose_waiter = task;
        if (sc->state) {
            // Cancel any pending CancelAfter timer entry BEFORE teardown —
            // bump the generation so a not-yet-fired timer callback no-ops
            // (it will not signal a state we are about to release). Mirrors
            // bz_cts_dispose at line 2253 (#1413, §6.1/#1378).
            sc->state->timer_gen.fetch_add(1, std::memory_order_acq_rel);
            bz_cancel_state_signal(sc->state);  // cancel children
        }
        if (sc->pending == 0) act = bz_scope_settle_locked(sc);
    }
    bz_scope_perform(act);
    return task;
}

// Dispose (unwind edge / sync fallback): cancel + non-blocking zombie drain.
// Never raises; the scope struct is freed at the last child completion (or
// here, when no children remain).
// fix #4353 (stdlib §6.10 / spec §30.14, v0.298): same timer_gen bump as
// bz_task_scope_dispose_async — a CancelAfter timer scheduled after Join()
// retains sc->state and would otherwise fire into the freed state cell.
void bz_task_scope_dispose_unwind(void* scp) {
    auto* sc = (bz_task_scope_t*)scp;
    if (!sc) return;
    BzScopeFinalActions act;
    {
        std::lock_guard<std::mutex> lk(sc->mtx);
        if (sc->disposed) return;  // idempotent (§28.2)
        sc->disposed = true;
        sc->closed = true;
        sc->zombie = true;
        if (sc->state) {
            sc->state->timer_gen.fetch_add(1, std::memory_order_acq_rel);
            bz_cancel_state_signal(sc->state);
        }
        if (sc->pending == 0) act = bz_scope_settle_locked(sc);
    }
    bz_scope_perform(act);
}

} // extern "C" (TaskScope)

extern "C" {

// §28.3 / #1250: called after loading an owned class result from a BOUND task's result
// buffer (.Task.Result or await <bound-task>) to mark the result as consumed.
// Frees and nulls the task's result buffer so bz_tcs_dispose_owned_result (invoked at
// TCS disposal) sees null and skips disposal — avoiding a double-free of the
// now-caller-owned class object.
// Safe on any bz_task_t regardless of whether it came from a TCS: if result is already
// null (no result, or already consumed) free(nullptr) is a no-op.
//
// fix #4189 (§30.5 "Disposing an unconsumed OwningTask<T>", v0.298): `tp`'s own
// result buffer can be a BORROW-ALIAS of a value produced further up a §30.5
// Pattern A chain (bz_task_set_chained) rather than an independently-owned
// copy: every hop of a chain stores the SAME pointer VALUE in its own,
// separate result buffer, and a hop whose own body was a genuinely orphaned
// Pattern-B kind-1 extraction still carries its OWN result_disposer (fix
// #4159's "nothing else will ever dispose this" case). Nulling only `tp`'s
// own slot (as this function always has) leaves every UPSTREAM hop's result
// field populated with that same pointer and its disposer still attached, so
// when the chain is eventually reclaimed (bz_task_free's recursive
// `chained_source` walk) an upstream hop's disposer fires again on the exact
// object this call is handing to the caller — a double free (measured:
// `var f := async lambda() => await <Task<T:class> source>; using var outer
// := f(); using var w := await outer;`, heap-use-after-free in
// bz$elemDispose, freed once by `w`'s own correct disposal, freed again when
// `outer`'s later scope-end disposal walks into the chain's true origin).
//
// Fix: when `drainChain` is nonzero, once the caller has taken ownership of
// `tp`'s own result above, walk `chained_source` transitively (a chain may be
// more than one hop deep) and null each upstream hop's own `result` the same
// way — never touching `chained_source` itself, which bz_task_free still
// needs to recurse into for memory reclaim. Nulling `result` is exactly what
// makes bz_task_free's own `task->result == null` guard skip that hop's
// result_disposer once the chain is freed, mirroring how it already protects
// `tp`'s own slot. Safe without locking each hop's mutex against a
// CONCURRENT producer: every task reachable via `chained_source` has, by
// construction, already fully settled by the time `tp` (the task at the head
// of the chain) itself settled enough to be awaited here — the same "no
// other thread still touches the struct" invariant bz_task_free and
// bz_task_needs_caller_disposal already rely on for these same fields. The
// lock is still taken per hop, matching every other writer of these fields
// in this file.
//
// `drainChain` MUST be 0 when `tp`'s extracted value is instead about to be
// RE-CHAINED by an ACTIVE enclosing §30.5 chained return (`return await
// f()`) — codegen's own `asyncChainedReturn_` is exactly that
// discriminator (LlvmBackend_Async.cpp's `lowerAwait` computes
// `drainChainOnTake = takeResultAfter && !asyncChainedReturn_`). That idiom
// (the pre-existing, already-tested "route an unawaited chained task through
// an intermediate async method" pattern every `chained_return_*` /
// `async_lambda_exprbody_pattern_a_3960` test exercises) relies on the
// chain's upstream `result` fields staying POPULATED until
// `bz_task_needs_caller_disposal` (queried by the ENCLOSING body's own
// completion, right after it re-chains onto `tp`) and `bz_task_free` walk
// them later — draining here unconditionally would make that decision
// wrongly (every upstream hop already looks "handled" via a null `result`
// no disposer will ever reach) and LEAK the value instead of disposing it.
// Measured: shipping this fix with `drainChain` unconditionally true
// regressed `using_var_direct_closure_chain_double_free_4162_test.cpp`'s own
// `routed_through_async_method_control_clean_30_5` control (plus three
// sibling #4159/#4118/#3960 controls exercising the identical
// intermediate-async-method idiom) from a clean disposal to a leak, caught
// by this fix's own pre-commit sweep.
void bz_task_take_result(void* tp, int8_t drainChain) {
    if (!tp) return;
    auto* task = static_cast<bz_task_t*>(tp);
    void* buf = nullptr;
    void* chainedSrc = nullptr;
    {
        std::lock_guard<std::mutex> lk(task->mtx);
        buf = task->result;
        task->result = nullptr;
        task->result_size = 0;
        chainedSrc = drainChain ? task->chained_source : nullptr;
    }
    free(buf);
    while (chainedSrc) {
        auto* src = static_cast<bz_task_t*>(chainedSrc);
        void* srcBuf = nullptr;
        void* next = nullptr;
        {
            std::lock_guard<std::mutex> lk(src->mtx);
            srcBuf = src->result;
            src->result = nullptr;
            src->result_size = 0;
            next = src->chained_source;
        }
        free(srcBuf);
        chainedSrc = next;
    }
}

// §30.5 / #1258, #1503: an unconsumed OwningTask<T:class>'s produced result is
// now disposed via bz_task_set_result_disposer + bz_task_free (see those
// functions' doc comments) rather than an eager, call-time-only dispose —
// the eager version (formerly bz_task_dispose_owned_result, removed here) was
// a no-op for a task still running at its `using var` scope-end drop, since
// it only ever inspected the task's result buffer at that exact synchronous
// point; the disposer is now persisted on the task struct so it also fires
// for a task that settles later, via bz_task_fire_and_forget's completion
// continuation (#1503).

// ---------------------------------------------------------------------------
// stdlib §6.7 (v0.212) / proposal 0023 D1-D7: Task.WhenEach streaming fan-in.
// ---------------------------------------------------------------------------
//
// A vtable-backed runtime object that implements IAsyncEnumerable<Task> /
// IAsyncEnumerable<Task<T>> (borrowing) and IOwningAsyncEnumerable<OwningTask<T>>
// (owning) directly. Layout matches the compiler-generated async-iterator
// state machine (see emitAsyncIteratorMethod in LlvmBackend_Async.cpp):
//
//   vtable[0] = self (the enumerator address; the iterator shape's
//             GetAsyncEnumerator() returns it)
//   vtable[1] = Dispose
//   vtable[2] = MoveNextAsync
//   vtable[3] = Current (borrowing: returns the current Task<T> pointer;
//              owning: TakeCurrent — transfers ownership, nulls the slot,
//              raises IOE on miss)
//
// The struct itself lives at the address the vtable points back to. The
// bz_we_* functions below are the vtable-slot implementations.
//
// ===========================================================================
// OWNERSHIP / LIFETIME INVARIANTS  (#2095 item 5, #2096; spec §6.7 v0.212)
// ===========================================================================
//
// (1) CONTEXT LIFETIME — refcount, not raw pointer (closes #2095 defect 1,
//     the UAF-after-dispose). `refs` starts at 1 (the owner: the returned
//     enumerator) and gains +1 for EACH per-input observer registered via
//     bz_task_then (bz_we_input_completed holds it). Every observer releases
//     its ref when it fires; bz_we_dispose releases the owner ref. The struct
//     is deleted only when the last ref drops, so a completion callback that
//     fires AFTER DisposeAsync still dereferences a live context (it observes
//     `disposed` and does nothing harmful) instead of freed memory. This is
//     option (b) of the #2095 audit — chosen over a bz_task_then cancel/
//     unregister API (option a) because bz_task_free never drains a task's
//     continuation list, so there is no safe unregister seam; refcounting
//     needs no change to the task core and mirrors BzWhenAnyCtx.
//
// (2) AT-MOST-ONCE ENQUEUE — the per-index `observed[]` once-flag (checked/set
//     under `mtx`) makes each input task transition un-observed -> observed
//     exactly once, closing every #2095 duplicate-enqueue window (mirrors
//     BzWhenAnyCtx's `fired` CAS). A task is therefore pushed to `ready` OR
//     delivered to a parked advance at most once.
//
// (3) SINGLE READY-SIGNAL — MoveNextAsync (bz_we_move_next) never re-registers
//     observers; the observers are registered ONCE at init. A call either
//     consumes a `ready` entry synchronously or parks ONE `pending_advance`
//     task that the next observer completes. (#2095 item 3.)
//
// (4) PER-TASK OWNERSHIP (owning variant; borrowing owns nothing). Each input
//     OwningTask<T> is owned by exactly one location at all times, so it is
//     freed exactly once (closes #2096):
//       a. un-observed / still running  -> discharged by its own observer when
//          it completes (frees it directly if `disposed`, else routes it to b/c);
//       b. observed, awaiting a consumer -> owned by the `ready` deque;
//       c. yielded, not yet taken        -> owned by `current_yielded`;
//       d. taken by the consumer         -> owned by the caller's `owned`
//          iteration variable (TakeCurrent transferred it out).
//     bz_we_dispose frees exactly the tasks in states (b) and (c) plus the
//     array CONTAINER; it never sweeps input-array elements — that is what
//     kept states (a) and (b) disjoint and is why the old input-sweep +
//     ready-drain double-freed. Residual: a still-running task that NEVER
//     completes leaks (its observer never fires) — the accepted fire-and-
//     forget degenerate case, not a double-free.
// ===========================================================================

struct bz_task_when_each_enumerator_t {
    void* vtable;                         // slot 0 (self-reference for GetAsyncEnumerator)
    std::atomic<int64_t> refs{1};         // owner + one ref per registered observer
    std::mutex mtx;                       // guards ready/current_yielded/disposed
    std::deque<std::pair<int64_t, void*>> ready;  // FIFO of (input_arr_index, task pointer)
                                          // in completion order; ties broken in registration
                                          // order (spec §6.7 D1). The index lets TakeCurrent
                                          // (owning variant) null the input array slot so
                                          // dispose doesn't double-free the task the consumer
                                          // now owns.
    void* input_arr;                      // bz_array_t* (borrowing) or OwningArray pointer
                                          //   (owning; treated as bz_array_t* for free if
                                          //   empty-on-entry is the fast path; non-empty
                                          //   owning case is freed by the enumerator on
                                          //   dispose per §30.5)
    int64_t n;                            // number of input task slots
    int64_t completed;                    // count of input tasks that have completed
    std::vector<uint8_t> observed;        // per-index at-most-once enqueue guard
    void* current_yielded;                // most recently yielded task pointer
                                          //   (nullptr if none, or after TakeCurrent)
    int64_t current_yielded_idx;          // index in input_arr of the yielded task
                                          //   (-1 if none). On TakeCurrent (owning), the
                                          //   input_arr slot is nulled so dispose doesn't
                                          //   double-free the task the consumer now owns.
    int32_t is_typed;                     // 0 = Task (no result), 1 = Task<T> (ptr-sized result)
    int32_t is_owning;                    // 0 = borrowing (do not free input_arr), 1 = owning
                                          //   (free input_arr on dispose; bz_task_free any
                                          //   unconsumed task — its result_disposer cascades
                                          //   to the produced class value per §30.5)
    int32_t disposed;                     // 0 = live, 1 = disposed
    int32_t exhausted;                    // 0 = elements possible, 1 = all yielded
    int32_t get_enum_called;              // §29.11 single-consumption guard: 0 first
                                          //   GetAsyncEnumerator, 1 after. Currently
                                          //   UNREACHED: the builtin codegen treats the
                                          //   source as its own enumerator and never
                                          //   dispatches vtable slot 0 (bz_we_get_enumerator),
                                          //   so the affine re-enumeration reject is not yet
                                          //   runtime-enforced for WhenEach (#2112; the guard
                                          //   is retained for direct runtime users and for the
                                          //   codegen-dispatch follow-up).
    bz_task_t* pending_advance;           // non-null when a MoveNextAsync is waiting for the
                                          //   next input task to complete; advanced to
                                          //   current_yielded by the per-input completion
                                          //   callback (single-pending-advance discipline;
                                          //   `await for` desugaring produces at most one)
};

// Forward declarations for the vtable functions (the C ABI takes a single
// `void* self` argument and returns the appropriate type).
static void bz_we_dispose(void* self);
static void* bz_we_get_enumerator(void* self);
static void* bz_we_move_next(void* self);
static void* bz_we_current(void* self);
static void* bz_we_take_current(void* self);

// Per-variant vtable globals. The compiler doesn't see these — codegen
// calls bz_task_when_each_init_borrowing / bz_task_when_each_init_owning
// and treats the returned pointer as the appropriate IAsyncEnumerable<T>* /
// IOwningAsyncEnumerable<OwningTask<T>>*. The async-iterator vtable protocol
// (slot 1=Dispose, slot 2=MoveNextAsync, slot 3=Current/TakeCurrent) is what
// the consumer's `await for` desugaring dispatches through
// (LlvmBackend_Stmts.cpp lowerAwaitFor).
//
// The vtable[0] self-reference is set lazily by the init function (we don't
// know the vtable's own address until the global is created). We use a
// pattern of "vtable->slot0 == vtable" as the standard runtime trick.

static void bz_we_release(bz_task_when_each_enumerator_t* e) {
    if (e->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete e;
}

// #2096 follow-up: discharge an OwningTask<T> the enumerator still owns. Unlike
// bz_task_free, this ALSO disposes the task's OWNED fault exception first —
// bz_task_free deliberately never touches error_obj (§30.9: the fault object is
// disposed by whoever disposes the task), so a faulted WhenEachOwned input that
// is never consumed would otherwise leak its exception object. Mirrors
// bz_task_fire_and_forget's slow-path continuation and WhenAllOwned's input-
// fault handling. `fault_borrowed` is honored for symmetry (a WhenEachOwned
// input owns its own fault, so it is normally false — but a borrowed fault must
// never be disposed here). Only used on the owning path.
static void bz_we_free_owned_task(void* tp) {
    if (!tp) return;
    auto* t = (bz_task_t*)tp;
    if (t->error_obj && !t->fault_borrowed) {
        bz_dispose_class_obj(t->error_obj);
        t->error_obj = nullptr;
    }
    bz_task_free(t);
}

// bz_we_dispose: vtable slot 1. Idempotent. Marks the enumerator disposed,
// completes any parked MoveNextAsync advance (which will see disposed/exhausted
// and return false), and frees unconsumed resources.
static void bz_we_dispose(void* self) {
    auto* e = (bz_task_when_each_enumerator_t*)self;
    if (!e) return;
    std::unique_lock<std::mutex> lk(e->mtx);
    if (e->disposed) return;  // idempotent
    e->disposed = 1;
    // #2096 owning-variant teardown (D7 / §30.5). Discharge every
    // OwningTask<T> the ENUMERATOR still owns — and ONLY those — so each
    // input task is freed exactly once across the whole system (see the
    // OWNERSHIP / LIFETIME INVARIANTS block above the struct). The
    // enumerator owns a task iff it sits in `ready` (observed, not yet
    // yielded) or in `current_yielded` (yielded, not yet taken). It does
    // NOT own, and must NOT free:
    //   * un-observed / still-running input tasks — their registered
    //     observer (bz_we_input_completed) discharges them when they
    //     complete (it sees `disposed` and, for the owning variant, calls
    //     bz_task_free). Freeing them here would bz_task_free a live task
    //     whose completion continuation is still registered on it — a UAF
    //     when that continuation later fires. This is why we NEVER sweep
    //     the input array's elements on dispose (the old code did, and a
    //     completed task lived in BOTH input_arr[i] and `ready` because the
    //     observer never nulled the slot — that was the #2096 double-free).
    //   * tasks already taken by the consumer — TakeCurrent transferred
    //     ownership out and cleared `current_yielded`; the consumer's owned
    //     iteration variable discharges them.
    if (e->is_owning) {
        while (!e->ready.empty()) {
            auto p = e->ready.front();
            e->ready.pop_front();
            if (p.second) bz_we_free_owned_task(p.second);  // + fault dispose
        }
        bz_we_free_owned_task(e->current_yielded);  // null-safe; + fault dispose
        // The input array CONTAINER is ours: WhenEachOwned took ownership of
        // the OwningArray (the §28.8 owned-sink rule moved the caller's
        // disposal obligation to us — see builtinOwnedSinkArg for
        // Task.WhenEachOwned). Free the container ONLY, never its elements
        // (they are discharged via ready/current/observer above); the
        // nullptr element-disposer keeps bz_array_free from touching them.
        if (e->input_arr) {
            bz_array_free(e->input_arr, nullptr);
            e->input_arr = nullptr;
        }
    } else {
        // Borrowing variant: the enumerator owns nothing. Entries in `ready`
        // and `current_yielded` are borrows of tasks the CALLER owns; drop
        // the references without freeing. input_arr is the caller's array —
        // never freed, never nulled (the caller still borrows those slots).
        e->ready.clear();
    }
    e->current_yielded = nullptr;
    e->current_yielded_idx = -1;
    bz_task_t* pending = e->pending_advance;
    e->pending_advance = nullptr;
    lk.unlock();
    if (pending) {
        uint8_t zero = 0;
        bz_task_complete(pending, &zero, 1);
    }
    // Observers retain the context until their task has settled.
    bz_we_release(e);
}

// bz_we_get_enumerator: vtable slot 0. The compiler-generated iterator
// sets vtable[0] = address_of_vtable_global (so vtable[0] == vtable). The
// `await for` desugaring calls GetAsyncEnumerator through some path; this
// is a vtable[0] function pointer lookup, but in the standard pattern
// vtable[0] is the self-reference, so loading it gives us back the
// enumerator pointer. (Looking at emitAsyncIteratorMethod: vtElems[0] is
// set to the vtable global itself — the codegen dereferences vtable[0]
// only when explicitly reading the vtable, not as a function call.)
//
// In the runtime builtin IAsyncEnumerable<T> shape, the codegen path for
// `src.GetAsyncEnumerator()` (LlvmBackend_Calls2.cpp) recognizes the
// builtin interface and short-circuits the call by returning the
// receiver unchanged — GetAsyncEnumerator is never actually invoked at
// runtime. This vtable[0] entry therefore exists only to match the
// async-iterator vtable layout (and to keep the runtime object
// self-describing); the affine check is enforced at the MoveNextAsync
// level below.
// `bz_we_get_enumerator` enforces the affine activation rule. The builtin
// codegen currently treats the source object as its own enumerator and does
// not dispatch this slot; direct runtime users still get a deterministic
// single-use guard here (a repeated call returns null for the pending-check
// path to convert to InvalidOperationException).
static void* bz_we_get_enumerator(void* self) {
    auto* e = (bz_task_when_each_enumerator_t*)self;
    if (!e) return nullptr;
    std::lock_guard<std::mutex> lk(e->mtx);
    if (e->get_enum_called) return nullptr;
    e->get_enum_called = 1;
    return self;
}

// bz_we_move_next: vtable slot 2. Returns a Task<bool>. If a ready task is
// available, set current_yielded and complete the task with true
// synchronously (the `await` in the consumer takes the fast path).
// Otherwise, if all input tasks are done and the queue is empty, complete
// with false (exhausted). Otherwise, register a continuation that
// completes the per-advance task when the next input completes or the
// enumerator is disposed.
static void* bz_we_move_next(void* self) {
    auto* e = (bz_task_when_each_enumerator_t*)self;
    bz_task_t* adv = (bz_task_t*)bz_task_alloc();
    std::unique_lock<std::mutex> lk(e->mtx);
    if (e->disposed) {
        uint8_t zero = 0;
        bz_task_complete(adv, &zero, 1);
        return adv;
    }
    if (!e->ready.empty()) {
        auto p = e->ready.front();
        e->ready.pop_front();
        e->current_yielded = p.second;
        e->current_yielded_idx = p.first;
        uint8_t one = 1;
        bz_task_complete(adv, &one, 1);
        return adv;
    }
    if (e->exhausted || e->completed >= e->n) {
        uint8_t zero = 0;
        bz_task_complete(adv, &zero, 1);
        e->exhausted = 1;
        return adv;
    }
    if (e->pending_advance) {
        uint8_t zero = 0;
        bz_task_complete(adv, &zero, 1);
        return adv;
    }
    // Init-time observers are the sole completion registrations. They deliver
    // one completion directly to this pending advance, avoiding duplicate
    // enqueue and the bz_task_then-under-lock race (#2095; proposal 0023 D1).
    e->pending_advance = adv;
    return adv;
}

// bz_we_current: vtable slot 3 (borrowing variant). Returns the
// most-recently-yielded task pointer (a borrow of the task — the caller's
// `await t` reads through it). The current_yielded slot is cleared by
// TakeCurrent for the owning variant; for the borrowing variant the
// pointer stays valid until the next MoveNextAsync (the consumer's
// `for var t in Task.WhenEach(tasks) do await t; end` naturally drops
// the borrow before the next iteration).
static void* bz_we_current(void* self) {
    auto* e = (bz_task_when_each_enumerator_t*)self;
    std::lock_guard<std::mutex> lk(e->mtx);
    return e->current_yielded;
}

// bz_we_take_current: vtable slot 3 (owning variant). Transfers ownership
// of current_yielded to the caller; nulls the slot. The IOE-on-miss check
// (mirroring the compiler-generated TakeCurrent for IOwningAsyncEnumerable<T>)
// is performed by codegen in `emitPendingCheck()` AFTER the vtable dispatch —
// this runtime helper just returns null when no value is pending, which
// codegen treats as a TakeCurrent-miss. The IOE raise itself is a
// follow-up tightening (one ioe_vtable plumbing point).
static void* bz_we_take_current(void* self) {
    auto* e = (bz_task_when_each_enumerator_t*)self;
    std::lock_guard<std::mutex> lk(e->mtx);
    void* t = e->current_yielded;
    e->current_yielded = nullptr;
    // Owning variant: null the input array slot so the enumerator's
    // Dispose doesn't free the task the consumer now owns. The slot
    // corresponds to current_yielded_idx (the (idx, task) pair recorded
    // when the task was pushed onto the ready queue). For the borrowing
    // variant, the slot is NOT owned by us, so we MUST NOT null it —
    // the caller still borrows the same task pointer.
    if (e->is_owning && t && e->current_yielded_idx >= 0 && e->input_arr) {
        void* nullPtr = nullptr;
        bz_array_set(e->input_arr, e->current_yielded_idx,
                     &nullPtr, sizeof(void*));
    }
    e->current_yielded_idx = -1;
    return t;  // null when no pending value; codegen handles the IOE
}

// Two vtable globals: borrowing (slot 3 = Current) and owning (slot 3 =
// TakeCurrent). They share slot 0/1/2 (self/Dispose/MoveNextAsync).
//
// We use a static initializer trick: declare them as raw globals filled in
// by bz_task_when_each_vtables_init, called lazily from the init funcs.

// fix #1268 / §29.11: slot 4 is the `GetAsyncEnumerator` the `await for`
// desugaring now dispatches on EVERY async-enumerable source (it is how the
// compiler-generated async iterator hands out an independent enumerator so a
// second `await for` replays instead of use-after-freeing the first loop's
// frame). This enumerator's enumerable and enumerator are the SAME object, so
// its slot 4 returns `self` — the desugaring compares the two pointers and, on
// equality, keeps exactly the pre-#1268 ownership (the loop disposes the one
// object when it owns it, the binder does when it is named). The array must be
// 5 wide or the desugaring's slot-4 read is out of bounds.
static void* g_we_vtable_borrowing[5] = {nullptr, nullptr, nullptr, nullptr,
                                         nullptr};
static void* g_we_vtable_owning[5]    = {nullptr, nullptr, nullptr, nullptr,
                                         nullptr};
static std::once_flag g_we_vtables_once;

// vtable slot 4. NOT bz_we_get_enumerator: that one consumes the §29.11
// single-consumption guard (`get_enum_called`) and returns null on a repeat,
// which the desugaring has no null path for. Re-enumeration enforcement for
// WhenEach stays where it was — #2112.
//
// §30.7 (v0.228, #1525): slot 4's calling convention grew a second `token`
// argument (the compiler-generated async iterator's GetAsyncEnumerator(token)
// now captures it — LlvmBackend_Async.cpp). Every slot-4 producer must match the
// 2-arg ABI the call sites now use (LlvmBackend_Stmts.cpp's `await for`,
// LlvmBackend_Calls2.cpp's direct call); WhenEach has no token-aware cancellation
// path of its own (out of this fix's scope — WhenEach cancellation would be a
// separate ruling), so the argument is accepted and ignored.
static void* bz_we_self_enumerator(void* self, void* /*token*/) { return self; }

// Thread-safe one-time vtable fill. A plain `if (inited) return; inited = 1;`
// flag has a first-use data race: two threads constructing enumerators
// concurrently (WhenEach on different domains) could both see inited==0 and one
// read a half-filled vtable. std::call_once serializes the initialization and
// publishes it with the right happens-before to every caller.
static void bz_we_vtables_init(void) {
    std::call_once(g_we_vtables_once, []() {
        // vtable[0] = self-reference (set after the global's address is known).
        g_we_vtable_borrowing[0] = g_we_vtable_borrowing;  // self
        g_we_vtable_borrowing[1] = (void*)bz_we_dispose;
        g_we_vtable_borrowing[2] = (void*)bz_we_move_next;
        g_we_vtable_borrowing[3] = (void*)bz_we_current;
        g_we_vtable_borrowing[4] = (void*)bz_we_self_enumerator;  // #1268
        g_we_vtable_owning[0] = g_we_vtable_owning;
        g_we_vtable_owning[1] = (void*)bz_we_dispose;
        g_we_vtable_owning[2] = (void*)bz_we_move_next;
        g_we_vtable_owning[3] = (void*)bz_we_take_current;
        g_we_vtable_owning[4] = (void*)bz_we_self_enumerator;  // #1268
    });
}

// Per-input-task completion helper for init. Pushes the completed task
// onto the enumerator's ready queue (or delivers it directly to a parked
// advance), increments `completed`, and completes any parked MoveNextAsync
// advance. This is the per-input observer registered once at init.
struct BzWeInitSlot {
    bz_task_when_each_enumerator_t* e;
    int64_t idx;
};

static void bz_we_input_completed(void* completedTask, void* state) {
    auto* slot = (BzWeInitSlot*)state;
    auto* e = slot->e;
    bz_task_t* advance = nullptr;
    uint8_t value = 0;
    bool free_completed = false;  // owning + disposed: enumerator frees it here
    {
        std::lock_guard<std::mutex> lk(e->mtx);
        // #2095 (dup-enqueue): the per-index once-flag guarantees a given
        // input task transitions from "un-observed" to "observed" exactly
        // once, so it is enqueued/delivered at most once no matter how many
        // times its completion fires.
        if (!e->observed[slot->idx]) {
            e->observed[slot->idx] = 1;
            e->completed++;
            if (e->disposed) {
                // #2096: the enumerator was disposed before this task
                // completed. For the OWNING variant the enumerator owns the
                // task and the consumer will never take it, so discharge it
                // here — exactly once (a disposed enumerator never pushes to
                // `ready` and never sets `current_yielded`, and bz_we_dispose
                // deliberately does NOT sweep input-array elements, precisely
                // so a task with a still-registered observer is freed by that
                // observer and not by dispose). For the BORROWING variant the
                // caller owns the task; the enumerator frees nothing.
                if (e->is_owning) free_completed = true;
            } else if (e->pending_advance) {
                advance = e->pending_advance;
                e->pending_advance = nullptr;
                e->current_yielded = completedTask;
                e->current_yielded_idx = slot->idx;
                value = 1;
            } else {
                e->ready.push_back({slot->idx, completedTask});
            }
        }
    }
    if (advance) bz_task_complete(advance, &value, 1);
    // Discharge outside the enum lock: bz_we_free_owned_task may run the task's
    // result_disposer AND its fault exception's Dispose (user destructors),
    // which must not run under e->mtx. Fault dispose closes the leak of a
    // faulted-after-Dispose owning input's exception object (#2096 follow-up).
    if (free_completed) bz_we_free_owned_task(completedTask);
    delete slot;
    bz_we_release(e);
}

// bz_task_when_each_init_borrowing: build a borrowing enumerator for
// Task.WhenEach(tasks: Array<Task>) and Task.WhenEach<T>(tasks: Array<Task<T>>).
// `is_typed` selects the typed variant — both share the same runtime
// shape (the yielded element is a Task<T> pointer; the consumer awaits
// it to get the result). `task_arr` is BORROWED — the caller retains
// ownership (the §28.3 deferred-activation frame carriage binds the
// borrow to the returned iterable so the compiler enforces
// source-outlives-settlement).
void* bz_task_when_each_init_borrowing(void* task_arr, int32_t is_typed) {
    bz_we_vtables_init();
    auto* arr = (bz_array_t*)task_arr;
    int64_t n = arr ? bz_array_len(arr) : 0;
    auto* e = new bz_task_when_each_enumerator_t();
    e->vtable = g_we_vtable_borrowing;
    e->input_arr = arr;
    e->n = n;
    e->completed = 0;
    e->observed.assign(static_cast<size_t>(n), 0);
    e->current_yielded = nullptr;
    e->current_yielded_idx = -1;
    e->is_typed = is_typed;
    e->is_owning = 0;
    e->disposed = 0;
    e->exhausted = (n == 0) ? 1 : 0;
    e->get_enum_called = 0;
    e->pending_advance = nullptr;
    // Register a per-input observer. The observer pushes the completed
    // task onto the ready queue (or delivers it to a parked advance) under
    // the enum's lock.
    // Note: we register the observer on each input task even if the task
    // is already done — bz_task_then's already-done fast path (above)
    // runs the continuation synchronously, which is correct.
    for (int64_t i = 0; i < n; ++i) {
        void* t = nullptr;
        bz_array_get(arr, i, &t, sizeof(void*));
        if (!t) {
            // Defensive (#2112): a null task slot is unreachable from the
            // language — WhenEach's input is a non-nullable Array<Task<T>>, and
            // the only sized-array constructor (§4.4 `new Array<T>(count, fill)`)
            // requires a non-null fill, so the type system precludes a null
            // element. Were one ever present, skipping it WITHOUT counting it
            // toward `completed` would strand `completed` below `n` forever, so
            // the `completed >= n` exhaustion test in bz_we_move_next could
            // never fire and a parked MoveNextAsync would hang permanently.
            // Count it as already-completed (nothing to observe) so exhaustion
            // still terminates cleanly.
            e->completed++;
            continue;
        }
        auto* slot = new BzWeInitSlot{e, i};
        e->refs.fetch_add(1, std::memory_order_relaxed);
        bz_task_then(t, bz_we_input_completed, slot);
    }
    return e;
}

// bz_task_when_each_init_owning: build an owning enumerator for
// Task.WhenEachOwned<T>(owned tasks: OwningArray<OwningTask<T>>).
// Takes ownership of the input array. Unconsumed tasks are freed by
// bz_we_dispose (per §30.5: bz_task_free runs the result_disposer on
// the produced class value, then frees the task struct).
//
// `result_disposer` is T's destructor thunk (codegen-synthesized from
// OwningTask<T>'s T; null when the runtime already knows how). #2096/§30.5:
// a bz_task_from_result task (Task.FromOwnedResult) carries NO disposer of
// its own — that is normally attached at the producing local's scope-end
// drop, which never runs once the task is moved into the array. Attach the
// disposer to every input task here so bz_we_dispose can cascade disposal
// of each UNCONSUMED OwningTask<T>'s produced class value (D7). A task whose
// result is later CONSUMED via TakeCurrent+await has its result slot nulled,
// so bz_task_free skips the disposer — no double-dispose.
void* bz_task_when_each_init_owning(void* owning_task_arr,
                                    void (*result_disposer)(void*)) {
    bz_we_vtables_init();
    auto* arr = (bz_array_t*)owning_task_arr;
    int64_t n = arr ? bz_array_len(arr) : 0;
    auto* e = new bz_task_when_each_enumerator_t();
    e->vtable = g_we_vtable_owning;
    e->input_arr = arr;
    e->n = n;
    e->completed = 0;
    e->observed.assign(static_cast<size_t>(n), 0);
    e->current_yielded = nullptr;
    e->current_yielded_idx = -1;
    e->is_typed = 1;  // OwningTask<T> is always typed
    e->is_owning = 1;
    e->disposed = 0;
    e->exhausted = (n == 0) ? 1 : 0;
    e->get_enum_called = 0;
    e->pending_advance = nullptr;
    for (int64_t i = 0; i < n; ++i) {
        void* t = nullptr;
        bz_array_get(arr, i, &t, sizeof(void*));
        if (!t) {
            // Defensive (#2112): a null slot is unreachable from the language
            // (OwningArray<OwningTask<T>> is non-nullable). Count it toward
            // `completed` — see the borrowing init above — so a hypothetical
            // null can never strand exhaustion and hang a parked MoveNextAsync.
            e->completed++;
            continue;
        }
        // Attach T's result disposer so an unconsumed task's class result is
        // disposed exactly once when bz_we_dispose frees it (§30.5 / D7).
        if (result_disposer) bz_task_set_result_disposer(t, result_disposer);
        auto* slot = new BzWeInitSlot{e, i};
        e->refs.fetch_add(1, std::memory_order_relaxed);
        bz_task_then(t, bz_we_input_completed, slot);
    }
    return e;
}

} // extern "C" (bz_task_take_result / Task.WhenEach)
