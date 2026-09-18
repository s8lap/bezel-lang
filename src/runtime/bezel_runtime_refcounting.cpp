// bezel_runtime_refcounting.cpp — string/closure/box refcounting, static-owned-field registry.
// Split from bezel_runtime.cpp per #1414 stage 2 (behavior-preserving motion).
#include "bezel_runtime_internal.hpp"

// ---------------------------------------------------------------------------
// §22.13 / #1212: static owned class/interface fields are disposed at program
// exit in REVERSE initialization order (last-init first-disposed).
//
// bz_register_static_owned(obj) is called at the point each static owned
// field is first written (inside the synthesized sfinit / user cctor body).
// An atexit handler installed on first registration iterates the list in
// reverse, dispatches each object's idempotent vtable Dispose (slot 2,
// CG35-itable, same slot as for `using var` / owned-field auto-dispose), then
// frees the block. The vtable Dispose is the SAME call path as normal owned-
// field disposal; calling free() afterward is the companion to emitDisposeAndReclaim.
//
// Double-free safety: a static owned field is the sole ownership root of its
// object — no other site auto-disposes it (it is not a local `using var`, not
// an instance field of some other owner). The vtable Dispose is idempotent
// (§28.6 run-once flag inside), so an explicit earlier `.Dispose()` call just
// marks it done — the atexit free still reclaims the block exactly once.
//
// Static owning-inline-array slots ([N owned]T fields): each slot that was
// written at static init is registered individually; they appear in the list
// in slot order and are disposed in reverse slot-registration order.
// ---------------------------------------------------------------------------

struct bz_static_owned_entry_t {
    void* obj;  // static owned class/interface instance pointer
};

static std::vector<bz_static_owned_entry_t>* g_static_owned = nullptr;

static void bz_dispose_static_owned(void) {
    if (!g_static_owned) return;
    // §22.13 (v0.196) / #1799: DRAIN the registry LIFO, do not iterate a
    // fixed index range captured once. Popping from the back disposes in
    // reverse-of-registration = reverse-of-initialization order (last-initialized
    // first-disposed), AND picks up any entry appended mid-pass: a destructor the
    // pass invokes may make its FIRST touch of an as-yet-uninitialized type, whose
    // lazy static initialization (permitted at exit, §22.12 rule 1) constructs a
    // static owned field and calls bz_register_static_owned → push_back onto THIS
    // same vector. The old `for i = size()-1 .. 0` bound was evaluated once at
    // entry, so such a late entry was left undisposed → leak. The drain takes it
    // next (LIFO = "reverse order of its own initialization", §22.13); strict
    // reverse order is guaranteed only among pre-exit initializations, exactly as
    // the spec states. The vector is deleted ONLY after the drain empties, so a
    // late push_back always lands in a live vector.
    while (!g_static_owned->empty()) {
        // Copy-then-pop: hold NO reference into the vector across the Dispose call
        // below — Dispose may trigger a bz_register_static_owned push_back that
        // reallocates the vector's buffer.
        bz_static_owned_entry_t entry = g_static_owned->back();
        g_static_owned->pop_back();
        void* obj = entry.obj;
        if (!obj) continue;
        // vtable layout (CG35-itable): [0]=td$, [1]=itmap$, [2]=Dispose.
        // Dispose is idempotent — a prior explicit .Dispose() is a no-op here.
        void** vtable = *(void***)obj;
        if (vtable && vtable[2]) {
            typedef void (*dispose_fn_t)(void*);
            ((dispose_fn_t)vtable[2])(obj);
        }
        free(obj);
    }
    delete g_static_owned;
    g_static_owned = nullptr;
}

extern "C" void bz_register_static_owned(void* obj) {
    if (!obj) return;
    if (!g_static_owned) {
        g_static_owned = new std::vector<bz_static_owned_entry_t>();
        atexit(bz_dispose_static_owned);
    }
    g_static_owned->push_back({obj});
}

// Called by the in-process JIT test harness immediately after the JIT
// main() returns (while the LLJIT is still alive and vtable pointers are
// valid).  Disposes everything registered in this JIT run and clears the
// list so the process-exit atexit handler is a no-op.  Without this,
// atexit runs after the LLJIT is destroyed and vtable[2] is a dangling
// pointer → SIGSEGV.
extern "C" void bz_flush_static_owned(void) {
    bz_dispose_static_owned();
}

// ---------------------------------------------------------------------------
// String reference counting (§4.6 / §28.9, issue #422 strings increment).
//
// `string` is runtime-managed: "its memory is reclaimed automatically" (§4.6).
// Strings are immutable, reference-shared LEAVES (§28.8) — a string can never
// hold an owning reference to another heap object — so acyclic reference
// counting is sound and sufficient: a value's data buffer and header are freed
// exactly when the last reference goes away, with no cycle to leak.
//
// §28.9 / #439: the count is ATOMIC now that async lands. Async continuations
// resume on any thread-pool thread (§30.13), and a runtime-managed string may be
// captured into an async frame and released on a different thread than the one
// that retained it, so retain/release must be a data-race-free atomic
// read-modify-write. The cheapest sound choice (correctness-first per §28.9) is
// a single __atomic_add_fetch / __atomic_sub_fetch with acq_rel ordering on the
// rc word: the increment publishes the new reference; the final decrement
// synchronizes-with all prior releases so the freeing thread observes every
// writer's stores before it frees. The rc field sits at an 8-byte-aligned offset
// in a malloc/calloc'd header (max-aligned base), so the reinterpreted int64* is
// suitably aligned for a lock-free atomic op. The SENTINEL check is a relaxed
// load (a literal's rc is a compile-time constant, never mutated concurrently).
//
// SENTINEL: literal/global strings (emitted as constant LLVM globals by
// codegen, never heap-allocated) carry rc == BZ_STRING_RC_SENTINEL; retain and
// release are no-ops on them. A literal must never be free()'d.
//
// null is a no-op for both (a nullable `string?` slot may hold null).

extern "C" void bz_string_retain(void* s) {
    if (!s) return;
    int64_t* rcp = (int64_t*)((uint8_t*)s + BZ_STRING_RC_OFFSET);
    if (__atomic_load_n(rcp, __ATOMIC_RELAXED) == BZ_STRING_RC_SENTINEL)
        return;                                // static/literal — never counted
    __atomic_add_fetch(rcp, 1, __ATOMIC_ACQ_REL);
}

extern "C" void bz_string_release(void* s) {
    if (!s) return;
    int64_t* rcp = (int64_t*)((uint8_t*)s + BZ_STRING_RC_OFFSET);
    if (__atomic_load_n(rcp, __ATOMIC_RELAXED) == BZ_STRING_RC_SENTINEL)
        return;                                // static/literal — never freed
    if (__atomic_sub_fetch(rcp, 1, __ATOMIC_ACQ_REL) > 0)
        return;
    // Last reference gone: free the data buffer, then the header. Read the data
    // pointer at offset 0 (the layout's first field). free(NULL) is safe (the
    // empty-string case may carry a null data pointer).
    void* data;
    memcpy(&data, (uint8_t*)s, sizeof(void*));
    free(data);
    free(s);
}

// §8.6 / §12.7 (#417): is the Object-typed value `obj` a string? A string in an
// Object slot is a bare bz_string* (word 0 = char* data); a class / boxed value
// / built-in exception has a vtable pointer at word 0. `vtable_table` is the
// null-terminated array codegen emits listing every vtable/box-vtable global.
// `obj` is a string iff it is non-null and its first word is NOT one of those
// vtables. (Runtime-internal vtables — IEnumerator state machines — are never
// stored in a plain Object slot, so omitting them from the table is sound.)
extern "C" int32_t bz_obj_is_string(void* obj, void** vtable_table) {
    if (!obj) return 0;
    void* word0;
    memcpy(&word0, obj, sizeof(void*));  // obj's first word (vtable or data ptr)
    if (vtable_table) {
        for (void** p = vtable_table; *p != nullptr; ++p)
            if (*p == word0) return 0;   // a known vtable → not a string
    }
    return 1;                            // not a vtable → a string
}

// ---------------------------------------------------------------------------
// §8.3 (v0.204, #2055): assigned identity hashes.
//
// The class-default `GetHashCode` and the box-default identity hash draw their
// value from a per-domain deterministic counter (assigned on first request,
// stored with the object thereafter), NOT from the object's storage address.
// The two invariants this upholds vs the prior `trunc(ptrToInt(obj))` lowering:
//   • Cross-run determinism (§30.13): given a fixed program in a single
//     domain with no world reads, the order in which objects' hashes are
//     first requested is identical across runs, so the assigned values are
//     identical — the runtime is no longer surfacing ASLR / heap-layout
//     entropy through GetHashCode.
//   • No-address invariant (§32.12): identity hash codes never expose
//     addresses, extending the §32.12 no-object-address invariant through
//     the hash surface.
//
// STORAGE: a process-wide side-table keyed by object pointer. This is the
// syncblock-style option the spec names ("per-object metadata word or side
// table"); it satisfies "objects never hashed pay nothing" — no per-object
// metadata word is added to the layout. Within a single run the value stored
// for a given pointer is stable (looked up, not recomputed); across the run
// boundary the table is empty (zero-initialized static storage), so cross-run
// determinism holds. Pointer-reuse after free is not a concern for the spec
// contract because the freed object is unreachable by the time its address is
// reused, so no live pair of objects ever shares a stored hash.
//
// REAP ON FREE (#2058, memory-efficiency hardening of #2055): the side-table
// maps object pointer → assigned hash and would otherwise grow without bound
// for a long-running service that hashes many transient objects (every hashed
// object's entry persists after the object is freed). The fix reaps an entry
// at the object's free site: `bz_obj_identity_hash_forget(obj)` is emitted by
// codegen at every owned-class reclaim (emitDisposeAndReclaim, immediately
// before the heap free in both release and §28.2 checked-disposal modes) and
// is called directly by `bz_box_release` at the last drop of a box wrapper —
// the two object kinds whose default `GetHashCode` reaches this helper. The
// per-domain counter is NOT touched by the reap, so the deterministic counter
// sequence is unchanged: only the freed object's side-table entry goes away,
// and with it the only thing the freed object could still answer about itself.
// Reaping is a pure memory-efficiency optimization — semantically the freed
// object is unreachable, so no live program behavior depends on its entry.
//
// Soundness: the reap runs WHILE THE OBJECT IS STILL LIVE (immediately before
// the heap free), so erasing its side-table entry cannot read freed memory.
// `std::unordered_map::erase` is a no-op on a missing key, so calling forget
// on an object that was never hashed (the common case — most freed objects are
// never hashed) costs only the mutex lock + a failed find. The erase is under
// the SAME `g_identity_hashes_mtx` that guards lookup and assignment, so the
// reap is atomic w.r.t. concurrent hashing on another thread.
//
// COUNTERS: per-domain (bz_domain_t::identity_hash_seq) when a domain is
// active on the calling thread, falling back to g_startup_thread_hash_seq
// (the startup/shutdown thread's own sequence, §30.13) when g_current_domain
// is null. fetch_add under the side-table lock makes assignment atomic and
// preserves the first-request order even under concurrent hashing within a
// domain (domain run-to-completion serializes continuations anyway, §24.6).
// ---------------------------------------------------------------------------
static std::unordered_map<void*, int32_t> g_identity_hashes;
static std::mutex                         g_identity_hashes_mtx;
static std::atomic<uint64_t>              g_startup_thread_hash_seq{0};

extern "C" int32_t bz_obj_identity_hash(void* obj) {
    if (!obj) return 0;
    std::lock_guard<std::mutex> lk(g_identity_hashes_mtx);
    auto it = g_identity_hashes.find(obj);
    if (it != g_identity_hashes.end()) return it->second;
    // First request for this object: draw the next value from the active
    // domain's sequence, or the startup/shutdown-thread sequence when no
    // domain is active. Counters are 1-based (0 is reserved for the null
    // object's hash above).
    uint64_t next;
    if (g_current_domain) {
        next = g_current_domain->identity_hash_seq.fetch_add(
                    1, std::memory_order_relaxed) + 1;
    } else {
        next = g_startup_thread_hash_seq.fetch_add(
                    1, std::memory_order_relaxed) + 1;
    }
    // int32 truncation: a 32-bit hash matches the §8.3 signature
    // `GetHashCode(): int` and the i32 storage used by hash-based collections.
    // Collisions across 2^31 distinct hashed objects in one run are acceptable
    // per the hash invariant (a.Equals(b) ⇒ same hash; converse not required).
    int32_t h = static_cast<int32_t>(next & 0x7fffffffu);
    g_identity_hashes[obj] = h;
    return h;
}

// §8.3 / #2058: reap the side-table entry for `obj` at its free site. Called
// by codegen at every owned-class reclaim (emitDisposeAndReclaim, immediately
// before the heap free) and by `bz_box_release` at the last drop of a box —
// the two object kinds whose default `GetHashCode` reaches
// `bz_obj_identity_hash`. Bounds side-table growth for long-running services
// that hash many transient objects. Determinism is preserved: the per-domain
// counter is untouched, only the freed object's entry is removed (and the
// freed object is unreachable, so no live behavior depends on it). A no-op on
// null and on objects that were never hashed (the common case).
extern "C" void bz_obj_identity_hash_forget(void* obj) {
    if (!obj) return;
    std::lock_guard<std::mutex> lk(g_identity_hashes_mtx);
    g_identity_hashes.erase(obj);
}

// §8.3 / #2058: test/debug accessor — the number of live entries currently in
// the identity-hash side-table. Used by the regression test for #2058 to
// assert the table is BOUNDED after many alloc+hash+free cycles (it stays
// small — the live-object count — rather than growing to the cycle count).
// Harmless in production (reads size() under the lock); never called by
// generated code.
extern "C" int32_t bz_identity_hash_table_size(void) {
    std::lock_guard<std::mutex> lk(g_identity_hashes_mtx);
    return static_cast<int32_t>(g_identity_hashes.size());
}

// ---------------------------------------------------------------------------
// Closure-cell reference counting (§27.5 / §28.9, issue #422 closures
// increment; issue #672 typed env-disposer foundation).
//
// Function-typed values (lambda closures — capturing or not — and method
// references) are RUNTIME-MANAGED (§28.9): "Runtime management governs the
// function value itself — the heap-allocated closure object that backs the
// function value." Codegen lowers each such value to a heap cell laid out as
//   { void* fn      (off  0),
//     void* env     (off  8),
//     int64_t rc    (off 16),
//     void* envdtor (off 24),
//     int64_t kind  (off 32) }  = 40 bytes.
// `fn` is the function/thunk pointer; `env` is null (non-capturing / static
// method reference) or points at a separately-allocated env block holding the
// capture pointers. The rc was appended AFTER env so fn (off 0) and env (off 8)
// keep their offsets: every codegen call site loads fn at offset 0 and env at
// GEP index 1, both unmoved. `kind` (off 32, §27.7.1 / #1544) tags the
// function-value equality rule (see bezel_runtime_internal.hpp's BZ_FVK_*
// constants); retain/release never read it.
//
// #672 TYPED ENV-DISPOSER (foundation). `envdtor` (off 24) is a per-lambda
// typed disposal function emitted by codegen beside the trampoline. When
// non-null, bz_closure_release calls it as `envdtor(env)` at last-drop BEFORE
// freeing the env block. This lets the disposer release refcounted captures
// (strings, class references) that the lambda holds via a +1 retain (the
// follow-up activation lane). Foundation: all producers store envdtor = null
// (calloc zero-fill + explicit store); at null the behaviour is identical to
// the old 24-byte layout (back-compat for all existing closures).
//
// LEAF with respect to ownership (foundation state). Per §28.9, a closure does
// NOT own the values it captures: "Capturing an owned class-typed local does
// NOT transfer ownership into the closure ... The original `using var` (or
// other owned location) retains the disposal obligation." The §28.3 capture
// table likewise classifies every closure capture as a BORROW. So in the
// CURRENT state (envdtor = null) a closure cell holds only borrows; freeing it
// frees ONLY the cell and its env block — never a captured value's storage.
// When the activation lane lands (populating envdtor for retaining captures),
// the envdtor will call the appropriate release helpers for the +1-owned slots,
// and the over-release rule will be enforced: envdtor only releases what the
// retain at capture-site added, no more.
// Acyclic reference counting is sound in both states (null and populated).
//
// §28.9 / #439: the count is ATOMIC — a closure may be captured into an async
// frame (or passed to a worker that runs it) and released on a different thread
// (§30.13). A single __atomic_add_fetch / __atomic_sub_fetch (acq_rel) on the
// 8-byte-aligned rc word is the cheapest sound read-modify-write; the final
// decrement synchronizes-with prior releases before the env block + cell are
// freed.
//
// null is a no-op for both retain and release (a nullable `((...) -> ...)?`
// slot may hold null).

// Byte offset of the rc field inside the closure cell (after fn + env).
static const int64_t BZ_CLOSURE_RC_OFFSET = 16;
// Byte offset of the env pointer inside the closure cell (after fn).
// #672: byte offset of the envdtor pointer inside the closure cell (after rc).
static const int64_t BZ_CLOSURE_ENVDTOR_OFFSET = 24;

extern "C" void bz_closure_retain(void* c) {
    if (!c) return;
    int64_t* rcp = (int64_t*)((uint8_t*)c + BZ_CLOSURE_RC_OFFSET);
    __atomic_add_fetch(rcp, 1, __ATOMIC_ACQ_REL);
}

extern "C" void bz_closure_release(void* c) {
    if (!c) return;
    int64_t* rcp = (int64_t*)((uint8_t*)c + BZ_CLOSURE_RC_OFFSET);
    if (__atomic_sub_fetch(rcp, 1, __ATOMIC_ACQ_REL) > 0)
        return;
    // Last reference gone.
    void* env;
    memcpy(&env, (uint8_t*)c + BZ_CLOSURE_ENV_OFFSET, sizeof(void*));
    // #672: invoke the typed env-disposer (if any) BEFORE freeing the env block.
    // Foundation: envdtor is always null today (calloc zero-fill; initClosureEnvdtor
    // stores null explicitly). When the capture-retain activation lane lands,
    // envdtor will be a per-lambda bz$lam$N$envdtor(ptr env) that releases the
    // +1-retained slots.  null check makes old and new paths identical when there
    // is nothing to dispose.  The envdtor is idempotent-safe: it runs exactly once
    // here, at the LAST release (rc == 0), so it can never double-release.
    void* envdtor_fn;
    memcpy(&envdtor_fn, (uint8_t*)c + BZ_CLOSURE_ENVDTOR_OFFSET, sizeof(void*));
    if (envdtor_fn && env) {
        // Cast to void(*)(void*) — the envdtor ABI: takes the env pointer.
        typedef void (*envdtor_t)(void*);
        ((envdtor_t)envdtor_fn)(env);
    }
    // Free the env block (uniquely owned by this cell; allocations in
    // lowerLambda/makeMethodThunk each produce one fresh env per cell).
    // free(NULL) is safe (a non-capturing closure carries a null env).
    free(env);
    free(c);
}

// ---------------------------------------------------------------------------
// Boxed value-type reference counting (§8.6 / §28.9, issue #422 boxes
// increment).
//
// When a VALUE type (record/enum/primitive/nullable value) is used where
// `Object` (or an interface it implements) is expected, codegen BOXES it
// (§8.6): a heap wrapper holds the value. Per §28.9 boxed value types are
// RUNTIME-MANAGED — "exempt from manual disposal," their memory reclaimed
// automatically. Codegen lowers each box to a cell laid out as
//   { void* vtable (off 0), int64_t rc (off 8), value-bytes (off 16) }.
// The vtable word at offset 0 is the boxable type's box vtable (its first slot
// is the type descriptor, so `is`/`as`/interface dispatch recognize the box
// uniformly with a class instance). The rc sits at the FIXED offset 8 — between
// the vtable word and the payload — so ONE runtime offset serves EVERY box no
// matter the boxed value's size; codegen's payload accessors advance past TWO
// words (offset 16). boxValue initializes rc to 1 (the producer's reference).
//
// LEAF with respect to ownership. The boxed value is a VALUE type
// (record/enum/primitive/nullable value): it owns no heap sub-objects that need
// disposing (records/enums/primitives are leaves; a record's only reference
// fields are runtime-managed, §9.7). So release frees ONLY the box wrapper —
// like a string. Acyclic reference counting is therefore sound.
//
// SAFETY — never handed a class instance. A class instance coerced to `Object`
// (the #399 reference path) keeps its bare pointer: boxValue is NOT invoked, no
// box wrapper exists, and codegen's placement discriminator records the slot as
// non-box. For an initializer-only local that discriminator is the initializer's
// static producer kind; for a reassignable local (#2960) it is an i1 state bit
// updated from every statically-classifiable store. So bz_box_release is only
// ever called on a genuine box cell. It does NOT inspect the vtable or attempt to
// tell a box from a class instance at runtime — the box-vs-class distinction is
// resolved at the codegen producer/store site. A class instance's bytes
// ({ vtable, i8 disposed-flag@1, fields }) are therefore never read as an rc and
// never freed here.
//
// §28.9 / #439: the count is ATOMIC now that async lands — a boxed value may be
// captured into an async frame and released on a different thread than the one
// that retained it (§30.13). A single __atomic_add_fetch / __atomic_sub_fetch
// (acq_rel) on the 8-byte-aligned rc word is the cheapest sound read-modify-
// write; the final decrement synchronizes-with prior releases before the wrapper
// is freed.
//
// null is a no-op for both (a null T? boxes to a null Object reference, §8.6).

// Byte offset of the rc field inside the box cell (after the vtable word).
static const int64_t BZ_BOX_RC_OFFSET = 8;

extern "C" void bz_box_retain(void* b) {
    if (!b) return;
    int64_t* rcp = (int64_t*)((uint8_t*)b + BZ_BOX_RC_OFFSET);
    __atomic_add_fetch(rcp, 1, __ATOMIC_ACQ_REL);
}

extern "C" void bz_box_release(void* b) {
    if (!b) return;
    int64_t* rcp = (int64_t*)((uint8_t*)b + BZ_BOX_RC_OFFSET);
    if (__atomic_sub_fetch(rcp, 1, __ATOMIC_ACQ_REL) > 0)
        return;
    // Last reference gone: free the box wrapper. The boxed value is a value type
    // (a leaf — no owned sub-objects), so nothing else is freed; the single
    // calloc'd cell is released exactly once. The box was proven non-escaping by
    // codegen, so no other reference exists — no UAF, no double-free.
    //
    // §8.3 / #2058: a box whose default GetHashCode was called has an entry in
    // the identity-hash side-table; reap it BEFORE the free so the table tracks
    // live objects, not all-time allocations. The box pointer is still valid
    // here (free is the next call), so the erase cannot read freed memory; a
    // never-hashed box is a cheap mutex lock + failed find.
    bz_obj_identity_hash_forget(b);
    free(b);
}

// ---------------------------------------------------------------------------
// fix #3460 (§28.3 / §30.1): cross-task borrow tracker.
//
// Classes in Bezel are single-owner (no GC, no per-class refcount), so a
// borrowed reference to a class instance cannot extend its lifetime through
// refcounting alone. When an async method captures `this = c` into its heap
// frame, the frame must hold a *borrow* on c that outlives every pending
// resumption — the §28.3 claim that "the borrow of the captured state must
// outlive any pending resumption". The borrow tracker is a process-global
// side table indexed by the borrowed object's pointer. Each entry holds an
// atomic borrow count and a condition variable for waiters. Codegen emits
// bz_register_borrow / bz_release_borrow at the frame's lifetime boundary,
// and emits bz_wait_for_borrows at the OWNER's dispose site (e.g. `using var
// c := new C()` scope-end) BEFORE the actual `free(c)`. With both halves in
// place, the race of #3460 closes: the owner's dispose site blocks until the
// worker's resume continuation has run emitAsyncComplete and released its
// borrow.
//
// Side table is a single global unordered_map guarded by g_borrow_mu. The
// entry is RETAINED when the count returns to zero (a future register on
// the same pointer must find it, and the BzBorrowEntry's cv must remain
// valid across the wait/notify pair). The condition variable is per-entry
// (no false wakes across objects). All three entry points are null-safe.
//
// Ordering: the register/release pair is acq_rel so a release that drops
// the count to zero synchronizes-with the wait_for_borrows' acquire-load of
// the same zero — the waiter observes every store that any preceding
// retain-er (a frame's register) made before its release.
// ---------------------------------------------------------------------------

struct BzBorrowEntry {
    std::atomic<int> borrow_count{0};
    std::condition_variable cv;
};

namespace {
std::mutex g_borrow_mu;
std::unordered_map<void*, std::unique_ptr<BzBorrowEntry>> g_borrow_table;
}  // namespace

extern "C" void bz_register_borrow(void* obj) {
    if (!obj) return;
    std::lock_guard<std::mutex> lk(g_borrow_mu);
    auto& slot = g_borrow_table[obj];
    if (!slot) slot.reset(new BzBorrowEntry());
    slot->borrow_count.fetch_add(1, std::memory_order_acq_rel);
}

extern "C" void bz_release_borrow(void* obj) {
    if (!obj) return;
    std::unique_lock<std::mutex> lk(g_borrow_mu);
    auto it = g_borrow_table.find(obj);
    if (it == g_borrow_table.end()) return;
    BzBorrowEntry* e = it->second.get();
    int prev = e->borrow_count.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) {
        // Last borrower gone. Notify any waiters BEFORE the lock is released
        // (so the waiter can re-acquire the mutex in cv.wait's atomic
        // release/wait — releasing the lock before notify_all allows a
        // second waiter to acquire it and miss the wakeup). The entry stays
        // in the table; future register/release find it and increment again.
        // The unique_ptr keeps the entry alive across the cv.notify_all,
        // which would otherwise UAF if we erased the entry first.
        e->cv.notify_all();
    }
}

extern "C" void bz_wait_for_borrows(void* obj) {
    if (!obj) return;
    std::unique_lock<std::mutex> lk(g_borrow_mu);
    auto it = g_borrow_table.find(obj);
    if (it == g_borrow_table.end()) return;
    BzBorrowEntry* e = it->second.get();
    e->cv.wait(lk, [&] {
        // Re-check under the lock: a register/release may have raced with us
        // between the find and the wait. The predicate reads the atomic with
        // acquire so it synchronizes-with the release that drove the count
        // to zero.
        return e->borrow_count.load(std::memory_order_acquire) == 0;
    });
}
