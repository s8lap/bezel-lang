// bezel_runtime_refcounting.cpp — string/closure/box refcounting, static-owned-field registry.
// Split from bezel_runtime.cpp per #1414 stage 2 (behavior-preserving motion).
#include "bezel_runtime_internal.hpp"

#include <unordered_set>

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
    void* obj;       // static owned class/interface instance pointer
    void** slotAddr; // address of the static field slot
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
    //
    // #5472: TWO PHASE to make LSan-visible when disposal is skipped.
    // Phase A (unregister, before any disposal): walk the registry and null every
    // slotAddr. This breaks external reachability BEFORE Phase B runs, so if
    // disposal is ever skipped (a future bug), the allocation is already
    // unreachable and LSan reports it as a direct leak.
    // Phase B (dispose): drain, dispose, free as today. Phase B's copy-then-pop
    // discipline ensures a late push_back lands in the live vector.
    // Phase A must precede Phase B: nulling slots after free (as the original
    // fix did) is too late — if disposal is skipped, the free never runs and
    // the nulling never happens, so the slot still holds the pointer.
    for (auto& entry : *g_static_owned) {
        if (entry.slotAddr) {
            *entry.slotAddr = nullptr;
        }
    }
    // Phase B: drain LIFO
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

extern "C" void bz_register_static_owned(void* obj, void** slotAddr) {
    if (!obj) return;
    if (!g_static_owned) {
        g_static_owned = new std::vector<bz_static_owned_entry_t>();
        atexit(bz_dispose_static_owned);
    }
    g_static_owned->push_back({obj, slotAddr});
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
    int64_t new_count = __atomic_sub_fetch(rcp, 1, __ATOMIC_ACQ_REL);
    if (new_count > 0)
        return;
    // fix #4547: an over-release (two releases for one reference — a defect
    // at the call site) drives the count negative. Report it HERE, at the
    // release that first goes negative, instead of falling through to a
    // second free() below (see bz_refcount_overrelease_fail's comment).
    if (new_count < 0)
        bz_refcount_overrelease_fail("string");
    // Last reference gone: free the data buffer, then the header. Read the data
    // pointer at offset 0 (the layout's first field). free(NULL) is safe (the
    // empty-string case may carry a null data pointer).
    void* data;
    memcpy(&data, (uint8_t*)s, sizeof(void*));
    free(data);
    free(s);
}

// ---------------------------------------------------------------------------
// fix #6776 (§8.1 / §8.2 / §8.5 / §12.7 / stdlib §2.1 / §3.1, v0.340): the
// runtime-owned vtables of the vtable-less built-ins that reach `Object`.
//
// §8.2 fixes every heap object's runtime type identity at allocation, and
// §3.1/§5.1 pass a class value to an `Object` parameter representation-
// preservingly (no box). A StringBuilder, an Array<T> or a Mutex at `Object`
// is therefore the same pointer the typed code holds, and every uniform
// `Object` operation (ToString / Equals / GetHashCode / Dispose through
// `using var o: Object`, `o is T`, `o as T`) reads ITS word 0 as a vtable.
// Until this fix these three had no vtable word at all: `bz_obj_is_string`
// below, a NEGATIVE test ("word 0 is not a listed vtable"), answered "string"
// for them, so `Console.WriteLine(o)` printed the array's backing-store bytes
// as text and then string-released the header (garbage output, SIGSEGV in
// bz_array_free, "attempting double-free" for Mutex — the issue's m19/m22).
//
// The fix gives each one a real vtable in the class slot layout
// [desc, itmap, Dispose, ToString, Equals, GetHashCode] (CG35 / #946–#948;
// LlvmBackend_impl.hpp's kToStringSlot…) so the existing class dispatch
// simply works, and turns `bz_obj_is_string` POSITIVE for them. The vtables
// are runtime-owned (not codegen-emitted) for the same reason bz_fnval_vtable
// is: a per-module `bz$vtable$table` cannot list another module's globals,
// and an Array<int> built in module A is tested in module B.
//
// Dispose (slot 2) follows the CLASS protocol: it releases the object's
// contents and leaves the header for the caller's reclaim step
// (emitDisposeAndReclaim → free, or the checked-mode quarantine). For the two
// bz_array_t-backed kinds that is exactly bz_array_mark_disposed — elements
// released, backing store freed, `disposed` set, header kept — so a checked
// build's use-after-dispose guard keeps working through `Object` too. The
// typed release paths (bz_array_free, bz_mutex_dispose) are unchanged.
//
// ToString (slot 3) returns a +1 string: StringBuilder renders its text (the
// stdlib §3.1 override), the other two the §8.5 default — the qualified type
// name, read for an array from the per-instantiation descriptor codegen
// stamps at construction (word 1; `btd$builtin$<name>` = {self, null, name})
// so `Array<int>` renders `System.Collections.Generic.Array<int>` exactly as
// the static emitVtablelessBuiltinToString does. A runtime-produced array
// (Split, ToArray, …) has no stamp and renders the head alone. Equals (slot
// 4) and GetHashCode (slot 5) are the §8.3 reference-identity defaults, which
// is what the stdlib declares for all three (no override).
// ---------------------------------------------------------------------------
extern "C" void* bz_builtin_itmap[1] = { nullptr };

static void bz_array_obj_dispose(void* o) {
    bz_array_mark_disposed(o, ((bz_array_t*)o)->elem_disposer);
}
static void bz_sb_obj_dispose(void* o) {
    // The builder's parts are +1 strings (the static release passes the same
    // bz_string_release thunk, LlvmBackend_Disposal.cpp's StringBuilder arm).
    bz_array_mark_disposed(o, bz_string_release);
}
static void* bz_array_obj_tostring(void* o) {
    void* desc = ((bz_array_t*)o)->type_desc;
    if (desc) {
        // Past the chain's null terminator: the rendered name as the string
        // LITERAL codegen emitted (rc sentinel, never freed), returned as-is
        // like bz_fnval_obj_tostring returns its descriptor's name.
        void* n = ((void**)desc)[2];
        if (n) return n;
    }
    static const char name[] = "System.Collections.Generic.Array";
    return bz_make_string(name, (int64_t)(sizeof(name) - 1));
}
static void* bz_sb_obj_tostring(void* o) {
    // fix #7243 (spec §4.6 / stdlib §3.1, v0.344): render the builder's
    // CONTENT — its code points, at the width the content's largest code
    // point requires. This used to call the legacy byte-level
    // `bz_strbuilder_to_string` and then `bz_make_string`, a width-1
    // constructor that takes no width, so the width was dropped on the floor:
    // "日" (U+65E5, stored as two bytes at width 2) came back as the TWO code
    // points 229 and 101, while the builder's own ToString returned the ONE
    // code point 26085 — the two routes of one operation disagreed (§8.5 makes
    // them the same operation). See bz_strbuilder_to_string_obj.
    return bz_strbuilder_to_string_obj(o);
}
static void* bz_mutex_obj_tostring(void*) {
    static const char name[] = "System.Threading.Mutex";
    return bz_make_string(name, (int64_t)(sizeof(name) - 1));
}
static bool bz_builtin_obj_equals(void* a, void* b) { return a == b; }
static int32_t bz_builtin_obj_hash(void* o) { return bz_obj_identity_hash(o); }

extern "C" void* bz_array_desc[2] = { (void*)bz_array_desc, nullptr };
extern "C" void* bz_array_vtable[6] = {
    (void*)bz_array_desc, (void*)bz_builtin_itmap,
    (void*)bz_array_obj_dispose, (void*)bz_array_obj_tostring,
    (void*)bz_builtin_obj_equals, (void*)bz_builtin_obj_hash,
};
extern "C" void* bz_sb_desc[2] = { (void*)bz_sb_desc, nullptr };
extern "C" void* bz_sb_vtable[6] = {
    (void*)bz_sb_desc, (void*)bz_builtin_itmap,
    (void*)bz_sb_obj_dispose, (void*)bz_sb_obj_tostring,
    (void*)bz_builtin_obj_equals, (void*)bz_builtin_obj_hash,
};
extern "C" void* bz_mutex_desc[2] = { (void*)bz_mutex_desc, nullptr };
extern "C" void* bz_mutex_vtable[6] = {
    (void*)bz_mutex_desc, (void*)bz_builtin_itmap,
    (void*)bz_mutex_obj_dispose, (void*)bz_mutex_obj_tostring,
    (void*)bz_builtin_obj_equals, (void*)bz_builtin_obj_hash,
};

static inline bool bz_word0_is_builtin_vtable(void* word0) {
    return word0 == (void*)bz_array_vtable || word0 == (void*)bz_sb_vtable ||
           word0 == (void*)bz_mutex_vtable;
}

// fix #6776: codegen's hooks onto the tags above. `bz_sb_tag` retags the
// bz_array_t a `new StringBuilder` allocates (word 0 := bz_sb_vtable);
// `bz_array_set_type_desc` stamps the per-instantiation descriptor into word
// 1 at a construction site whose static type is known; `bz_array_inst_desc`
// reads it back for `o is Array<int>` — null for a non-array (including a
// StringBuilder, a string or a class instance) and for an unstamped array.
extern "C" void bz_sb_tag(void* arr) {
    if (arr) ((bz_array_t*)arr)->vtable = (void*)bz_sb_vtable;
}
extern "C" void bz_array_set_type_desc(void* arr, void* desc) {
    if (!arr) return;
    void* word0;
    memcpy(&word0, arr, sizeof(void*));
    if (word0 != (void*)bz_array_vtable) return;  // only a bz_array_t has the slot
    ((bz_array_t*)arr)->type_desc = desc;
}
// fix #6891 (§8.5, §12.7, v0.340): codegen's call-site stamp for an array a
// RUNTIME producer allocated (String.Split, a lazy ToArray, readers, ...):
// set the descriptor only when the array carries none, so an array that
// already holds its instantiation tag is never overwritten.
extern "C" void bz_array_stamp_if_unset(void* arr, void* desc) {
    if (!arr) return;
    void* word0;
    memcpy(&word0, arr, sizeof(void*));
    if (word0 != (void*)bz_array_vtable) return;
    if (!((bz_array_t*)arr)->type_desc) ((bz_array_t*)arr)->type_desc = desc;
}
extern "C" void* bz_array_inst_desc(void* o) {
    if (!o) return nullptr;
    void* word0;
    memcpy(&word0, o, sizeof(void*));
    if (word0 != (void*)bz_array_vtable) return nullptr;
    return ((bz_array_t*)o)->type_desc;
}

// §8.6 / §12.7 (#417): is the Object-typed value `obj` a string? A string in an
// Object slot is a bare bz_string* (word 0 = char* data); a class / boxed value
// / built-in exception has a vtable pointer at word 0. `vtable_table` is the
// null-terminated array codegen emits listing every vtable/box-vtable global.
// `obj` is a string iff it is non-null and its first word is NOT one of those
// vtables, nor one of the runtime's own (bz_fnval_vtable and, since #6776, the
// three built-in vtables above). (Runtime-internal vtables — IEnumerator state
// machines — are never stored in a plain Object slot, so omitting them from the
// table is sound. The vtable-less built-ins NOT yet tagged — LinkedList,
// Dictionary, the dict-backed sets, Semaphore, Random, … — still fall through
// to "string" here; that residual is tracked separately.)
extern "C" int32_t bz_obj_is_string(void* obj, void** vtable_table) {
    if (!obj) return 0;
    void* word0;
    memcpy(&word0, obj, sizeof(void*));  // obj's first word (vtable or data ptr)
    // fix #5978 (§8.1 / §12.7, v0.338): a function value at `Object` presents
    // the runtime's own vtable (BZ_CLOSURE_OBJVIEW_OFFSET), which no module's
    // table lists. Before this it was a bare closure cell whose word 0 is a
    // code pointer, so it was answered "string" here.
    if (word0 == (void*)bz_fnval_vtable) return 0;
    // fix #6776: likewise the runtime-owned built-in vtables (see above).
    if (bz_word0_is_builtin_vtable(word0)) return 0;
    if (vtable_table) {
        for (void** p = vtable_table; *p != nullptr; ++p)
            if (*p == word0) return 0;   // a known vtable → not a string
    }
    return 1;                            // not a vtable → a string
}

// fix #6776: is `obj` one of the three built-ins tagged above (word 0 is
// bz_array_vtable / bz_sb_vtable / bz_mutex_vtable)? The owned-field and
// collection-interface disposal sites split "user class instance" (slot-2
// Dispose + reclaim) from "array-backed built-in" (the typed free with the
// STATIC element policy, which the header's `elem_disposer` does not carry
// for a borrowing `Array<string>`); before #6776 that split was
// `bz_obj_is_string == 0` alone, because an untagged bz_array_t answered
// "string". A tagged one answers "not a string" now, so those sites AND this
// test to keep the built-in on its typed arm. Returns 1 (tagged built-in) /
// 0 (anything else, including null).
extern "C" int32_t bz_obj_is_tagged_builtin(void* obj) {
    if (!obj) return 0;
    void* word0;
    memcpy(&word0, obj, sizeof(void*));
    return bz_word0_is_builtin_vtable(word0) ? 1 : 0;
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
// domain (domain run-to-completion serializes continuations anyway, §24).
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
// increment; issue #672 typed env-disposer foundation; #4475 dispose flag).
//
// Function-typed values (lambda closures — capturing or not — and method
// references) are RUNTIME-MANAGED (§28.9): "Runtime management governs the
// function value itself — the heap-allocated closure object that backs the
// function value." Codegen lowers each such value to a heap cell laid out as
//   { void* fn      (off  0),
//     void* env     (off  8),
//     int64_t rc    (off 16),
//     void* envdtor (off 24),
//     int64_t kind  (off 32),
//     int8_t disposed (off 40),
//     int8_t hashed (off 41),
//     void* vtable  (off 48),     // #5978: the object view's header —
//     void* sig     (off 56) }    //   `bz_fnval_vtable` + formed-type descriptor
//   = 64 bytes.
// `fn` is the function/thunk pointer; `env` is null (non-capturing / static
// method reference) or points at a separately-allocated env block holding the
// capture pointers. The rc was appended AFTER env so fn (off 0) and env (off 8)
// keep their offsets: every codegen call site loads fn at offset 0 and env at
// GEP index 1, both unmoved. `kind` (off 32, §27.7.1 / #1544) tags the
// function-value equality rule (see bezel_runtime_internal.hpp's BZ_FVK_*
// constants); retain/release never read it. `disposed` (off 40, #4475) is the
// §28.2 use-after-dispose flag: 0 by default (calloc zero-fill), set to 1 by
// bz_closure_mark_disposed on an explicit `c.Dispose()`. bz_closure_release
// reads it at last-drop: when 1, the cell is QUARANTINED instead of freed (a
// deliberate leak parallel to bz_array_mark_disposed), so a post-dispose
// invoke's guard can read the flag and throw DisposedAccessException instead
// of UAF-reading freed memory.
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
// #4475 (§28.2): byte offset of the disposed flag (after kind). Single byte
// is enough — the flag is binary. calloc zero-fill makes a fresh cell read
// as "not disposed" (0), which is the only safe default.
static const int64_t BZ_CLOSURE_DISPOSED_OFFSET = 40;

extern "C" void bz_closure_retain(void* c) {
    c = bz_closure_base(c);   // #5978: either view names the same cell
    if (!c) return;
    int64_t* rcp = (int64_t*)((uint8_t*)c + BZ_CLOSURE_RC_OFFSET);
    __atomic_add_fetch(rcp, 1, __ATOMIC_ACQ_REL);
}

extern "C" void bz_closure_release(void* c) {
    c = bz_closure_base(c);   // #5978: either view names the same cell
    if (!c) return;
    int64_t* rcp = (int64_t*)((uint8_t*)c + BZ_CLOSURE_RC_OFFSET);
    int64_t new_count = __atomic_sub_fetch(rcp, 1, __ATOMIC_ACQ_REL);
    if (new_count > 0)
        return;
    // fix #4547: an over-release (two releases for one reference — a defect
    // at the call site, e.g. #4540's racing releases) drives the count
    // negative. Report it HERE, at the release that first goes negative,
    // instead of falling through to a second free() below (see
    // bz_refcount_overrelease_fail's comment).
    if (new_count < 0)
        bz_refcount_overrelease_fail("closure");
    // Last reference gone.
    // #5978 / §8.3: reap the assigned identity hash of a cell that drew one
    // (bz_closure_value_hash sets the byte), as #2058 does for a class.
    int8_t hashed = 0;
    memcpy(&hashed, (uint8_t*)c + BZ_CLOSURE_HASHED_OFFSET, sizeof(int8_t));
    if (hashed) bz_obj_identity_hash_forget(c);
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
    // #4475 (§28.2): if the closure was EXPLICITLY DISPOSED (codegen emits
    // bz_closure_mark_disposed under --checked-disposal on `c.Dispose()`),
    // the disposed flag is 1. The cell must stay alive (quarantined) so the
    // closure-invoke guard can read the flag and throw DisposedAccessException
    // instead of UAF-reading a freed cell at fn (offset 0). free the env block
    // (it is uniquely owned) but NOT the cell. Outside --checked-disposal the
    // flag is never written, so the legacy free-on-last-drop path is unchanged
    // and calloc's zero-fill keeps the byte at 0.
    int8_t disposed = 0;
    memcpy(&disposed, (uint8_t*)c + BZ_CLOSURE_DISPOSED_OFFSET, sizeof(int8_t));
    free(env);  // env is owned by this cell; release it either way.
    if (disposed) {
        // Quarantine: do NOT free(c). The cell becomes a deliberate leak
        // until the process exits (the same lifetime-end semantics as
        // bz_array_mark_disposed / bz_dict_mark_disposed). The disposed flag
        // is still readable, and the rc remains 0, so a future retain on the
        // same pointer would be unsound — but the codegen stops retaining
        // post-dispose (the `c.Dispose()` arm in --checked-disposal nulls
        // the slot after the mark, parallel to channel/semaphore, so any
        // second access goes through the same guard).
        return;
    }
    free(c);
}

// #4475 (§28.2): mark a closure-typed value as disposed. Called by codegen
// BEFORE the explicit `c.Dispose()` release under --checked-disposal, so the
// release's last-drop path sees the flag and quarantines instead of freeing.
// Idempotent — repeated calls are no-ops. null is a no-op.
extern "C" void bz_closure_mark_disposed(void* c) {
    c = bz_closure_base(c);
    if (!c) return;
    int8_t one = 1;
    memcpy((uint8_t*)c + BZ_CLOSURE_DISPOSED_OFFSET, &one, sizeof(int8_t));
}

// #4475 (§28.2): read the per-instance disposed flag. Returns 0 for null
// (a null closure is never "disposed" — it never held anything). The codegen
// guard at the closure-invoke lower site calls this and throws
// DisposedAccessException on a nonzero return.
extern "C" int8_t bz_closure_is_disposed(void* c) {
    c = bz_closure_base(c);
    if (!c) return 0;
    int8_t d = 0;
    memcpy(&d, (uint8_t*)c + BZ_CLOSURE_DISPOSED_OFFSET, sizeof(int8_t));
    return d;
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
    int64_t new_count = __atomic_sub_fetch(rcp, 1, __ATOMIC_ACQ_REL);
    if (new_count > 0)
        return;
    // fix #4547: an over-release (two releases for one reference — a defect
    // at the call site) drives the count negative. Report it HERE, at the
    // release that first goes negative, instead of falling through to a
    // second free() below (see bz_refcount_overrelease_fail's comment).
    if (new_count < 0)
        bz_refcount_overrelease_fail("box");
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
// Lazy atexit-registration flag: set the first time any code calls
// bz_register_borrow, so a program that never crosses an async-method
// borrow boundary never installs the exit-time drain. See bz_register_borrow
// and bz_drain_borrow_table.
bool g_borrow_atexit_registered = false;
}  // namespace

// Forward-declared: defined further below (after bz_wait_for_borrows). The
// forward declaration lets bz_register_borrow install it as an atexit handler
// at first invocation without a header split.
static void bz_drain_borrow_table(void);

extern "C" void bz_register_borrow(void* obj) {
    if (!obj) return;
    std::lock_guard<std::mutex> lk(g_borrow_mu);
    // Lazy atexit registration (matches bz_register_static_owned's pattern):
    // a process that never crosses an async-method borrow boundary never
    // installs the drain, never allocates the table, and pays nothing for
    // #3460 at exit — exit-time behavior is bit-identical to a build that
    // doesn't link the borrow tracker.
    if (!g_borrow_atexit_registered) {
        g_borrow_atexit_registered = true;
        std::atexit(bz_drain_borrow_table);
    }
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
// fix #3460 follow-up: process-exit drain for the borrow-tracker side table.
//
// The entry-retain-not-erase design (above) keeps BzBorrowEntry instances alive
// across count==0 so a late register on the same object pointer can find them.
// For long-running services that capture many objects across many async tasks,
// the side table therefore GROWS without bound for the process lifetime — at
// exit every entry's BzBorrowEntry (and its condition variable) is still live,
// and AddressSanitizer's exit-time leak scan flags the heap allocation per
// entry as a leak.
//
// The drain runs as a process-finalization callback (atexit): at exit every
// thread is done, so every entry's count must be 0 (any live borrow would be
// a soundness bug — the bz_wait_for_borrows at the OWNER's free site would
// still be blocked on the cv, and the OWNER's free(obj) would not have been
// reached). We therefore drain unconditionally: take the lock, swap the table
// for an empty local, notify_all any waiters that may be racing the exit
// (none in a well-formed program; the notify_all is defensive), then release
// the lock and let the local's destruction free every BzBorrowEntry.
//
// The drain does NOT wait for a particular entry's count to drop — by exit
// the count is 0 by construction (see the argument above), and even if a
// count were non-zero, waiting at exit would deadlock: the thread that owns
// the borrow is already gone.
//
// Registered as an atexit handler on FIRST bz_register_borrow invocation
// (matching the static-owned registry's lazy-atexit pattern), so a process
// that never registers a borrow pays nothing — the handler is never
// installed, the table is never created, and exit's static destruction
// order is the same as before #3460.
static void bz_drain_borrow_table(void) {
    std::unordered_map<void*, std::unique_ptr<BzBorrowEntry>> drained;
    {
        std::lock_guard<std::mutex> lk(g_borrow_mu);
        // Swap so the table is empty after the drain; a stray register after
        // the drain (impossible: all threads are done, but defensive) would
        // find an empty table and allocate a fresh entry, no UAF.
        drained.swap(g_borrow_table);
        // Wake any waiters still racing the exit (defensive — see comment).
        for (auto& kv : drained) {
            kv.second->cv.notify_all();
        }
    }
    // Release the lock first so the per-entry cv destructors (which can take
    // their own locks internally on some libstdc++ versions) don't recurse on
    // g_borrow_mu. Then let `drained` fall out of scope — each unique_ptr
    // frees its BzBorrowEntry (which destructs the std::condition_variable;
    // the libstdc++ cv destructor is lock-free).
    (void)drained.size();
}

// ---------------------------------------------------------------------------
// #4602 (§28.2, v0.338): checked-disposal escaped set — see the declarations
// in bezel_runtime.h. §28.2 lets checked builds defer reclamation of a
// disposed block ("the implementation may quarantine it"); it does not
// require retaining a block no borrow can reach. The set records every
// pointer codegen saw leave a frame; a release site that is otherwise
// eligible frees the block only when its pointer is absent.
// ---------------------------------------------------------------------------
namespace {
std::mutex g_cd_escaped_mu;
std::unordered_set<void*>* g_cd_escaped = nullptr;  // reachable via a global
int g_cd_quarantine_all = -1;  // -1 = not yet read from the environment
}  // namespace

extern "C" void bz_cd_mark_escaped(void* obj) {
    if (!obj) return;
    std::lock_guard<std::mutex> lk(g_cd_escaped_mu);
    if (!g_cd_escaped) g_cd_escaped = new std::unordered_set<void*>();
    g_cd_escaped->insert(obj);
}

extern "C" void bz_cd_mark_escaped_slot(void* slot) {
    if (!slot) return;
    bz_cd_mark_escaped(*static_cast<void**>(slot));
}

extern "C" int8_t bz_cd_may_free(void* obj) {
    if (!obj) return 0;
    std::lock_guard<std::mutex> lk(g_cd_escaped_mu);
    if (g_cd_quarantine_all < 0) {
        const char* v = getenv("BEZEL_CHECKED_DISPOSAL_QUARANTINE_ALL");
        g_cd_quarantine_all = (v && v[0] && strcmp(v, "0") != 0) ? 1 : 0;
    }
    if (g_cd_quarantine_all) return 0;
    if (g_cd_escaped && g_cd_escaped->count(obj)) return 0;
    return 1;
}

// ---------------------------------------------------------------------------
// fix #4509 (§27.5 / §28.9, v0.306): CAPTURE-CELL reference counting.
//
// The full protocol is documented at the declarations in bezel_runtime.h. In
// one line: the declaring frame's reference to a capture cell is implicit and
// unannounced, every closure env that captures the cell announces itself with
// `bz_cell_retain`, and each of them — envs and frame alike — lets go with
// `bz_cell_release`; the last one out frees the cell.
//
// WHY A SIDE TABLE and not a header word: see bezel_runtime.h. The pattern is
// the one `g_identity_hashes` above already uses, mutex and all.
//
// WHY THE FIRST RETAIN RECORDS 2: the frame's reference exists from the
// `calloc` in `boxLocalSlot` and is never announced — announcing it would mean
// teaching all three allocation sites which cells are refcounted, which they
// cannot know (the classification is made later, at the capture site). So the
// table's invariant is "entry present ⇒ value == frame + live envs", and the
// first env to arrive establishes it at 2. A cell that no env ever retains
// stays out of the table entirely, and `bz_cell_release` on it is a plain
// `free` — which is exactly what the pre-#4509 scope-end path emitted, so an
// unclassified cell is byte-identical.
//
// EXACTLY-ONCE: codegen emits one retain per env capture-store, one release in
// that env's envdtor, and one release at the declaring scope's end (on both the
// normal and the unwind edge, a given exit going one way only). So releases ==
// 1 + N and the count reaches 0 exactly once. An env that is never released
// (a leaked closure) leaves the cell alive — the leak direction, which is the
// PRIME DIRECTIVE's safe side.
//
// ADDRESS REUSE happens constantly and is handled explicitly, not avoided: a
// cell's `calloc` block is freed here and the very next iteration's cell
// typically lands on the same address (measured 197 of 200 iterations on
// #4509's own event-subscription loop). Before #4564 the entry was ERASED at
// the last drop, so a recycled address simply found no entry — correct, but
// only by throwing away the fact that the cell had died, which is exactly what
// #4564 needed back. See the TOMBSTONE note below for how both are now had at
// once.
//
// ---------------------------------------------------------------------------
// fix #4564 (§27.5 / §28.9, v0.306): TOMBSTONES — detecting an over-release
// instead of double-freeing on it.
//
// THE DEFECT. `bz_cell_release` has two arms: a tracked cell decrements and
// frees at zero, and an UNTRACKED cell (no env ever retained it) is freed
// outright, because the frame is then the sole holder. Erasing the entry at
// the last drop collapsed those two states into one. A release PAST the last
// drop — an over-release, always a defect at the call site — found no entry
// and took the untracked arm, so it called `free()` on a block this function
// had already freed. That is the same double free #4547 fixed for
// string/closure/box, reached by a different route: there the post-decrement
// count went to -1 and fell through a `> 0` guard, here there is no count
// left to go negative. #4547's three-way rewrite therefore does not transfer.
//
// THE FIX. The last drop now leaves a TOMBSTONE — the entry stays, with the
// count at `BZ_CELL_TOMBSTONE` — instead of erasing. A release that finds a
// tombstone is an over-release and is reported by the same
// `bz_refcount_overrelease_fail` #4547 introduced, before any free.
//
// A TOMBSTONE IS NOT A LEAK OF THE CELL: the block is still freed at the last
// drop, on the same instruction as before. What survives is the 1 map entry,
// and only until one of the two reclaim routes takes it:
//
//   (1) RETAIN CLEARS IT. A retain is only ever emitted at a live cell's
//       capture-store, so a retain landing on a tombstone means `calloc` has
//       recycled the address into a NEW cell — the entry must be re-armed at 2
//       (frame + this env), exactly as the not-present arm does, NOT
//       incremented off the tombstone. This arm is load-bearing, not
//       defensive: it fires on 197 of the 200 iterations of #4509's repro, and
//       without it that loop would count `0 + 1 == 1` for a cell with two
//       holders and free it at the FIRST release — turning #4564's latent
//       double free into an immediate use-after-free on every iteration.
//
//   (2) A BOUNDED RING EVICTS IT. `g_cell_dead_ring` holds the last
//       BZ_CELL_TOMBSTONE_RING tombstoned addresses; tombstoning a cell evicts
//       the oldest and erases its entry. Without this the table would grow by
//       one node per dead cell for the process lifetime — unbounded growth on
//       every CORRECT program, which is a worse trade than the latent double
//       free it is buying (#4509 exists because 8 B per capture was already
//       too much). The eviction re-checks that the evicted entry is still a
//       tombstone, so it can never drop a live cell's count.
//
// So the detection window is the last BZ_CELL_TOMBSTONE_RING cell deaths, and
// past that the behaviour degrades to exactly what it was before this fix.
// That is deliberate: an over-release is emitted by the SAME frame/envdtor
// pair that issued the legitimate releases, so it arrives within a handful of
// cell deaths, while the memory cost of a wider window is paid by every
// correct program forever.
//
// WHY ABORT AND NOT "skip the free and carry on". A tombstone hit could in
// principle also be a NEW, never-retained cell at a recycled address taking
// the untracked arm — which would make the abort a false positive on a correct
// program. It cannot be, because codegen emits `bz_cell_release` at exactly
// two sites (the declaring scope's `Disposal::cellRelease` and the env's
// envdtor), both gated on the same `CaptureEntry::retainCellInEnv` that gates
// the retain, and `boxLocalSlot` allocates the cell AT the capture site that
// retains it — so a non-null cell reaching either release site has been
// retained. Measured on five programs covering all three armed shapes
// (per-iteration cell, one cell shared by N envs, env outliving its frame):
// 504 releases, of which 0 took the untracked arm. The untracked arm is kept
// exactly as it was for the cells that legitimately never enter the table.
// ---------------------------------------------------------------------------
static std::unordered_map<void*, int64_t> g_cell_rc;
static std::mutex                         g_cell_rc_mtx;

// A count a live entry can never hold: the first retain records 2 and a
// decrement that reaches 0 tombstones instead of returning.
static const int64_t     BZ_CELL_TOMBSTONE      = 0;
static const std::size_t BZ_CELL_TOMBSTONE_RING = 1024;
static void*             g_cell_dead_ring[BZ_CELL_TOMBSTONE_RING];
static std::size_t       g_cell_dead_head = 0;

// Tombstone `cell` (whose entry the caller has just driven to
// BZ_CELL_TOMBSTONE) and evict the oldest tombstone, so the table holds at
// most BZ_CELL_TOMBSTONE_RING of them. Caller holds g_cell_rc_mtx.
static void bz_cell_tombstone_record(void* cell) {
    void* evicted = g_cell_dead_ring[g_cell_dead_head];
    // `evicted == cell` is the ring having wrapped onto an earlier life of
    // this same address; erasing then would drop the tombstone we are in the
    // middle of installing.
    if (evicted && evicted != cell) {
        auto e = g_cell_rc.find(evicted);
        // Only if it is STILL a tombstone: the address may have been recycled
        // into a live cell since, and erasing that would lose a real count.
        if (e != g_cell_rc.end() && e->second == BZ_CELL_TOMBSTONE)
            g_cell_rc.erase(e);
    }
    g_cell_dead_ring[g_cell_dead_head] = cell;
    g_cell_dead_head = (g_cell_dead_head + 1) % BZ_CELL_TOMBSTONE_RING;
}

extern "C" void bz_cell_retain(void* cell) {
    if (!cell) return;
    std::lock_guard<std::mutex> lk(g_cell_rc_mtx);
    auto it = g_cell_rc.find(cell);
    if (it == g_cell_rc.end()) g_cell_rc.emplace(cell, 2);  // frame + this env
    // fix #4564 route (1): a tombstone at a retained address is a RECYCLED
    // address, not a dead cell coming back — re-arm at frame + this env.
    else if (it->second == BZ_CELL_TOMBSTONE) it->second = 2;
    else ++it->second;
}

extern "C" void bz_cell_release(void* cell) {
    if (!cell) return;
    {
        std::lock_guard<std::mutex> lk(g_cell_rc_mtx);
        auto it = g_cell_rc.find(cell);
        if (it != g_cell_rc.end()) {
            // fix #4564: a tombstone is a cell this function already drove to
            // zero and freed. Reaching it again is an over-release — two
            // releases for one reference — so report it HERE, before the free
            // below turns it into a double free that names the allocator
            // instead of the bug (#4547's argument, same §28.9 invariant).
            if (it->second == BZ_CELL_TOMBSTONE)
                bz_refcount_overrelease_fail("capture cell");
            if (--it->second > 0) return;
            // Last drop. The free below still happens on this call; what the
            // tombstone keeps is the knowledge that it did.
            bz_cell_tombstone_record(cell);
        }
        // Not in the table: no env ever retained it, so the frame is the only
        // holder and this release is the cell's one and only free.
    }
    free(cell);
}
