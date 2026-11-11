// bezel_runtime_shared_weak.cpp — Shared<T> / Weak<T> shared ownership.
// Split from bezel_runtime.cpp per #1414 stage 2 (behavior-preserving motion).
#include "bezel_runtime_internal.hpp"

// ---------------------------------------------------------------------------
// Shared<T> / Weak<T> — shared ownership (§28.13 / stdlib §2.16)
//
// TWO objects: the CONTROL BLOCK, shared by every handle of one value
//   { void* value; atomic<long> strong; atomic<long> weak;
//     void (*dispose_value)(void*); int value_disposed }
// and the HANDLE CELL, one per `Shared<T>` / `Weak<T>` handle
//   { void* vtable; bz_shared_t* cb; int8_t released; int8_t handle_disposed }
// The compiler passes a handle as a `ptr` to its CELL and calls the functions
// below; it never reads either struct.
//
// fix #8533 (§28.13 / §28.4 / §28.5 / stdlib §2.16, v0.357): until this fix
// there was no cell — a handle WAS the control block's address, so `Clone()`
// and `Downgrade()` returned the pointer they were given and every handle of
// one value was the same pointer. Stdlib §2.16 declares both types as
// `public class … : IDisposable` ("Handles are ordinary owned class
// instances"; `Clone(): owned Shared<T>; // count + 1; a new independent
// handle`), so a handle converts to `IDisposable`, and an `owned IDisposable`
// slot discharges its value by the CLASS protocol: the word-0 vtable's
// Dispose slot, then a reclaim that frees the block (§28.4 step 2, §28.5).
// Over the control block that read `value` as a vtable and then `free`d a
// block other handles still pointed at (MEASURED: heap-buffer-overflow at an
// `owned d: IDisposable` parameter, an `owned F: IDisposable?` field and an
// `OwningArray<IDisposable>` element).
//
// A handle therefore has to be what stdlib §2.16 says it is: its own uniquely
// owned block, opening with a vtable word (bz_shared_vtable / bz_weak_vtable,
// bezel_runtime_refcounting.cpp — the Mutex / Semaphore handle-plus-core
// shape). Slot 2 gives the handle's count back and leaves the cell for the
// caller's reclaim, exactly as every other class's Dispose does; the typed
// releases (bz_shared_release / bz_weak_release) give the count back AND free
// the cell, so the typed sites are unchanged. `released` makes the two routes
// idempotent against each other (a typed local disposed through an
// `IDisposable` borrow, then released at scope end).
//
// v0.140 / #689: strong and weak counts are atomic so that Shared<T> for
// internally-synchronized T (Mutex, Atomic<T>, AtomicInteger<T>, Semaphore)
// can be cloned and sent across domains (§28.13 sendability rule).  The domain-
// local path pays no extra cost beyond the single-CPU-ordering that acq_rel
// already provides; the cross-domain path gets the required atomicity. A CELL
// has exactly one owner (it is an owned class instance), so its two flags are
// plain bytes.
// ---------------------------------------------------------------------------

struct bz_shared_t {
    void*  value;
    std::atomic<long> strong;   // per #689 / §28.13 v0.140: was plain long
    std::atomic<long> weak;     // per #689 / §28.13 v0.140: was plain long
    void (*dispose_value)(void*);
    int    value_disposed;   // 1 after the value has been disposed (idempotent guard)
};

struct bz_shared_handle_t {
    void*        vtable;     // bz_shared_vtable (strong) / bz_weak_vtable (weak)
    bz_shared_t* cb;         // null once this handle's count has been given back
    int8_t       released;   // this handle's count has been given back
    // §28.2 v0.135 / issue #627: checked-disposal-mode quarantine flag.
    // Set by bz_shared_mark_disposed (called from checked-mode explicit Dispose()
    // instead of bz_shared_release). bz_shared_release becomes a no-op when this
    // is set so the CELL remains alive (deliberate leak in checked builds) for
    // the disposed-flag guard in bz_shared_is_disposed.
    int8_t       handle_disposed;
};

static bz_shared_handle_t* bz_shared_handle_alloc(void* vtable, bz_shared_t* cb) {
    bz_shared_handle_t* h =
        (bz_shared_handle_t*)calloc(1, sizeof(bz_shared_handle_t));
    h->vtable = vtable;
    h->cb = cb;
    return h;
}

// Give one STRONG count back. When the count hits 0, dispose the value; when
// both counts hit 0 free the control block. §28.13.
// per #689 / §28.13 v0.140: atomic fetch_sub; dispose/free only when the
// decremented-to value reaches 0 (unique "last releaser" wins the disposal).
static void bz_shared_cb_release_strong(bz_shared_t* cb) {
    if (!cb) return;
    long prev = cb->strong.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1 && !cb->value_disposed) {
        cb->value_disposed = 1;
        // §9.8 v0.141 / #703: the value's destructor may release a Weak<T>
        // back-edge that points at THIS control block (a Shared/Weak cycle
        // disposed via its sole strong owner — e.g. A owns Shared<B>, B holds
        // Weak<A>). That re-entrant weak release runs while we are still on
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
        if (pw == 1)
            free(cb);
    }
}

// Give one WEAK count back.  Free the block when both counts are 0. §28.13.
// per #689 / §28.13 v0.140: atomic fetch_sub; free only when the unique last
// weak releaser brings weak to 0 AND strong is already 0.
static void bz_shared_cb_release_weak(bz_shared_t* cb) {
    if (!cb) return;
    long prev = cb->weak.fetch_sub(1, std::memory_order_acq_rel);
    // The value was already disposed (strong == 0 got here first).
    // Free the block only when this was the last weak handle.
    if (prev == 1 && cb->strong.load(std::memory_order_acquire) == 0)
        free(cb);
}

// Allocate a new control block and its first strong handle.  strong=1, weak=0.
// per §28.13 constructor.
void* bz_shared_new(void* value, void (*dispose_value)(void*)) {
    bz_shared_t* cb = (bz_shared_t*)calloc(1, sizeof(bz_shared_t));
    // placement-new the atomics into the calloc'd memory (calloc zeroes first).
    new (&cb->strong) std::atomic<long>(1);
    new (&cb->weak)   std::atomic<long>(0);
    cb->value = value;
    cb->dispose_value = dispose_value;
    cb->value_disposed = 0;
    return (void*)bz_shared_handle_alloc((void*)bz_shared_vtable, cb);
}

// §9.8 v0.141 / #703: fill the value of a Shared control block created as a
// placeholder (bz_shared_new(nullptr, disposer)). Used by CYCLIC deserialization:
// the handle is registered under its identity tag BEFORE its object body exists,
// so a Weak back-reference (or Shared alias) encountered while constructing that
// body can resolve and bz_shared_downgrade / bz_shared_clone it. Once the body is
// constructed this sets the value. The caller guarantees the value is set before
// any release, so the disposer never runs on a null value.
extern "C" void bz_shared_set_value(void* handle, void* value) {
    bz_shared_handle_t* h = (bz_shared_handle_t*)handle;
    if (!h || !h->cb) return;
    h->cb->value = value;
}

// Increment the strong count and return a NEW handle on the same control
// block (stdlib §2.16: "count + 1; a new independent handle"). §28.13 Clone.
// per #689 / §28.13 v0.140: atomic fetch_add with acq_rel so a cross-domain
// clone sees a consistent strong count.
void* bz_shared_clone(void* handle) {
    bz_shared_handle_t* h = (bz_shared_handle_t*)handle;
    if (!h || !h->cb) return nullptr;
    h->cb->strong.fetch_add(1, std::memory_order_acq_rel);
    return (void*)bz_shared_handle_alloc((void*)bz_shared_vtable, h->cb);
}

// Return the underlying value pointer (borrow). §28.13 Get. Also asked of a
// WEAK handle by the cyclic serializer (the value's identity, null once the
// value is gone) — both cells open the same way.
void* bz_shared_get(void* handle) {
    bz_shared_handle_t* h = (bz_shared_handle_t*)handle;
    if (!h || !h->cb) return nullptr;
    return h->cb->value;
}

// §28.2 v0.135 / issue #627: checked-disposal-mode quarantine for Shared<T>.
// Called instead of bz_shared_release for the explicit handle.Dispose() in
// checked builds. Gives the handle's count back (disposing the value if it was
// the last) and marks handle_disposed=1, but does NOT free the cell — keeping
// it alive (deliberate leak in checked/debug builds) so bz_shared_is_disposed
// can read the flag afterward. Idempotent (safe to call twice). The scope-end
// bz_shared_release becomes a no-op when handle_disposed is set (see
// bz_shared_release below).
void bz_shared_mark_disposed(void* handle) {
    if (!handle) return;
    bz_shared_handle_t* h = (bz_shared_handle_t*)handle;
    if (h->handle_disposed) return;  // idempotent
    h->handle_disposed = 1;
    if (h->released) return;         // already given back through `IDisposable`
    h->released = 1;
    bz_shared_t* cb = h->cb;
    h->cb = nullptr;
    bz_shared_cb_release_strong(cb);
    // Do NOT free(h) — the cell stays alive for the disposed-flag guard.
}

// §28.2 v0.135 / issue #627: return non-zero if the Shared<T> handle has been
// quarantined via bz_shared_mark_disposed (checked-disposal builds only), or
// (#8533) disposed through an `IDisposable` view of it.
int8_t bz_shared_is_disposed(void* handle) {
    if (!handle) return 0;
    bz_shared_handle_t* h = (bz_shared_handle_t*)handle;
    return (h->handle_disposed || h->released) ? 1 : 0;
}

// Mint a Weak handle (a new cell on the same control block, weak count +1).
// §28.13 Downgrade. per #689: atomic fetch_add for cross-domain safety.
void* bz_shared_downgrade(void* handle) {
    bz_shared_handle_t* h = (bz_shared_handle_t*)handle;
    if (!h || !h->cb) return nullptr;
    h->cb->weak.fetch_add(1, std::memory_order_acq_rel);
    return (void*)bz_shared_handle_alloc((void*)bz_weak_vtable, h->cb);
}

// TryUpgrade: produce a new strong handle if the value is still alive. §28.13.
// Returns 1 on success (strong count was > 0), 0 if the value is gone.
//
// per #689 / §28.13 v0.140: CAS loop that atomically increments strong ONLY
// when its current value is > 0.  This closes the upgrade-vs-last-drop race:
// a concurrent strong release that brings strong to 0 is not beaten by a
// non-atomic read+increment pair (the classic Arc<T>::upgrade race).
//
// Loop:   read strong; if 0 → value gone, return 0.
//         CAS strong(expected, expected+1) with acq_rel.
//         On spurious failure retry (compare_exchange_weak).
int bz_weak_try_upgrade(void* weak_handle, void** out_handle) {
    bz_shared_handle_t* h = (bz_shared_handle_t*)weak_handle;
    bz_shared_t* cb = h ? h->cb : nullptr;
    if (!cb) { if (out_handle) *out_handle = nullptr; return 0; }
    long expected = cb->strong.load(std::memory_order_acquire);
    while (expected > 0) {
        if (cb->strong.compare_exchange_weak(expected, expected + 1,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            if (out_handle)
                *out_handle = (void*)bz_shared_handle_alloc(
                    (void*)bz_shared_vtable, cb);
            else
                bz_shared_cb_release_strong(cb);  // nobody takes the handle
            return 1;
        }
        // expected has been refreshed by compare_exchange_weak on failure; retry.
    }
    // strong == 0: value already gone.
    if (out_handle) *out_handle = nullptr;
    return 0;
}

// Release one strong handle: give its count back (see
// bz_shared_cb_release_strong) and free the cell. §28.13 Dispose / Dispose().
// §28.2 v0.135 / issue #627: in checked-disposal builds, bz_shared_mark_disposed
// quarantines the handle (handle_disposed=1, cell NOT freed). A subsequent
// scope-end bz_shared_release call must be a no-op — skip if already quarantined.
void bz_shared_release(void* handle) {
    bz_shared_handle_t* h = (bz_shared_handle_t*)handle;
    if (!h) return;
    // Skip if already quarantined by bz_shared_mark_disposed (checked mode).
    if (h->handle_disposed) return;
    bz_shared_t* cb = h->released ? nullptr : h->cb;
    free(h);
    bz_shared_cb_release_strong(cb);
}

// Release one weak handle: give its count back and free the cell. §28.13.
void bz_weak_release(void* weak_handle) {
    bz_shared_handle_t* h = (bz_shared_handle_t*)weak_handle;
    if (!h) return;
    bz_shared_t* cb = h->released ? nullptr : h->cb;
    free(h);
    bz_shared_cb_release_weak(cb);
}

// fix #8533 (§28.4 step 2 / §28.5 / stdlib §2.16, v0.357): the CLASS-protocol
// Dispose of the two handles — slot 2 of bz_shared_vtable / bz_weak_vtable
// (bezel_runtime_refcounting.cpp). It is what an `IDisposable` slot holding a
// handle dispatches to ("runtime dispatch resolves disposal to the underlying
// class's `Dispose()`", §28.5): the handle's count is given back — the value
// is disposed when the last strong handle goes, exactly as the typed
// `Dispose()` does — and the CELL is left for the caller's reclaim step.
// Idempotent, as every class Dispose is (§28.4).
extern "C" void bz_shared_obj_dispose(void* handle) {
    bz_shared_handle_t* h = (bz_shared_handle_t*)handle;
    if (!h || h->released) return;
    h->released = 1;
    bz_shared_t* cb = h->cb;
    h->cb = nullptr;
    bz_shared_cb_release_strong(cb);
}
extern "C" void bz_weak_obj_dispose(void* weak_handle) {
    bz_shared_handle_t* h = (bz_shared_handle_t*)weak_handle;
    if (!h || h->released) return;
    h->released = 1;
    bz_shared_t* cb = h->cb;
    h->cb = nullptr;
    bz_shared_cb_release_weak(cb);
}
