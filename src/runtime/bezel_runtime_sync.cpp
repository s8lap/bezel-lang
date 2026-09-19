// bezel_runtime_sync.cpp — Mutex/Lock, Semaphore, Atomic.
// Split from bezel_runtime.cpp per #1414 stage 2 (behavior-preserving motion).
#include "bezel_runtime_internal.hpp"

// ---------------------------------------------------------------------------
// Mutex and Lock runtime (stdlib §6.2, issue #647)
//
// bz_mutex_t — a POSIX mutex with a refcounted core.
//
// Lifetime via reference counting (mirrors fix #634 / bz_channel_t):
//
//   Every holder that can dereference a bz_mutex_t* acquires +1 on core_refs
//   and releases -1 when done; the last release (core_refs → 0) destroys the
//   pthread_mutex and deletes the struct.
//
//   Holders and their acquire / release sites:
//     - Mutex object: bz_mutex_new acquires +1 (starts at 1);
//       bz_mutex_dispose releases -1 after marking disposed.
//     - Lock handle: bz_mutex_acquire / bz_mutex_try_acquire acquires +1;
//       bz_lock_release releases -1 after releasing the pthread_mutex.
//
//   This prevents bz_mutex_dispose from destroying the core while a live
//   Lock handle still holds a bz_mutex_t* (UAF-safe: Lock outliving Mutex).
//
// Recursiveness (documented choice, not specified by §6.2):
//   NON-RECURSIVE (PTHREAD_MUTEX_ERRORCHECK). A second pthread_mutex_lock or
//   pthread_mutex_timedlock from the same thread returns EDEADLK / ETIMEDOUT.
//   TryAcquire-while-held returns null immediately (timeout path) rather than
//   deadlocking, enabling the held-mutex test without a second thread.
//
// bz_lock_t — the owned guard returned by Acquire / TryAcquire.
//   Holds a borrowed pointer to bz_mutex_t (core ref) and an idempotent
//   released flag (§28.2 exactly-once release).
// ---------------------------------------------------------------------------

struct bz_mutex_t {
    pthread_mutex_t     mtx;
    // per #647 (mirrors #634): refcount — Mutex object holds +1, each Lock holds +1.
    // Last release (core_refs → 0) calls pthread_mutex_destroy and deletes.
    std::atomic<int>    core_refs{1};
    // disposed flag: set atomically by bz_mutex_dispose so concurrent Dispose
    // calls from two domains are safe (§28.2 / fix #706 — TOCTOU double-free).
    // exchange(true) ensures exactly one caller proceeds to core_release.
    std::atomic<bool>   disposed{false};
};

struct bz_lock_t {
    bz_mutex_t* mutex;   // borrowed (+ core_refs held)
    bool        released; // idempotent flag (§28.2)
};

static void mutex_core_ref(bz_mutex_t* m) {
    m->core_refs.fetch_add(1, std::memory_order_relaxed);
}

static void mutex_core_release(bz_mutex_t* m) {
    if (m->core_refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        pthread_mutex_destroy(&m->mtx);
        delete m;
    }
}

extern "C" {

// §6.2: Mutex — allocate a new non-recursive POSIX mutex.  core_refs = 1
// (the Mutex object's reference).  Returns bz_mutex_t* (opaque to Bezel code).
// The non-recursive choice (PTHREAD_MUTEX_ERRORCHECK) is the documented
// stdlib §6.2 choice — see the file-header note.
void* bz_mutex_new(void) {
    auto* m = new bz_mutex_t();
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    // PTHREAD_MUTEX_ERRORCHECK: prevents deadlock on self-lock; EDEADLK returned
    // instead of blocking, which maps cleanly to TryAcquire → null.
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&m->mtx, &attr);
    pthread_mutexattr_destroy(&attr);
    // core_refs already 1 (Mutex object ref) via the in-struct initializer.
    return m;
}

// §6.2: Mutex.Acquire — block until the mutex is available; return an owned
// bz_lock_t*. The lock struct acquires a core ref (+1) so the mutex core
// stays alive even if the Mutex object is disposed before the Lock is released.
void* bz_mutex_acquire(void* mp) {
    auto* m = (bz_mutex_t*)mp;
    pthread_mutex_lock(&m->mtx);  // blocking; non-recursive → always enters if not held
    mutex_core_ref(m);             // Lock holds a ref on the core
    auto* lk = new bz_lock_t{m, false};
    return lk;
}

// §6.2: Mutex.TryAcquire(timeout_ms) — attempt to acquire with a millisecond
// timeout. Returns an owned bz_lock_t* on success, or null on timeout /
// already-held. Uses pthread_mutex_timedlock with CLOCK_REALTIME (per POSIX
// timedlock). An EDEADLK (same-thread re-lock on ERRORCHECK mutex) maps to
// the timeout null path — enabling the TryAcquire-while-held test shape
// from the file-header note.
void* bz_mutex_try_acquire(void* mp, int64_t timeout_ms) {
    auto* m = (bz_mutex_t*)mp;
    int rc;
    if (timeout_ms <= 0) {
        // Zero / negative timeout: try without waiting.
        rc = pthread_mutex_trylock(&m->mtx);
        // EDEADLK = same thread already holds it (ERRORCHECK); treat as EBUSY.
        if (rc == EDEADLK) rc = EBUSY;
    } else {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec  += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000LL;
        if (deadline.tv_nsec >= 1000000000LL) {
            deadline.tv_sec  += 1;
            deadline.tv_nsec -= 1000000000LL;
        }
        rc = pthread_mutex_timedlock(&m->mtx, &deadline);
        if (rc == EDEADLK) rc = ETIMEDOUT;
    }
    if (rc != 0) return nullptr;   // timeout or contention → null (owned Lock?)
    mutex_core_ref(m);
    auto* lk = new bz_lock_t{m, false};
    return lk;
}

// §6.2: Lock.Dispose — release the held lock.  Idempotent per §28.2: the
// first call releases the pthread_mutex and frees the bz_lock_t struct;
// subsequent calls are no-ops.
// Called by the Lock's disposer (scope exit / explicit Dispose()).
// fix #4354 (stdlib §6.2, v0.298): the prior version only guarded the
// pthread_mutex_unlock with `released`, but ran `delete lk` and
// `mutex_core_release(m)` on every call. A second `Dispose()` then
// dereferenced the freed `lk->released` (UAF) and double-freed both
// `bz_lock_t` and the underlying mutex core. Move the cleanup inside
// the guard so the second call is a true no-op.
void bz_lock_release(void* lkp) {
    if (!lkp) return;
    auto* lk = (bz_lock_t*)lkp;
    if (lk->released) return;          // idempotent (§28.2)
    lk->released = true;
    pthread_mutex_unlock(&lk->mutex->mtx);
    // Release the core ref the Lock holds; may destroy the mutex if the Mutex
    // object was already disposed (Lock-outlives-Mutex scenario).
    bz_mutex_t* m = lk->mutex;
    delete lk;          // free the bz_lock_t struct
    mutex_core_release(m);
}

// §6.2: Mutex.Dispose — dispose the Mutex object.  Marks disposed (for
// idempotency) and releases the Mutex object's core ref.  The pthread_mutex
// is destroyed only when the last Lock is also released (via
// mutex_core_release → core_refs → 0). Idempotent: calling twice is a no-op
// on the second call.
// fix #706: exchange(true) is atomic — concurrent Dispose calls from two
// domains cannot both proceed to core_release (only the first caller, which
// sees false, drops the core ref; the second sees true and returns
// immediately).
void bz_mutex_dispose(void* mp) {
    if (!mp) return;
    auto* m = (bz_mutex_t*)mp;
    if (m->disposed.exchange(true)) return;  // already disposed → no-op (fix #706)
    mutex_core_release(m);  // drop the Mutex object's own ref
}

// ---------------------------------------------------------------------------
// §6.2 async variants (issue #647, v0.139): AcquireAsync / TryAcquireAsync.
//
// Waiter/handoff design (recorded per CLAUDE.md §4 "decline-with-reasons"):
//
//   A true "waiter queue" on the mutex core with direct handoff on release
//   would require replacing the pthread_mutex_t with a hand-rolled spinlock/
//   condvar assembly and changing bz_lock_release to pop and complete the next
//   waiter's task atomically.  That is sound but large.
//
//   Alternative chosen (simpler, auditable):
//     Uncontended fast path — pthread_mutex_trylock succeeds → allocate the
//     lock, complete the task immediately (already-done fast path in await).
//     Contended path — submit a pool worker that calls pthread_mutex_lock (or
//     timedlock) in a blocking manner, allocates the lock on acquire, then
//     calls bz_task_complete.  bz_task_complete → task_notify → domain_enqueue
//     wakes the awaiting continuation in its own domain queue — identical to
//     the Task.Delay mechanism.  The "handoff" is thus: when the holder calls
//     bz_lock_release (pthread_mutex_unlock), the OS wakes the blocking pool
//     worker, which then completes the task and routes the continuation back to
//     the domain.  This is correct: no starvation (POSIX liveliness), no UAF
//     (core refs held across the blocking acquire and released after complete),
//     no double-release (idempotent bz_lock_t flag).
//
//   Cancellation:
//     Pre-checked before pool submission.  If already cancelled at call time →
//     fault the task immediately (already-done, so await's fault path fires
//     synchronously on the next domain step).  If cancellation arrives WHILE
//     the pool worker is blocking inside pthread_mutex_lock, the worker detects
//     it after lock acquisition: it releases the mutex immediately (unlock +
//     core deref) and faults the task with OCE.  This is safe because the mutex
//     was only briefly held by the pool worker and is released before the fault;
//     the caller never sees the Lock object.
//
//   TryAcquireAsync:
//     Same as AcquireAsync but uses pthread_mutex_timedlock / trylock on the
//     pool worker.  If the timeout expires → complete the task with a null ptr
//     (no lock) rather than a bz_lock_t.  Cancel + timeout: if already
//     cancelled pre-submit → fault immediately (takes priority over timeout).
//
// ---------------------------------------------------------------------------

// Internal helper: allocate an already-done task carrying a void*-result.
// result_ptr is stored as sizeof(void*) bytes so the await machinery
// (which reads task->result as a void*) retrieves the lock pointer.
static bz_task_t* mutex_make_done_task(void* result_ptr) {
    bz_main_domain_get();  // per fix #632: lazy-init with permanent ref
    auto* task = new bz_task_t();
    // fix #713: route to the caller's domain.
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      task->domain = d; domain_ref(d); }
    // Store the pointer as the task's result (sizeof(void*) bytes).
    task->result = malloc(sizeof(void*));
    memcpy(task->result, &result_ptr, sizeof(void*));
    task->result_size = (int64_t)sizeof(void*);
    task->done = true;
    return task;
}

// Internal helper: allocate an already-faulted task with an OCE object.
static bz_task_t* mutex_make_faulted_task(void* oce_vtable) {
    bz_main_domain_get();
    auto* task = new bz_task_t();
    // fix #713: route to the caller's domain.
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      task->domain = d; domain_ref(d); }
    task->error_obj = bz_task_make_oce(oce_vtable);
    task->is_error = true;
    task->done = true;
    return task;
}

// bz_mutex_acquire_async — per §6.2 / #647.
// Returns a bz_task_t* (OwningTask<Lock>).
// Fast path (trylock succeeds): already-done task with the lock pointer.
// Slow path (contested): pool worker blocks, then completes the task.
void* bz_mutex_acquire_async(void* mp, void* token, void* oce_vtable) {
    auto* m = (bz_mutex_t*)mp;
    auto* s = (bz_cancel_state_t*)token;

    // Pre-cancel check.
    // #716: acquire load — synchronizes-with release-store in bz_cts_cancel/dispose.
    if (s && s->cancelled.load(std::memory_order_acquire)) {
        return mutex_make_faulted_task(oce_vtable);
    }

    // Uncontended fast path: try without blocking.
    int rc = pthread_mutex_trylock(&m->mtx);
    if (rc == 0) {
        // Acquired immediately.
        mutex_core_ref(m);
        auto* lk = new bz_lock_t{m, false};
        return mutex_make_done_task(lk);
    }

    // Contended path: submit a pool worker.
    if (!g_scheduler) bz_scheduler_init(4);
    bz_main_domain_get();
    auto* task = new bz_task_t();
    // fix #713: route to the caller's domain.
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      task->domain = d; domain_ref(d); }

    // The worker holds a core ref so the mutex core cannot be destroyed while
    // the pool thread is blocking inside pthread_mutex_lock.
    mutex_core_ref(m);

    // The worker captures the token pointer; retain it so the bz_cancel_state_t
    // stays alive even if the caller's CTS is disposed before the pool thread
    // finishes. The worker releases its reference when done (#647 / §6.1).
    bz_cancel_state_retain(s);

    g_scheduler->submit([task, m, s, oce_vtable]() {
        pthread_mutex_lock(&m->mtx);
        // Check cancel AFTER acquiring (the brief hold is released if cancelled).
        // #716: acquire load — synchronizes-with release-store in bz_cts_cancel.
        if (s && s->cancelled.load(std::memory_order_acquire)) {
            pthread_mutex_unlock(&m->mtx);
            mutex_core_release(m);  // release the worker's extra core ref
            bz_cancel_state_release(s);  // release the worker's token ref
            void* excObj = bz_task_make_oce(oce_vtable);
            bz_task_complete_error_obj(task, excObj);
            return;
        }
        bz_cancel_state_release(s);  // release the worker's token ref
        // Acquire succeeded and not cancelled: build a lock and complete.
        // The core ref the worker held is transferred to the new bz_lock_t.
        auto* lk = new bz_lock_t{m, false};
        // (core ref already held — do NOT call mutex_core_ref again here)
        void* lkPtr = lk;
        bz_task_complete(task, &lkPtr, (int64_t)sizeof(void*));
    });
    return task;
}

// bz_mutex_try_acquire_async — per §6.2 / #647.
// Returns a bz_task_t* (OwningTask<Lock?>).
// Result is a bz_lock_t* on success, or a null void* on timeout.
void* bz_mutex_try_acquire_async(void* mp, int64_t timeout_ms,
                                  void* token, void* oce_vtable) {
    auto* m = (bz_mutex_t*)mp;
    auto* s = (bz_cancel_state_t*)token;

    // Pre-cancel check (takes priority over timeout).
    // #716: acquire load — synchronizes-with release-store in bz_cts_cancel/dispose.
    if (s && s->cancelled.load(std::memory_order_acquire)) {
        return mutex_make_faulted_task(oce_vtable);
    }

    // Uncontended fast path: trylock (works for zero timeout too).
    int rc;
    if (timeout_ms <= 0) {
        rc = pthread_mutex_trylock(&m->mtx);
        if (rc == EDEADLK) rc = EBUSY;
    } else {
        // Try without blocking first; if held we go to slow path.
        rc = pthread_mutex_trylock(&m->mtx);
        if (rc == EDEADLK) rc = EBUSY;
    }

    if (rc == 0) {
        // Acquired immediately → non-null result.
        mutex_core_ref(m);
        auto* lk = new bz_lock_t{m, false};
        return mutex_make_done_task(lk);
    }

    // If zero/negative timeout: mutex is held, return null immediately (no point
    // submitting a pool worker that would return instantly anyway).
    if (timeout_ms <= 0) {
        void* nullPtr = nullptr;
        bz_main_domain_get();
        auto* task = new bz_task_t();
        // fix #713: route to the caller's domain.
        { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
          task->domain = d; domain_ref(d); }
        task->result = malloc(sizeof(void*));
        memcpy(task->result, &nullPtr, sizeof(void*));
        task->result_size = (int64_t)sizeof(void*);
        task->done = true;
        return task;
    }

    // Positive timeout + mutex currently held: submit timed blocking on pool.
    if (!g_scheduler) bz_scheduler_init(4);
    bz_main_domain_get();
    auto* task = new bz_task_t();
    // fix #713: route to the caller's domain.
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      task->domain = d; domain_ref(d); }

    // Worker holds a core ref across the blocking timedlock.
    mutex_core_ref(m);
    int64_t tms = timeout_ms;

    // Retain the token for the pool worker so the bz_cancel_state_t stays alive
    // even if the CTS is disposed before the worker finishes (#647 / §6.1).
    bz_cancel_state_retain(s);

    g_scheduler->submit([task, m, tms, s, oce_vtable]() {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec  += tms / 1000;
        deadline.tv_nsec += (tms % 1000) * 1000000LL;
        if (deadline.tv_nsec >= 1000000000LL) {
            deadline.tv_sec  += 1;
            deadline.tv_nsec -= 1000000000LL;
        }
        int rc2 = pthread_mutex_timedlock(&m->mtx, &deadline);
        if (rc2 != 0) {
            // Timeout or same-thread deadlock detection → null result.
            mutex_core_release(m);  // release the worker's extra core ref
            bz_cancel_state_release(s);  // release the worker's token ref
            void* nullPtr = nullptr;
            bz_task_complete(task, &nullPtr, (int64_t)sizeof(void*));
            return;
        }
        // Acquired: check cancellation post-acquire.
        // #716: acquire load — synchronizes-with release-store in bz_cts_cancel.
        if (s && s->cancelled.load(std::memory_order_acquire)) {
            pthread_mutex_unlock(&m->mtx);
            mutex_core_release(m);
            bz_cancel_state_release(s);  // release the worker's token ref
            void* excObj = bz_task_make_oce(oce_vtable);
            bz_task_complete_error_obj(task, excObj);
            return;
        }
        bz_cancel_state_release(s);  // release the worker's token ref
        // Build the lock; core ref transferred to bz_lock_t.
        auto* lk = new bz_lock_t{m, false};
        void* lkPtr = lk;
        bz_task_complete(task, &lkPtr, (int64_t)sizeof(void*));
    });
    return task;
}

} // extern "C" (Mutex + Lock)

// ---------------------------------------------------------------------------
// Semaphore and SemaphoreToken runtime (stdlib §6.3, issue #647)
//
// bz_semaphore_t — a POSIX counting semaphore with a refcounted core.
//
// Lifetime via reference counting (mirrors bz_mutex_t / fix #634):
//
//   Every holder that can dereference a bz_semaphore_t* acquires +1 on
//   core_refs and releases -1 when done; the last release (core_refs → 0)
//   destroys the sem_t and deletes the struct.
//
//   Holders and their acquire / release sites:
//     - Semaphore object: bz_semaphore_new acquires +1 (starts at 1);
//       bz_semaphore_dispose releases -1 after marking disposed.
//     - SemaphoreToken handle: bz_semaphore_acquire / bz_semaphore_try_acquire
//       acquires +1; bz_semaphore_token_release releases -1 after posting.
//
//   This prevents bz_semaphore_dispose from destroying the core while live
//   SemaphoreToken handles still hold a bz_semaphore_t* (UAF-safe: Token
//   outliving Semaphore is safe).
//
// bz_semaphore_token_t — the owned guard returned by Acquire / TryAcquire.
//   Holds a borrowed pointer to bz_semaphore_t (core ref) and an idempotent
//   released flag (§28.2 exactly-once release).
//
// Async acquire design (mirrors bz_mutex_acquire_async exactly):
//   Uncontended fast path — sem_trywait succeeds → already-done task.
//   Contended / positive-timeout path — pool worker calls sem_timedwait;
//     when the semaphore is posted the worker allocates the token and
//     calls bz_task_complete.  Cancellation: pre-checked; if cancelled
//     while the pool worker is blocking, the worker detects it after
//     sem_timedwait returns (either the timed wait expired or the OS
//     woke it up on a sem_post from the holder's token release) and faults
//     the task with OCE.
// ---------------------------------------------------------------------------

struct bz_semaphore_t {
    sem_t                sem;
    std::atomic<int>    core_refs{1};
    // fix #706: atomic so concurrent Dispose from two domains cannot both
    // reach semaphore_core_release (exchange(true) ensures exactly one caller).
    std::atomic<bool>   disposed{false};
    // fix #4445 (stdlib §6.3, v0.302): maxCount is the permit ceiling
    // documented at construction. It is recorded for diagnostics and is NOT
    // enforced at release time, because no release path can exceed it: a
    // permit is returned ONLY by disposing a SemaphoreToken, and a token
    // exists only because an acquisition consumed a permit, so the count
    // can never rise above where it started. POSIX sem_post has no upper
    // bound of its own, but the ceiling is structurally unreachable. The
    // construction-time ArgumentException guard (codegen LlvmBackend_Exprs2.cpp
    // — max < 1 or initial > max) is the whole reachable effect of
    // maxCount. (Stored as int for diagnostics.)
    int                 maxCount;
};

struct bz_semaphore_token_t {
    bz_semaphore_t* sem;     // borrowed (+ core_refs held)
    bool            released; // idempotent flag (§28.2)
};

static void semaphore_core_ref(bz_semaphore_t* s) {
    s->core_refs.fetch_add(1, std::memory_order_relaxed);
}

static void semaphore_core_release(bz_semaphore_t* s) {
    if (s->core_refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        sem_destroy(&s->sem);
        delete s;
    }
}

// Internal helper: make an already-done task carrying a void* result.
// Mirrors mutex_make_done_task.
static bz_task_t* semaphore_make_done_task(void* result_ptr) {
    bz_main_domain_get();
    auto* task = new bz_task_t();
    // fix #713: route to the caller's domain.
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      task->domain = d; domain_ref(d); }
    task->result = malloc(sizeof(void*));
    memcpy(task->result, &result_ptr, sizeof(void*));
    task->result_size = (int64_t)sizeof(void*);
    task->done = true;
    return task;
}

// Internal helper: make an already-faulted task with an OCE object.
// Mirrors mutex_make_faulted_task.
static bz_task_t* semaphore_make_faulted_task(void* oce_vtable) {
    bz_main_domain_get();
    auto* task = new bz_task_t();
    // fix #713: route to the caller's domain.
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      task->domain = d; domain_ref(d); }
    task->error_obj = bz_task_make_oce(oce_vtable);
    task->is_error = true;
    task->done = true;
    return task;
}

extern "C" {

// §6.3: Semaphore — allocate a new counting semaphore.  core_refs = 1.
// initialCount: the initial permit count (must be >= 0).
// maxCount: the permit ceiling (diagnostics-only here; see bz_semaphore_t
// comment for why no release-time check is needed).
//
// fix #4445 (stdlib §6.3, v0.302): the ArgumentException contract on the
// two-arg form (`max < 1` or `initial > max`) is enforced at the codegen
// guard in LlvmBackend_Exprs2.cpp — bz_semaphore_new is only called with
// already-validated args. The clamps below are a defensive default for any
// direct FFI / misuse path; a value reaching here that would have tripped
// the codegen guard means the guard was bypassed (e.g. a future caller or
// a hand-written FFI binding).
void* bz_semaphore_new(int initial_count, int max_count) {
    if (initial_count < 0) initial_count = 0;
    if (max_count < initial_count) max_count = initial_count;
    auto* s = new bz_semaphore_t();
    s->maxCount = max_count;
    // core_refs = 1 via the in-struct initializer.
    sem_init(&s->sem, /*pshared=*/0, (unsigned int)initial_count);
    return s;
}

// bz_semaphore_acquire — block until a permit is available; return an owned
// bz_semaphore_token_t*.  The token acquires a core ref (+1) so the semaphore
// core stays alive even if the Semaphore object is disposed before the token
// is released.
void* bz_semaphore_acquire(void* sp) {
    auto* s = (bz_semaphore_t*)sp;
    // sem_wait restarts on EINTR.
    int rc;
    do { rc = sem_wait(&s->sem); } while (rc == -1 && errno == EINTR);
    semaphore_core_ref(s);
    auto* tok = new bz_semaphore_token_t{s, false};
    return tok;
}

// bz_semaphore_try_acquire — attempt to acquire with a millisecond timeout.
// Returns an owned bz_semaphore_token_t* on success, or null on timeout.
// Uses sem_timedwait with CLOCK_REALTIME (per POSIX).
void* bz_semaphore_try_acquire(void* sp, int64_t timeout_ms) {
    auto* s = (bz_semaphore_t*)sp;
    int rc;
    if (timeout_ms <= 0) {
        // Zero / negative timeout: try without waiting.
        rc = sem_trywait(&s->sem);
    } else {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec  += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000LL;
        if (deadline.tv_nsec >= 1000000000LL) {
            deadline.tv_sec  += 1;
            deadline.tv_nsec -= 1000000000LL;
        }
        do {
            rc = sem_timedwait(&s->sem, &deadline);
        } while (rc == -1 && errno == EINTR);
    }
    if (rc != 0) return nullptr;  // timeout or unavailable → null (owned SemaphoreToken?)
    semaphore_core_ref(s);
    auto* tok = new bz_semaphore_token_t{s, false};
    return tok;
}

// bz_semaphore_token_release — release the held permit.  Idempotent per §28.2:
// the first call posts the semaphore; subsequent calls are no-ops.
// Called by the SemaphoreToken's disposer (scope exit / explicit Dispose()).
void bz_semaphore_token_release(void* tokp) {
    if (!tokp) return;
    auto* tok = (bz_semaphore_token_t*)tokp;
    if (!tok->released) {
        tok->released = true;
        sem_post(&tok->sem->sem);
    }
    // Release the core ref the Token holds; may destroy the semaphore core if
    // the Semaphore object was already disposed (Token-outlives-Semaphore).
    bz_semaphore_t* s = tok->sem;
    delete tok;                 // free the bz_semaphore_token_t struct
    semaphore_core_release(s);
}

// §6.3: Semaphore.Dispose — dispose the Semaphore object.  Marks disposed
// (for idempotency) and releases the Semaphore object's core ref.  The sem_t
// is destroyed only when the last SemaphoreToken is also released (via
// semaphore_core_release → core_refs → 0).  Idempotent: second call is no-op.
// fix #706: exchange(true) is atomic — concurrent Dispose from two domains
// cannot both proceed to semaphore_core_release.
void bz_semaphore_dispose(void* sp) {
    if (!sp) return;
    auto* s = (bz_semaphore_t*)sp;
    if (s->disposed.exchange(true)) return;  // already disposed → no-op (fix #706)
    semaphore_core_release(s);  // drop the Semaphore object's own ref
}

// ---------------------------------------------------------------------------
// §6.3 async variants (issue #647, v0.140): AcquireAsync / TryAcquireAsync.
//
// Design: mirrors bz_mutex_acquire_async / bz_mutex_try_acquire_async exactly.
// Fast path: sem_trywait succeeds → already-done task (no pool worker).
// Slow/contended path: pool worker calls sem_wait / sem_timedwait blocking;
//   on success allocates a token and calls bz_task_complete.  Cancellation:
//   pre-checked before pool submission; if already cancelled → fault immediately.
//   Post-acquire cancel: worker checks the token post-sem_wait and posts back
//   before faulting (the semaphore was only briefly held by the pool worker).
// ---------------------------------------------------------------------------

// bz_semaphore_acquire_async — per §6.3 / #647.
// Returns a bz_task_t* (OwningTask<SemaphoreToken>).
void* bz_semaphore_acquire_async(void* sp, void* token, void* oce_vtable) {
    auto* s = (bz_semaphore_t*)sp;
    auto* cs = (bz_cancel_state_t*)token;

    // Pre-cancel check.
    // #716: acquire load — synchronizes-with release-store in bz_cts_cancel/dispose.
    if (cs && cs->cancelled.load(std::memory_order_acquire)) {
        return semaphore_make_faulted_task(oce_vtable);
    }

    // Uncontended fast path: try without blocking.
    int rc = sem_trywait(&s->sem);
    if (rc == 0) {
        semaphore_core_ref(s);
        auto* tok = new bz_semaphore_token_t{s, false};
        return semaphore_make_done_task(tok);
    }

    // Contended path: submit a pool worker.
    if (!g_scheduler) bz_scheduler_init(4);
    bz_main_domain_get();
    auto* task = new bz_task_t();
    // fix #713: route to the caller's domain.
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      task->domain = d; domain_ref(d); }

    // Worker holds a core ref so the semaphore core cannot be destroyed while
    // the pool thread is blocking inside sem_wait.
    semaphore_core_ref(s);

    // Retain the token pointer for the pool worker (mirrors bz_mutex_acquire_async).
    bz_cancel_state_retain(cs);

    g_scheduler->submit([task, s, cs, oce_vtable]() {
        int rc2;
        do { rc2 = sem_wait(&s->sem); } while (rc2 == -1 && errno == EINTR);
        // Check cancel AFTER acquiring (the brief hold is released if cancelled).
        // #716: acquire load — synchronizes-with release-store in bz_cts_cancel.
        if (cs && cs->cancelled.load(std::memory_order_acquire)) {
            sem_post(&s->sem);              // give the permit back
            semaphore_core_release(s);       // release the worker's extra core ref
            bz_cancel_state_release(cs);
            void* excObj = bz_task_make_oce(oce_vtable);
            bz_task_complete_error_obj(task, excObj);
            return;
        }
        bz_cancel_state_release(cs);
        // Acquire succeeded and not cancelled: build a token.
        // The core ref the worker held is transferred to the new bz_semaphore_token_t.
        auto* tok = new bz_semaphore_token_t{s, false};
        void* tokPtr = tok;
        bz_task_complete(task, &tokPtr, (int64_t)sizeof(void*));
    });
    return task;
}

// bz_semaphore_try_acquire_async — per §6.3 / #647.
// Returns a bz_task_t* (OwningTask<SemaphoreToken?>).
// Result is a bz_semaphore_token_t* on success, or null on timeout.
void* bz_semaphore_try_acquire_async(void* sp, int64_t timeout_ms,
                                      void* token, void* oce_vtable) {
    auto* s = (bz_semaphore_t*)sp;
    auto* cs = (bz_cancel_state_t*)token;

    // Pre-cancel check (takes priority over timeout).
    // #716: acquire load — synchronizes-with release-store in bz_cts_cancel/dispose.
    if (cs && cs->cancelled.load(std::memory_order_acquire)) {
        return semaphore_make_faulted_task(oce_vtable);
    }

    // Uncontended fast path (zero timeout also uses trywait).
    int rc = sem_trywait(&s->sem);
    if (rc == 0) {
        semaphore_core_ref(s);
        auto* tok = new bz_semaphore_token_t{s, false};
        return semaphore_make_done_task(tok);
    }

    // If zero/negative timeout: no permits available; return null immediately.
    if (timeout_ms <= 0) {
        void* nullPtr = nullptr;
        bz_main_domain_get();
        auto* task2 = new bz_task_t();
        // fix #713: route to the caller's domain.
        { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
          task2->domain = d; domain_ref(d); }
        task2->result = malloc(sizeof(void*));
        memcpy(task2->result, &nullPtr, sizeof(void*));
        task2->result_size = (int64_t)sizeof(void*);
        task2->done = true;
        return task2;
    }

    // Positive timeout + no permits currently available: submit timed blocking.
    if (!g_scheduler) bz_scheduler_init(4);
    bz_main_domain_get();
    auto* task = new bz_task_t();
    // fix #713: route to the caller's domain.
    { bz_domain_t* d = g_current_domain ? g_current_domain : g_main_domain;
      task->domain = d; domain_ref(d); }

    semaphore_core_ref(s);
    int64_t tms = timeout_ms;
    bz_cancel_state_retain(cs);

    g_scheduler->submit([task, s, tms, cs, oce_vtable]() {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec  += tms / 1000;
        deadline.tv_nsec += (tms % 1000) * 1000000LL;
        if (deadline.tv_nsec >= 1000000000LL) {
            deadline.tv_sec  += 1;
            deadline.tv_nsec -= 1000000000LL;
        }
        int rc2;
        do {
            rc2 = sem_timedwait(&s->sem, &deadline);
        } while (rc2 == -1 && errno == EINTR);
        if (rc2 != 0) {
            // Timeout → null result.
            semaphore_core_release(s);
            bz_cancel_state_release(cs);
            void* nullPtr = nullptr;
            bz_task_complete(task, &nullPtr, (int64_t)sizeof(void*));
            return;
        }
        // Acquired: check cancellation post-acquire.
        // #716: acquire load — synchronizes-with release-store in bz_cts_cancel.
        if (cs && cs->cancelled.load(std::memory_order_acquire)) {
            sem_post(&s->sem);
            semaphore_core_release(s);
            bz_cancel_state_release(cs);
            void* excObj = bz_task_make_oce(oce_vtable);
            bz_task_complete_error_obj(task, excObj);
            return;
        }
        bz_cancel_state_release(cs);
        auto* tok = new bz_semaphore_token_t{s, false};
        void* tokPtr = tok;
        bz_task_complete(task, &tokPtr, (int64_t)sizeof(void*));
    });
    return task;
}

} // extern "C" (Semaphore + SemaphoreToken)

// ---------------------------------------------------------------------------
// Atomic<T> and AtomicInteger<T> runtime (stdlib §6.4, issue #647)
//
// Layout: bz_atomic_t has a refcounted core (for send-safe Shared<Atomic<T>>),
// a disposed flag (§28.2 idempotent dispose), and an 8-byte payload slot that
// holds cell_size bytes aligned to cell_size.  All __atomic_* operations use
// __ATOMIC_SEQ_CST per §6.4 v0.140.
//
// cell_size ∈ {1, 2, 4, 8}: derived from the T type restriction at sema/codegen
// time and baked in at construction.
// ---------------------------------------------------------------------------

struct bz_atomic_t {
    std::atomic<int>  core_refs{1};
    // fix #706: atomic so concurrent Dispose from two domains cannot both
    // reach atomic_core_release (exchange(true) ensures exactly one caller).
    std::atomic<bool> disposed{false};
    int              cell_size;       // 1, 2, or 4 for short T; 8 for long T
    // Value payload: 8-byte aligned buffer; cell_size bytes are used.
    alignas(8) uint8_t cell[8];
};

static void atomic_core_release(bz_atomic_t* a) {
    if (a->core_refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete a;
}

extern "C" {

// §6.4: Atomic<T> / AtomicInteger<T> — allocate a new atomic cell;
// core_refs = 1.
void* bz_atomic_new(int cell_size, uint64_t initial) {
    if (cell_size <= 0 || cell_size > 8) cell_size = 4;  // defensive
    auto* a = new bz_atomic_t();
    a->cell_size = cell_size;
    memset(a->cell, 0, 8);
    // Store initial value (seq-cst) into the cell.
    switch (cell_size) {
        case 1: { uint8_t  v = (uint8_t)initial;
                  __atomic_store(&a->cell[0], &v, __ATOMIC_SEQ_CST); break; }
        case 2: { uint16_t v = (uint16_t)initial;
                  __atomic_store((uint16_t*)a->cell, &v, __ATOMIC_SEQ_CST); break; }
        case 4: { uint32_t v = (uint32_t)initial;
                  __atomic_store((uint32_t*)a->cell, &v, __ATOMIC_SEQ_CST); break; }
        case 8: { uint64_t v = initial;
                  __atomic_store((uint64_t*)a->cell, &v, __ATOMIC_SEQ_CST); break; }
        default: break;
    }
    return a;
}

// §6.4: Atomic.Dispose — idempotent dispose (§28.2); drops the core ref.
// fix #706: exchange(true) is atomic — concurrent Dispose from two domains
// cannot both proceed to atomic_core_release.
void bz_atomic_dispose(void* ptr) {
    if (!ptr) return;
    auto* a = static_cast<bz_atomic_t*>(ptr);
    if (a->disposed.exchange(true)) return;  // already disposed → no-op (fix #706)
    atomic_core_release(a);
}

// bz_atomic_load — seq-cst load; returns value zero-extended to uint64_t.
uint64_t bz_atomic_load(void* ptr, int cell_size) {
    if (!ptr) return 0;
    auto* a = static_cast<bz_atomic_t*>(ptr);
    switch (cell_size) {
        case 1: { uint8_t  v = 0;
                  __atomic_load(&a->cell[0], &v, __ATOMIC_SEQ_CST);
                  return (uint64_t)v; }
        case 2: { uint16_t v = 0;
                  __atomic_load((uint16_t*)a->cell, &v, __ATOMIC_SEQ_CST);
                  return (uint64_t)v; }
        case 4: { uint32_t v = 0;
                  __atomic_load((uint32_t*)a->cell, &v, __ATOMIC_SEQ_CST);
                  return (uint64_t)v; }
        case 8: { uint64_t v = 0;
                  __atomic_load((uint64_t*)a->cell, &v, __ATOMIC_SEQ_CST);
                  return v; }
        default: return 0;
    }
}

// bz_atomic_store — seq-cst store; val truncated to cell_size bytes.
void bz_atomic_store(void* ptr, uint64_t val, int cell_size) {
    if (!ptr) return;
    auto* a = static_cast<bz_atomic_t*>(ptr);
    switch (cell_size) {
        case 1: { uint8_t  v = (uint8_t)val;
                  __atomic_store(&a->cell[0], &v, __ATOMIC_SEQ_CST); break; }
        case 2: { uint16_t v = (uint16_t)val;
                  __atomic_store((uint16_t*)a->cell, &v, __ATOMIC_SEQ_CST); break; }
        case 4: { uint32_t v = (uint32_t)val;
                  __atomic_store((uint32_t*)a->cell, &v, __ATOMIC_SEQ_CST); break; }
        case 8: { uint64_t v = val;
                  __atomic_store((uint64_t*)a->cell, &v, __ATOMIC_SEQ_CST); break; }
        default: break;
    }
}

// bz_atomic_exchange — seq-cst exchange; returns old value as uint64_t.
uint64_t bz_atomic_exchange(void* ptr, uint64_t val, int cell_size) {
    if (!ptr) return 0;
    auto* a = static_cast<bz_atomic_t*>(ptr);
    switch (cell_size) {
        case 1: { uint8_t  nv = (uint8_t)val, ov = 0;
                  __atomic_exchange(&a->cell[0], &nv, &ov, __ATOMIC_SEQ_CST);
                  return (uint64_t)ov; }
        case 2: { uint16_t nv = (uint16_t)val, ov = 0;
                  __atomic_exchange((uint16_t*)a->cell, &nv, &ov, __ATOMIC_SEQ_CST);
                  return (uint64_t)ov; }
        case 4: { uint32_t nv = (uint32_t)val, ov = 0;
                  __atomic_exchange((uint32_t*)a->cell, &nv, &ov, __ATOMIC_SEQ_CST);
                  return (uint64_t)ov; }
        case 8: { uint64_t nv = val, ov = 0;
                  __atomic_exchange((uint64_t*)a->cell, &nv, &ov, __ATOMIC_SEQ_CST);
                  return ov; }
        default: return 0;
    }
}

// bz_atomic_compare_exchange — strong CAS; returns the observed value.
// Returns expected on success, the blocking value on failure.
uint64_t bz_atomic_compare_exchange(void* ptr, uint64_t expected,
                                    uint64_t replacement, int cell_size) {
    if (!ptr) return expected;
    auto* a = static_cast<bz_atomic_t*>(ptr);
    switch (cell_size) {
        case 1: {
            uint8_t exp = (uint8_t)expected, rep = (uint8_t)replacement;
            __atomic_compare_exchange(&a->cell[0], &exp, &rep,
                                      /*weak=*/0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            return (uint64_t)exp;   // exp updated to observed value by __atomic_compare_exchange
        }
        case 2: {
            uint16_t exp = (uint16_t)expected, rep = (uint16_t)replacement;
            __atomic_compare_exchange((uint16_t*)a->cell, &exp, &rep,
                                      0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            return (uint64_t)exp;
        }
        case 4: {
            uint32_t exp = (uint32_t)expected, rep = (uint32_t)replacement;
            __atomic_compare_exchange((uint32_t*)a->cell, &exp, &rep,
                                      0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            return (uint64_t)exp;
        }
        case 8: {
            uint64_t exp = expected, rep = replacement;
            __atomic_compare_exchange((uint64_t*)a->cell, &exp, &rep,
                                      0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            return exp;
        }
        default: return expected;
    }
}

// bz_atomic_add — seq-cst fetch-add with §6.1 overflow check.
// Returns the POST-add value (§6.4: "returns post-operation value").
// signed_: 1 if T is a signed type (sbyte/short/int/long); 0 if unsigned.
// ove_vtable: vtable* for OverflowException; passed through to bz_set_exc /
//   bz_exc mechanism (same pattern as the checked-arithmetic codegen path).
uint64_t bz_atomic_add(void* ptr, int64_t delta, int signed_,
                       int cell_size, void* ove_vtable) {
    if (!ptr) return 0;
    auto* a = static_cast<bz_atomic_t*>(ptr);
    // Perform a CAS loop: read current, check overflow, write new value.
    // A seq-cst fetch_add would suffice for the atomic part, but we need the
    // overflow check between read and write, so use a CAS loop per §6.4 note.
    switch (cell_size) {
        case 1: {
            uint8_t cur;
            __atomic_load(&a->cell[0], &cur, __ATOMIC_SEQ_CST);
            for (;;) {
                // Overflow check (§6.1).
                int64_t c = signed_ ? (int64_t)(int8_t)cur : (int64_t)cur;
                int64_t r = c + delta;
                bool ovf = signed_ ? (r < -128 || r > 127) : (r < 0 || r > 255);
                if (ovf && ove_vtable) { bz_set_exc(ove_vtable); return 0; }
                uint8_t nv = (uint8_t)(uint64_t)r;
                uint8_t exp = cur;
                if (__atomic_compare_exchange(&a->cell[0], &exp, &nv,
                                              0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                    return (uint64_t)nv;
                cur = exp;
            }
        }
        case 2: {
            uint16_t cur;
            __atomic_load((uint16_t*)a->cell, &cur, __ATOMIC_SEQ_CST);
            for (;;) {
                int64_t c = signed_ ? (int64_t)(int16_t)cur : (int64_t)cur;
                int64_t r = c + delta;
                bool ovf = signed_ ? (r < -32768 || r > 32767) : (r < 0 || r > 65535);
                if (ovf && ove_vtable) { bz_set_exc(ove_vtable); return 0; }
                uint16_t nv = (uint16_t)(uint64_t)r;
                uint16_t exp = cur;
                if (__atomic_compare_exchange((uint16_t*)a->cell, &exp, &nv,
                                              0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                    return (uint64_t)nv;
                cur = exp;
            }
        }
        case 4: {
            uint32_t cur;
            __atomic_load((uint32_t*)a->cell, &cur, __ATOMIC_SEQ_CST);
            for (;;) {
                int64_t c = signed_ ? (int64_t)(int32_t)cur : (int64_t)cur;
                int64_t r = c + delta;
                bool ovf = signed_ ? (r < -2147483648LL || r > 2147483647LL)
                                   : (r < 0 || r > 4294967295LL);
                if (ovf && ove_vtable) { bz_set_exc(ove_vtable); return 0; }
                uint32_t nv = (uint32_t)(uint64_t)r;
                uint32_t exp = cur;
                if (__atomic_compare_exchange((uint32_t*)a->cell, &exp, &nv,
                                              0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                    return (uint64_t)nv;
                cur = exp;
            }
        }
        case 8: {
            uint64_t cur;
            __atomic_load((uint64_t*)a->cell, &cur, __ATOMIC_SEQ_CST);
            for (;;) {
                // 64-bit overflow: use __int128 to detect cleanly.
                __int128 c = signed_ ? (__int128)(int64_t)cur : (__int128)cur;
                __int128 r = c + (__int128)delta;
                bool ovf;
                if (signed_)
                    ovf = (r < (__int128)INT64_MIN || r > (__int128)INT64_MAX);
                else
                    ovf = (r < 0 || r > (__int128)(uint64_t)UINT64_MAX);
                if (ovf && ove_vtable) { bz_set_exc(ove_vtable); return 0; }
                uint64_t nv = (uint64_t)r;
                uint64_t exp = cur;
                if (__atomic_compare_exchange((uint64_t*)a->cell, &exp, &nv,
                                              0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                    return nv;
                cur = exp;
            }
        }
        default: return 0;
    }
}

} // extern "C" (Atomic<T> / AtomicInteger<T>)
