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
// Inline fast-path in bz_task_then (already-done):
//   The caller of bz_task_then is itself a continuation running inside the
//   domain's active runner.  Firing the next continuation *inline* (on the
//   same C call stack) would be correct for the memory-safety contract, but
//   it would (a) recursively deepen the C stack for every already-done await
//   chain and (b) skip the domain-queue round-trip that lets other
//   continuations that arrived concurrently interleave at the right points.
//   Routing through domain_enqueue is the correct run-to-completion
//   implementation: the current continuation returns to the domain drain loop,
//   which finds the newly-enqueued item and runs it next — a domain-level tail
//   call without stack growth.
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

// #1553 / §22.13 (v0.181): a stable, non-zero per-OS-thread identifier used by
// the static-initialization re-entrancy backstop (emitEnsureInitFns) to
// distinguish a SAME-thread re-entrant touch of an in-progress initializer —
// which must raise a runtime-synthesized TypeInitializationException — from a
// cross-domain touch on a different pool thread, which is the §22.12 rule-1
// blocking case and is NOT synthesized here (that cross-domain waits-for /
// blocking half is deliberately out of scope; tracked at #1391). The id is
// drawn from a process-global atomic counter on first use per thread, so it is
// unique per live thread and never 0 (0 marks "no owner recorded" in the
// per-type owner-thread global).
extern "C" uint64_t bz_current_thread_id() {
    static std::atomic<uint64_t> next{1};
    static thread_local uint64_t id =
        next.fetch_add(1, std::memory_order_relaxed);
    return id;
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
// per §6.8 / #652: Domain instance — bz_domain_alloc / bz_domain_get_name.
//
// stdlib §6.8 documents Domain as a static-class for Domain.Spawn.  The probe
// suite exercises `new Domain("test")` and `d.Name` as an instance-creation
// path that lets programs name a domain for diagnostics.  The instance is an
// owned heap pointer (IDisposable) backed by a bz_domain_t with a stored name.
//
// bz_domain_alloc(name_str: bz_string*) → bz_domain_t*
//   Allocates a fresh bz_domain_t, copies the name from the bz_string argument
//   (width-1 assumed for a domain name; falls back to "" for null/non-ASCII),
//   and returns the pointer with refs=1 (the Bezel `using var` holds this ref).
//
// bz_domain_dispose(dom: bz_domain_t*) — IDisposable.Dispose() for a named
//   Domain instance.  Releases the caller's ref (acquired by bz_domain_alloc).
//   When refs drops to 0 the domain is deleted (destructor frees the name).
//
// bz_domain_get_name(dom: bz_domain_t*) → bz_string*
//   Returns a fresh heap bz_string (rc=1) containing the domain's name.
//   Caller owns the returned string (bz_string_release when done).
// ---------------------------------------------------------------------------

extern "C" void* bz_domain_alloc(const void* name_str_p) {
    bz_domain_t* dom = new bz_domain_t();
    domain_ref(dom);  // the `using var` in Bezel holds this ref
    // Copy name from bz_string (§4.6 layout: width-1 ASCII name).
    const char* raw = "";
    int64_t     rlen = 0;
    if (name_str_p) {
        const bz_string_hdr* h = (const bz_string_hdr*)name_str_p;
        if (h->data && h->width == 1) { raw = h->data; rlen = h->len; }
    }
    dom->name = (char*)malloc((size_t)rlen + 1);
    memcpy(dom->name, raw, (size_t)rlen);
    dom->name[rlen] = '\0';
    return dom;
}

extern "C" void bz_domain_dispose(void* dom_p) {
    if (!dom_p) return;
    domain_release((bz_domain_t*)dom_p);
}

// bz_domain_get_name: return a fresh bz_string (rc=1) with the domain's name.
extern "C" void* bz_domain_get_name(const void* dom_p) {
    const char* nm = "";
    int64_t     nlen = 0;
    if (dom_p) {
        const bz_domain_t* dom = (const bz_domain_t*)dom_p;
        if (dom->name) { nm = dom->name; nlen = (int64_t)strlen(nm); }
    }
    char* buf = (char*)malloc((size_t)nlen + 1);
    memcpy(buf, nm, (size_t)nlen);
    buf[nlen] = '\0';
    bz_string_hdr* sh = (bz_string_hdr*)calloc(1, sizeof(bz_string_hdr));
    sh->data  = buf;
    sh->len   = nlen;
    sh->width = 1;
    sh->rc    = 1;  // caller owns this reference (§4.6)
    return sh;
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

// §30.13 (#618): bz_task_then routes the continuation through the task's domain
// queue regardless of the already-done fast path.
//
// When already_done is true the caller is executing as the domain's active
// runner (it is itself a continuation that just ran and is chaining the next
// step via bz_task_then).  Routing through domain_enqueue means: the current
// continuation returns to the domain drain loop, which finds the newly-enqueued
// item and runs it next — a domain-level tail call without stack growth.  This
// is the correct run-to-completion implementation: other continuations that
// arrived concurrently (e.g. from a parallel timer completion on a sub-task)
// can be interleaved at the right points between the already-done items.
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
    if (already_done)
        domain_enqueue(dom, cont, task, state);
}

void bz_task_free(void* tp) {
    auto* task = (bz_task_t*)tp;
    free(task->result);
    free(task->error_msg);
    // §30.5 / #1332 (v0.162): dispose the chained Pattern A source task (if any)
    // transitively — the source owns the result that the outer task borrowed.
    void* chainedSrc = task->chained_source;
    bz_domain_t* dom = task->domain;
    delete task;
    if (dom) domain_release(dom);  // per fix #632: release task's domain ref
    if (chainedSrc) bz_task_free(chainedSrc);
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
// (§1529): the terminal continuation scans the ORIGINAL input array in INPUT
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
static void bz_dispose_class_obj(void* obj) {
    if (!obj) return;
    void** vtbl = nullptr;
    memcpy(&vtbl, obj, sizeof(void*));
    if (vtbl && vtbl[2]) {
        typedef void (*DispFn)(void*);
        ((DispFn)vtbl[2])(obj);
    }
    free(obj);
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
        bz_task_free(tp);
    } else {
        // Slow path: task still running; register a continuation so the worker
        // frees it when it eventually completes.
        bz_task_then(tp,
            [](void* completedTask, void* /*state*/) {
                bz_task_free(completedTask);
            },
            nullptr);
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
    bool     disposed;
    // per fix #634: refcount — Channel object holds +1, each sender holds +1;
    // last release (core_refs == 1 → 0) deletes.
    std::atomic<int> core_refs{1};
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
        if (!ch->pending.empty()) {
            recv = ch->pending.front();
            ch->pending.pop_front();
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
    {   std::lock_guard<std::mutex> lk(ch->mtx);
        ch->closed = true;
        pending.swap(ch->pending);
        senders.swap(ch->pending_senders);
    }
    for (auto& r : pending) {
        void* excObj = nullptr;
        if (cce_vtable) {
            excObj = calloc(1, (size_t)BZ_EXC_OBJ_SIZE);
            memcpy(excObj, &cce_vtable, sizeof(void*));
        }
        bz_task_complete_error_obj(r.task, excObj);
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
                // Dispose the original fault exception. Dynamic dispatch via
                // vtable slot 2 (Dispose), then free.
                void** fault_vtbl = nullptr;
                memcpy(&fault_vtbl, fault_exc, sizeof(void*));
                if (fault_vtbl && fault_vtbl[2]) {
                    typedef void (*DispFn)(void*);
                    ((DispFn)fault_vtbl[2])(fault_exc);
                }
                free(fault_exc);
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
        if (sc->state) bz_cancel_state_signal(sc->state);  // cancel children
        if (sc->pending == 0) act = bz_scope_settle_locked(sc);
    }
    bz_scope_perform(act);
    return task;
}

// Dispose (unwind edge / sync fallback): cancel + non-blocking zombie drain.
// Never raises; the scope struct is freed at the last child completion (or
// here, when no children remain).
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
        if (sc->state) bz_cancel_state_signal(sc->state);
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
void bz_task_take_result(void* tp) {
    if (!tp) return;
    auto* task = static_cast<bz_task_t*>(tp);
    void* buf = nullptr;
    {
        std::lock_guard<std::mutex> lk(task->mtx);
        buf = task->result;
        task->result = nullptr;
        task->result_size = 0;
    }
    free(buf);
}

// §30.5 / #1258: dispose an UNCONSUMED owned class result of an OwningTask<T:class>
// at the task's using-var scope-end disposal, BEFORE bz_task_fire_and_forget /
// bz_task_free frees the task struct. Mirrors bz_tcs_dispose_owned_result, but the
// per-type disposer is passed in by codegen (bz_task_t carries no disposer field).
// Guards:
//   • disposer == null   → not an OwningTask<T:class> (Task<T> / no-value task); skip.
//   • task->is_error     → the async body faulted; no result was produced; skip.
//   • task->result == null → the result was already consumed (bz_task_take_result
//                            zeroed the buffer at the await consume site); skip → no
//                            double-free of the now-caller-owned object.
// On dispose: extract the class pointer from the result buffer, free the buffer, null
// the slot (so the subsequent bz_task_free's free(task->result) is free(nullptr)), then
// run the disposer (the class destructor + object free).
void bz_task_dispose_owned_result(void* tp, void (*disposer)(void*)) {
    if (!tp || !disposer) return;
    auto* task = static_cast<bz_task_t*>(tp);
    void* result_to_dispose = nullptr;
    void* buf_to_free = nullptr;
    {
        std::lock_guard<std::mutex> lk(task->mtx);
        if (!task->is_error && task->result &&
            task->result_size >= static_cast<int64_t>(sizeof(void*))) {
            result_to_dispose = *static_cast<void**>(task->result);
            buf_to_free = task->result;
            task->result = nullptr;
            task->result_size = 0;
        }
    }
    free(buf_to_free);
    if (result_to_dispose) disposer(result_to_dispose);
}

} // extern "C" (bz_task_take_result)
