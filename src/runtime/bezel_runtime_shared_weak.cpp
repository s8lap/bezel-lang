// bezel_runtime_shared_weak.cpp — Shared<T> / Weak<T> shared ownership.
// Split from bezel_runtime.cpp per #1414 stage 2 (behavior-preserving motion).
#include "bezel_runtime_internal.hpp"

// ---------------------------------------------------------------------------
// Shared<T> / Weak<T> — shared ownership (§28.13 / stdlib §2.16)
//
// Control block layout: { void* value; atomic<long> strong; atomic<long> weak;
//                          void (*dispose_value)(void*); bool disposed }
// Handles (strong and weak) are both pointers to this struct; the compiler
// passes them as `ptr` and calls the functions below.
//
// v0.140 / #689: strong and weak counts are now atomic so that Shared<T> for
// internally-synchronized T (Mutex, Atomic<T>, AtomicInteger<T>, Semaphore)
// can be cloned and sent across domains (§28.13 sendability rule).  The domain-
// local path pays no extra cost beyond the single-CPU-ordering that acq_rel
// already provides; the cross-domain path gets the required atomicity.
// ---------------------------------------------------------------------------

struct bz_shared_t {
    void*  value;
    std::atomic<long> strong;   // per #689 / §28.13 v0.140: was plain long
    std::atomic<long> weak;     // per #689 / §28.13 v0.140: was plain long
    void (*dispose_value)(void*);
    int    value_disposed;   // 1 after the value has been disposed (idempotent guard)
    // §28.2 v0.135 / issue #627: checked-disposal-mode quarantine flag.
    // Set by bz_shared_mark_disposed (called from checked-mode explicit Dispose()
    // instead of bz_shared_release). bz_shared_release becomes a no-op when this
    // is set so the control block remains alive (deliberate leak in checked
    // builds) for the disposed-flag guard in bz_shared_is_disposed.
    int8_t handle_disposed;
};

// Allocate a new control block.  strong=1, weak=0. per §28.13 constructor.
void* bz_shared_new(void* value, void (*dispose_value)(void*)) {
    bz_shared_t* cb = (bz_shared_t*)calloc(1, sizeof(bz_shared_t));
    // placement-new the atomics into the calloc'd memory (calloc zeroes first).
    new (&cb->strong) std::atomic<long>(1);
    new (&cb->weak)   std::atomic<long>(0);
    cb->value = value;
    cb->dispose_value = dispose_value;
    cb->value_disposed = 0;
    return (void*)cb;
}

// §9.8 v0.141 / #703: fill the value of a Shared control block created as a
// placeholder (bz_shared_new(nullptr, disposer)). Used by CYCLIC deserialization:
// the handle is registered under its identity tag BEFORE its object body exists,
// so a Weak back-reference (or Shared alias) encountered while constructing that
// body can resolve and bz_shared_downgrade / bz_shared_clone it. Once the body is
// constructed this sets the value. The caller guarantees the value is set before
// any release, so the disposer never runs on a null value.
extern "C" void bz_shared_set_value(void* handle, void* value) {
    bz_shared_t* cb = (bz_shared_t*)handle;
    if (!cb) return;
    cb->value = value;
}

// Increment the strong count and return the same handle (a new alias). §28.13 Clone.
// per #689 / §28.13 v0.140: atomic fetch_add with acq_rel so a cross-domain
// clone sees a consistent strong count.
void* bz_shared_clone(void* handle) {
    bz_shared_t* cb = (bz_shared_t*)handle;
    if (!cb) return nullptr;
    cb->strong.fetch_add(1, std::memory_order_acq_rel);
    return handle;
}

// Return the underlying value pointer (borrow). §28.13 Get.
void* bz_shared_get(void* handle) {
    bz_shared_t* cb = (bz_shared_t*)handle;
    if (!cb) return nullptr;
    return cb->value;
}

// §28.2 v0.135 / issue #627: checked-disposal-mode quarantine for Shared<T>.
// Called instead of bz_shared_release for the explicit handle.Dispose() in
// checked builds. Disposes the owned value and marks handle_disposed=1, but
// does NOT free the control block — keeping it alive (deliberate leak in
// checked/debug builds) so bz_shared_is_disposed can read the flag afterward.
// Idempotent (safe to call twice). The scope-end bz_shared_release becomes a
// no-op when handle_disposed is set (see bz_shared_release below).
void bz_shared_mark_disposed(void* handle) {
    if (!handle) return;
    bz_shared_t* cb = (bz_shared_t*)handle;
    if (cb->handle_disposed) return;  // idempotent
    cb->handle_disposed = 1;
    // per #689: use atomic fetch_sub; then check the new value.
    long prev = cb->strong.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1 && !cb->value_disposed) {
        cb->value_disposed = 1;
        if (cb->dispose_value && cb->value)
            cb->dispose_value(cb->value);
        cb->value = nullptr;
    }
    // Do NOT free(cb) — block stays alive for the disposed-flag guard.
}

// §28.2 v0.135 / issue #627: return non-zero if the Shared<T> handle has been
// quarantined via bz_shared_mark_disposed (checked-disposal builds only).
int8_t bz_shared_is_disposed(void* handle) {
    if (!handle) return 0;
    return ((bz_shared_t*)handle)->handle_disposed;
}

// Mint a Weak handle (same control block, weak count +1). §28.13 Downgrade.
// per #689: atomic fetch_add for cross-domain safety.
void* bz_shared_downgrade(void* handle) {
    bz_shared_t* cb = (bz_shared_t*)handle;
    if (!cb) return nullptr;
    cb->weak.fetch_add(1, std::memory_order_acq_rel);
    return handle;   // same block; bz_weak_t* aliases bz_shared_t*
}

// TryUpgrade: produce a new strong handle if the value is still alive. §28.13.
// Returns 1 on success (strong count was > 0), 0 if the value is gone.
//
// per #689 / §28.13 v0.140: CAS loop that atomically increments strong ONLY
// when its current value is > 0.  This closes the upgrade-vs-last-drop race:
// a concurrent bz_shared_release that brings strong to 0 is not beaten by a
// non-atomic read+increment pair (the classic Arc<T>::upgrade race).
//
// Loop:   read strong; if 0 → value gone, return 0.
//         CAS strong(expected, expected+1) with acq_rel.
//         On spurious failure retry (compare_exchange_weak).
int bz_weak_try_upgrade(void* weak_handle, void** out_handle) {
    bz_shared_t* cb = (bz_shared_t*)weak_handle;
    if (!cb) { if (out_handle) *out_handle = nullptr; return 0; }
    long expected = cb->strong.load(std::memory_order_acquire);
    while (expected > 0) {
        if (cb->strong.compare_exchange_weak(expected, expected + 1,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            if (out_handle) *out_handle = weak_handle;
            return 1;
        }
        // expected has been refreshed by compare_exchange_weak on failure; retry.
    }
    // strong == 0: value already gone.
    if (out_handle) *out_handle = nullptr;
    return 0;
}

// Release one strong reference.  When the count hits 0, dispose the value;
// when both counts hit 0 free the control block. §28.13 Dispose / Dispose().
// §28.2 v0.135 / issue #627: in checked-disposal builds, bz_shared_mark_disposed
// quarantines the handle (handle_disposed=1, block NOT freed). A subsequent
// scope-end bz_shared_release call must be a no-op — skip if already quarantined.
// per #689 / §28.13 v0.140: atomic fetch_sub; dispose/free only when the
// decremented-to value reaches 0 (unique "last releaser" wins the disposal).
void bz_shared_release(void* handle) {
    bz_shared_t* cb = (bz_shared_t*)handle;
    if (!cb) return;
    // Skip if already quarantined by bz_shared_mark_disposed (checked mode).
    if (cb->handle_disposed) return;
    long prev = cb->strong.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1 && !cb->value_disposed) {
        cb->value_disposed = 1;
        // §9.8 v0.141 / #703: the value's destructor may release a Weak<T>
        // back-edge that points at THIS control block (a Shared/Weak cycle
        // disposed via its sole strong owner — e.g. A owns Shared<B>, B holds
        // Weak<A>). That re-entrant bz_weak_release runs while we are still on
        // the stack here; it would see strong==0, bring weak to 0, and free(cb)
        // underneath us → use-after-free / double free. Hold a temporary weak
        // reference across disposal so the weak count cannot reach 0 during
        // dispose_value; drop it afterwards and free iff we are then the last
        // outstanding reference.
        cb->weak.fetch_add(1, std::memory_order_acq_rel);
        void* val = cb->value;
        cb->value = nullptr;
        if (cb->dispose_value && val)
            cb->dispose_value(val);
        long pw = cb->weak.fetch_sub(1, std::memory_order_acq_rel);
        // pw == 1 means our temporary ref was the last weak handle (no real
        // weak handle outstanding) → free now that strong is already 0.
        if (pw == 1 && !cb->handle_disposed)
            free(cb);
    }
}

// Release one weak reference.  Free the block when both counts are 0. §28.13.
// per #689 / §28.13 v0.140: atomic fetch_sub; free only when the unique last
// weak releaser brings weak to 0 AND strong is already 0.
void bz_weak_release(void* weak_handle) {
    bz_shared_t* cb = (bz_shared_t*)weak_handle;
    if (!cb) return;
    long prev = cb->weak.fetch_sub(1, std::memory_order_acq_rel);
    // The value was already disposed (strong == 0 got here first).
    // Free the block only when this was the last weak handle.
    // §28.2 v0.135 / issue #627: do NOT free if handle_disposed is set —
    // the block was quarantined by bz_shared_mark_disposed (checked builds)
    // and must remain alive for the scope-end bz_shared_release no-op check.
    if (prev == 1 &&
        cb->strong.load(std::memory_order_acquire) == 0 &&
        !cb->handle_disposed)
        free(cb);
}

