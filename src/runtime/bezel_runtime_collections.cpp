// bezel_runtime_collections.cpp — arrays, dictionaries, linked lists, Random, array/LINQ enumerators, GroupBy, Buffer<T>.
// Split from bezel_runtime.cpp per #1414 stage 2 (behavior-preserving motion).
#include "bezel_runtime_internal.hpp"

// fix #4748 / stdlib §4.4 (v0.307): thread-local sort-in-progress tracking.
// Approach: during bz_array_sort_closure, any structural mutator (Add/RemoveAt/
// InsertAt/Clear) calls longjmp to escape back. bz_array_sort_closure uses
// setjmp to catch this, and after std::sort returns, checks the flag and
// throws InvalidOperationException so the Bezel landing pad catches it.
#include <csetjmp>
thread_local void*  bz_sort_in_progress_arr = nullptr;  // null = no sort in progress
thread_local bool   bz_sort_mutation_attempted = false;  // true = mutation during sort
thread_local std::jmp_buf* bz_sort_jmp_buf = nullptr;   // non-null = jump target

// fix #4748 / stdlib §4.4: the InvalidOperationException vtable, registered at program
// startup via bz_ioe_vtable_init. Used by bz_throw_invalid_operation and the shim's
// catch block to allocate a well-formed exception object.
void* bz_ioe_vtable = nullptr;

// ---------------------------------------------------------------------------
// fix #3026 (spec §28.2 / §28.9 / §8.1, stdlib §4.1 / §4.6-§4.9, v0.240):
// the polymorphic collection-interface dispose dispatcher. The dispose chain
// in LlvmBackend_Calls2.cpp's `isCollectionInterfaceName` arm (IReadOnlyCollection<T>
// / ICollection<T> / IOwningCollection<T>) emits a call to this helper
// instead of a single bz_*_free: the static type erases which concrete
// backing the runtime value holds, and the three backings have incompatible
// structs (bz_array_t / bz_llist_t / bz_dict_t). Each struct carries a
// `kind` field (see the BZ_RUNTIME_KIND_* constants on bz_array_t) set by
// its alloc fn; this helper reads the field and dispatches to the matching
// free. `dispose_elem` is the static-element-type disposer the caller
// computed (null for borrowing, the class-element disposer for IOwningCollection<T>)
// and is sound for every backing because bz_array_free / bz_llist_free /
// bz_dict_free all accept the same dispose_elem signature (and a null
// dispose_elem is a no-op for Set<T> where bz_dict_free does not dispose
// values).
//
// PROBE ORDER. bz_array_t and bz_llist_t both carry their kind at the same
// offset 48 (bz_llist_t's struct has an 8-byte padding slot for this
// reason — see the comment on the struct). bz_dict_t's struct is larger and
// carries its kind at offset 88 (its order_count field occupies offset 56,
// a non-negative count that cannot collide with the negative sentinels).
// The probes walk offset 48 first, then offset 88.
//
// SENTINEL VALUES. The kind sentinels are negative int64 values (-1 / -2 /
// -3). order_count (the bz_dict_t field at offset 56) is a non-negative
// count of valid insertions — it cannot equal a negative sentinel — so the
// first probe can never false-positive a dict as an array or llist. A
// positive sentinel would not have this property (a freshly constructed
// dict has order_count == 0, but a populated dict's order_count grows
// without a natural upper bound and could in principle collide with a
// small positive sentinel).
//
// UNTAGGED POINTER. An untagged pointer (BZ_RUNTIME_KIND_NONE = 0, or any
// other value the probes do not match) is treated as a bounded leak —
// return without doing anything. The trade is the same one #2967/#2970
// record for the prior diagnostic-untagged positions, and the only path
// that produces an untagged pointer is fresh stack scratch the runtime
// never hands to codegen, so the no-op is defensive rather than
// observable. // per §28.9 / §28.2
// ---------------------------------------------------------------------------
extern "C" void bz_runtime_dispose(void* p, void (*dispose_elem)(void*)) {
    if (!p) return;
    // Probe offset 48 first — both bz_array_t and bz_llist_t carry the
    // kind here. The negative sentinel values rule out a bz_dict_t
    // false-positive (its order_count at offset 56 is non-negative).
    int64_t k48 = 0;
    memcpy(&k48, (uint8_t*)p + 48, sizeof(int64_t));
    if (k48 == BZ_RUNTIME_KIND_ARRAY) { bz_array_free(p, dispose_elem); return; }
    if (k48 == BZ_RUNTIME_KIND_LLIST) { bz_llist_free(p, dispose_elem); return; }
    // bz_dict_t's kind lives at offset 88 (struct ends at 96). Reading
    // there is safe — the alloc sized the struct for it.
    int64_t k88 = 0;
    memcpy(&k88, (uint8_t*)p + 88, sizeof(int64_t));
    if (k88 == BZ_RUNTIME_KIND_DICT) { bz_dict_free(p, dispose_elem); return; }
    // untagged — bounded leak (see header).
}

extern "C" int64_t bz_runtime_kind(void* p) {
    if (!p) return BZ_RUNTIME_KIND_NONE;
    int64_t k48 = 0;
    memcpy(&k48, (uint8_t*)p + 48, sizeof(int64_t));
    if (k48 == BZ_RUNTIME_KIND_ARRAY) return k48;
    if (k48 == BZ_RUNTIME_KIND_LLIST) return k48;
    int64_t k88 = 0;
    memcpy(&k88, (uint8_t*)p + 88, sizeof(int64_t));
    if (k88 == BZ_RUNTIME_KIND_DICT) return k88;
    return BZ_RUNTIME_KIND_NONE;
}

// ---------------------------------------------------------------------------
// bz_array_t — dynamic array
// ---------------------------------------------------------------------------


void* bz_array_alloc(int64_t elem_size, int64_t initial_cap) {
    // §31.4: both allocations terminate on failure. The `data` check in
    // particular is load-bearing — the pre-existing code set `capacity` whether
    // or not the buffer materialized, so an OOM here handed back a header
    // claiming capacity over a NULL buffer and the first add wrote through it.
    auto* arr = (bz_array_t*)calloc(1, sizeof(bz_array_t));
    if (!arr) bz_alloc_fail("Array<T> header");
    arr->kind = BZ_RUNTIME_KIND_ARRAY;  // fix #3026
    if (initial_cap > 0) {
        arr->data = calloc((size_t)initial_cap, (size_t)elem_size);
        if (!arr->data) bz_alloc_fail("Array<T> backing store");
        arr->capacity = initial_cap;
    }
    return arr;
}

int64_t bz_array_len(void* arrp) {
    return arrp ? ((bz_array_t*)arrp)->count : 0;
}

// stdlib §4.4/§4.5: the array-backed collection's allocated capacity (#741).
int64_t bz_array_capacity(void* arrp) {
    return arrp ? ((bz_array_t*)arrp)->capacity : 0;
}

// §4.14: allocate a bz_array_t with n pre-filled (zero-initialized) slots.
// Used by `new OwningBuffer<T>(n)` so that index-assignment data[i] := v is valid
// for i in 0..<n without requiring Add() first. All n elements are calloc'd to 0.
void* bz_array_alloc_count_n(int64_t elem_size, int64_t n) {
    auto* arr = (bz_array_t*)calloc(1, sizeof(bz_array_t));
    arr->kind = BZ_RUNTIME_KIND_ARRAY;  // fix #3026
    if (n > 0) {
        arr->data = calloc((size_t)n, (size_t)elem_size);
        arr->capacity = n;
        arr->count    = n;  // all slots are pre-populated (zero-valued)
    }
    return arr;
}

// fix #1762 finding R5: realloc-into-same-pointer leaks on failure — the
// canonical anti-pattern `p = realloc(p, n)` overwrites `p` with NULL, losing
// the only pointer to the original block. Use a tmp pointer.
//
// "collection growth" is named verbatim in §31.4's allocation-failure list, so
// a failed growth TERMINATES. That is also the only safe option here: this
// function returns void and its callers write immediately afterwards, so any
// non-terminating failure posture (silently not growing) would leave them
// writing past a buffer that was never grown — a heap-buffer-overflow, which
// is strictly worse than the NULL-deref crash the tmp-pointer change removed.
static void bz_array_grow(bz_array_t* arr, int64_t elem_size, int64_t min_cap) {
    int64_t new_cap = arr->capacity < 4 ? 4 : arr->capacity * 2;
    if (new_cap < min_cap) new_cap = min_cap;
    void* tmp = realloc(arr->data, (size_t)(new_cap * elem_size));
    if (!tmp) bz_alloc_fail("collection growth (Array<T> backing store)");
    arr->data = tmp;
    // Zero new entries for safety
    if (new_cap > arr->capacity) {
        memset((char*)arr->data + arr->capacity * elem_size, 0,
               (size_t)((new_cap - arr->capacity) * elem_size));
    }
    arr->capacity = new_cap;
}

// fix #4748 / stdlib §4.4: forward declaration — implementation below.
static inline void bz_throw_invalid_operation(void);

void bz_array_add(void* arrp, const void* elem, int64_t elem_size) {
    auto* arr = (bz_array_t*)arrp;
    // fix #4748 / stdlib §4.4: structural mutation during sort callback → longjmp out.
    // Using longjmp instead of throw avoids std::terminate() from C++ exceptions
    // propagating through std::sort.
    if (bz_sort_in_progress_arr == arr && bz_sort_jmp_buf) {
        bz_sort_mutation_attempted = true;
        std::longjmp(*bz_sort_jmp_buf, 1);
        return;  // not reached
    }
    if (arr->count >= arr->capacity) bz_array_grow(arr, elem_size, arr->count + 1);
    memcpy((char*)arr->data + arr->count * elem_size, elem, (size_t)elem_size);
    arr->count++;
    arr->mod_count++;  // §29.12 / #1232: structural mutation
}

void bz_array_get(void* arrp, int64_t idx, void* out, int64_t elem_size) {
    auto* arr = (bz_array_t*)arrp;
    memcpy(out, (char*)arr->data + idx * elem_size, (size_t)elem_size);
}

void bz_array_set(void* arrp, int64_t idx, const void* elem, int64_t elem_size) {
    auto* arr = (bz_array_t*)arrp;
    memcpy((char*)arr->data + idx * elem_size, elem, (size_t)elem_size);
}

// issue #388 / stdlib §4.5: OwningArray<T>'s `set(owned)` indexer — dispose the
// element currently at idx (a pointer-sized owned object reference) before
// overwriting the slot with the new owner. Reading the old pointer before the
// store guarantees exactly-once disposal of the displaced owner.
void bz_array_set_disposing(void* arrp, int64_t idx, const void* elem,
                            int64_t elem_size, void (*dispose_elem)(void*)) {
    if (!arrp) return;
    auto* arr = (bz_array_t*)arrp;
    if (dispose_elem && arr->data) {
        void* prev = ((void**)arr->data)[idx];
        if (prev) dispose_elem(prev);
    }
    memcpy((char*)arr->data + idx * elem_size, elem, (size_t)elem_size);
}

void bz_array_remove_at(void* arrp, int64_t idx, int64_t elem_size) {
    auto* arr = (bz_array_t*)arrp;
    // fix #4748 / stdlib §4.4: structural mutation during sort callback → longjmp out.
    if (bz_sort_in_progress_arr == arr && bz_sort_jmp_buf) {
        bz_sort_mutation_attempted = true;
        std::longjmp(*bz_sort_jmp_buf, 1);
        return;  // not reached
    }
    if (idx < 0 || idx >= arr->count) return;
    // Shift elements left
    int64_t tail = arr->count - idx - 1;
    if (tail > 0) {
        memmove((char*)arr->data + idx * elem_size,
                (char*)arr->data + (idx + 1) * elem_size,
                (size_t)(tail * elem_size));
    }
    arr->mod_count++;  // §29.12 / #1232: structural mutation (also covers pop_at and remove_at_disposing which delegate here)
    arr->count--;
}

// stdlib §4.5: insert at idx, shifting [idx, count) up by one.
void bz_array_insert_at(void* arrp, int64_t idx, const void* elem,
                        int64_t elem_size) {
    auto* arr = (bz_array_t*)arrp;
    // fix #4748 / stdlib §4.4: structural mutation during sort callback → longjmp out.
    if (bz_sort_in_progress_arr == arr && bz_sort_jmp_buf) {
        bz_sort_mutation_attempted = true;
        std::longjmp(*bz_sort_jmp_buf, 1);
        return;  // not reached
    }
    if (idx < 0) idx = 0;
    if (idx > arr->count) idx = arr->count;            // clamp; idx==count appends
    if (arr->count >= arr->capacity)
        bz_array_grow(arr, elem_size, arr->count + 1);
    int64_t tail = arr->count - idx;                    // elements to shift up
    if (tail > 0) {
        memmove((char*)arr->data + (idx + 1) * elem_size,
                (char*)arr->data + idx * elem_size,
                (size_t)(tail * elem_size));
    }
    memcpy((char*)arr->data + idx * elem_size, elem, (size_t)elem_size);
    arr->count++;
    arr->mod_count++;  // §29.12 / #1232: structural mutation
}

void bz_array_clear(void* arrp) {
    if (arrp) {
        auto* arr = (bz_array_t*)arrp;
        // fix #4748 / stdlib §4.4: structural mutation during sort callback → longjmp out.
        if (bz_sort_in_progress_arr == arr && bz_sort_jmp_buf) {
            bz_sort_mutation_attempted = true;
            std::longjmp(*bz_sort_jmp_buf, 1);
            return;  // not reached
        }
        arr->count = 0;
        arr->mod_count++;  // §29.12 / #1232: structural mutation
    }
}

// stdlib §4.5: dispose every non-null class-typed element, then empty.
void bz_array_clear_disposing(void* arrp, void (*dispose_elem)(void*)) {
    if (!arrp) return;
    auto* arr = (bz_array_t*)arrp;
    // fix #4748 / stdlib §4.4: structural mutation during sort callback.
    // Dispose elements FIRST (before any jump), then signal the mutation.
    // Since the sort result is abandoned anyway when a mutation is detected,
    // disposing before longjmp is correct — the array contents are discarded.
    if (dispose_elem && arr->data) {
        for (int64_t i = 0; i < arr->count; ++i) {
            void* elem_ptr = ((void**)arr->data)[i];
            // Null the slot before disposing so the live range never re-reaches
            // a disposed object (mirrors the spec's move-and-null pattern).
            ((void**)arr->data)[i] = nullptr;
            if (elem_ptr) dispose_elem(elem_ptr);
        }
    }
    arr->count = 0;
    arr->mod_count++;  // §29.12 / #1232: structural mutation
    if (bz_sort_in_progress_arr == arr && bz_sort_jmp_buf) {
        bz_sort_mutation_attempted = true;
        std::longjmp(*bz_sort_jmp_buf, 1);
        // not reached
    }
}

// stdlib §4.5: dispose the non-null element at idx, then remove it.
void bz_array_remove_at_disposing(void* arrp, int64_t idx, int64_t elem_size,
                                  void (*dispose_elem)(void*)) {
    if (!arrp) return;
    auto* arr = (bz_array_t*)arrp;
    if (idx < 0 || idx >= arr->count) return;
    if (dispose_elem && arr->data) {
        void* elem_ptr = ((void**)arr->data)[idx];
        if (elem_ptr) dispose_elem(elem_ptr);
    }
    bz_array_remove_at(arrp, idx, elem_size);
}

// fix #642 / stdlib §4.5: PopAt(i) — copy element at idx into *out (transferring
// ownership to the caller) then remove the slot WITHOUT disposal.  The caller
// holds the sole reference after this call; it must dispose when done.
// Unlike bz_array_remove_at_disposing this never invokes a disposer.
void bz_array_pop_at(void* arrp, int64_t idx, void* out, int64_t elem_size) {
    if (!arrp || !out) return;
    auto* arr = (bz_array_t*)arrp;
    if (idx < 0 || idx >= arr->count) return;
    // Copy the element bytes BEFORE shifting so the pointer/value is captured.
    memcpy(out, (char*)arr->data + idx * elem_size, (size_t)elem_size);
    // Remove the slot (no disposal — ownership transferred to caller).
    bz_array_remove_at(arrp, idx, elem_size);
}

// #2608 / §28.11 / stdlib §4.3: record an OWNING array-backed collection's
// element disposer in the header at CONSTRUCTION. Called by codegen only for a
// concrete owning collection (`new OwningArray<T>()` etc.) whose element policy
// yields a disposer thunk; never for a borrowing collection, which leaves the
// calloc-zeroed NULL in place. Idempotent and null-safe.
void bz_array_set_elem_disposer(void* arrp, void (*dispose_elem)(void*)) {
    if (!arrp) return;
    ((bz_array_t*)arrp)->elem_disposer = dispose_elem;
}

// #2608: free an array whose element ownership is not statically known at the
// free site — an `owned` field typed by one of the read-only interfaces SHARED
// by the borrowing and owning collection hierarchies (stdlib §4.3), where the
// dynamic backing may be either `Array<T>` (borrowing — elements must NOT be
// disposed) or `OwningArray<T>` (owning — elements MUST be disposed). The
// recorded header disposer is the only sound discriminator; an untagged
// (borrowing) array yields NULL and this degenerates EXACTLY to today's
// `bz_array_free(arr, nullptr)`, so this path can never over-dispose.
void bz_array_free_dynamic(void* arrp) {
    if (!arrp) return;
    bz_array_free(arrp, ((bz_array_t*)arrp)->elem_disposer);
}

void bz_array_free(void* arrp, void (*dispose_elem)(void*)) {
    if (!arrp) return;
    auto* arr = (bz_array_t*)arrp;
    if (dispose_elem && arr->data) {
        // Dispose elements in REVERSE insertion order per §28.11: ordered owning
        // collections (OwningArray, OwningStack, OwningQueue) dispose last-inserted
        // first. For pointer-sized elements, elem_size = sizeof(void*).
        // bz_array_free with non-null dispose is for pointer-element arrays only
        // (the codegen ensures this).
        for (int64_t i = arr->count - 1; i >= 0; --i) {
            void* elem_ptr = ((void**)arr->data)[i];
            if (elem_ptr) dispose_elem(elem_ptr);
        }
    }
    free(arr->data);
    free(arr);
}

// §28.2 v0.135 / issue #621: checked-disposal-mode variant of bz_array_free.
// Instead of freeing the header block, it:
//  1. Runs element disposal (same as bz_array_free).
//  2. Frees the DATA buffer (contents are gone, no double-free risk).
//  3. Sets disposed = 1 on the HEADER, then leaves the header alive
//     (quarantine) so the disposed flag remains readable for subsequent
//     indexer-guard loads. This is a deliberate memory leak in checked
//     (debug) builds — consistent with the class-instance quarantine used
//     by emitDisposeAndReclaim for §28.2 compliance.
void bz_array_mark_disposed(void* arrp, void (*dispose_elem)(void*)) {
    if (!arrp) return;
    auto* arr = (bz_array_t*)arrp;
    if (arr->disposed) return;  // idempotent
    if (dispose_elem && arr->data) {
        for (int64_t i = arr->count - 1; i >= 0; --i) {
            void* elem_ptr = ((void**)arr->data)[i];
            if (elem_ptr) dispose_elem(elem_ptr);
        }
    }
    free(arr->data);
    arr->data  = nullptr;
    arr->count = 0;
    arr->disposed = 1;
    // Do NOT free(arr) — header stays alive for the disposed-flag guard.
}

// §28.2 v0.135 / issue #621: return the disposed byte for a bz_array_t*.
// Called by checked-mode indexer guards to test use-after-dispose.
// Returns 0 (live) or 1 (disposed).  Null pointer treated as non-disposed
// (a null array hits the bounds-check guard separately).
int8_t bz_array_is_disposed(void* arrp) {
    if (!arrp) return 0;
    return ((bz_array_t*)arrp)->disposed;
}

void bz_array_sort(void* arrp, int64_t elem_size,
                   int (*cmp)(const void*, const void*)) {
    auto* arr = (bz_array_t*)arrp;
    if (arr && arr->count > 1) {
        qsort(arr->data, (size_t)arr->count, (size_t)elem_size, cmp);
        arr->mod_count++;  // §29.12 / #4631: structural mutation (Sort repositions)
    }
}

// stdlib §3 / #472: Sort with a closure-bearing comparator.
// `cmpfn(closure, a, b)` wraps the user lambda `(T, T) -> int`.
// We use a thread-local shim so qsort (which takes a plain function pointer)
// can reach the closure without a global lock.
//
// fix #4748 / stdlib §4.4 (v0.307): a structural mutation that reaches the
// same array from inside its own sort callback must raise InvalidOperationException
// (§4.4 loud-failure discipline, extended from enumerator invalidation).
// We use setjmp/longjmp to escape from the mutation in the comparator back to
// bz_array_sort_closure, where we throw after std::sort returns.
// fix #4748 / stdlib §4.4: signal a pending InvalidOperationException during a sort
// callback. After std::sort returns, bz_array_sort_closure checks the mutation flag
// and throws InvalidOperationException so the Bezel landing pad catches it.
extern "C" thread_local void* bz_exc;  // defined in bezel_runtime_itable.cpp
extern "C" void bz_ioe_vtable_init(void* vtable) { bz_ioe_vtable = vtable; }
// fix #4748 / stdlib §4.4: raise InvalidOperationException by setting bz_exc.
// Bezel's exception handling uses explicit control flow (setjmp/longjmp), NOT C++
// EH with landing pads. The generated code checks bz_exc after the call via
// emitPendingCheck() and branches to the catch handler. Returning normally lets
// the generated code's pending-check mechanism handle the control-flow transfer.
__attribute__((noinline)) static void bz_throw_invalid_operation(void) {
    if (!bz_ioe_vtable) return;  // defensive: not initialized (shouldn't happen)
    void* obj = calloc(1, (size_t)BZ_EXC_OBJ_SIZE);
    if (!obj) return;
    *(void**)obj = bz_ioe_vtable;
    bz_exc = obj;  // set so emitPendingCheck() in generated code branches to catch
}

void bz_array_sort_closure(void* arrp, int64_t elem_size,
                           void* closure,
                           int (*cmpfn)(void*, const void*, const void*)) {
    auto* arr = (bz_array_t*)arrp;
    if (!arr || arr->count < 2) return;
    bz_sort_in_progress_arr = arr;       // fix #4748: mark sort-in-progress
    bz_sort_mutation_attempted = false;  // clear any stale flag

    // fix #4748 / stdlib §4.4: use setjmp/longjmp to escape from the comparator.
    // longjmp from inside std::sort avoids C++ exception propagation issues.
    // After longjmp returns (non-zero), we throw InvalidOperationException.
    // Declare volatile variables BEFORE setjmp so their values survive longjmp.
    volatile int64_t volatile_count = arr->count;
    volatile void** volatile_ptrs = nullptr;  // heap-allocated, survives longjmp
    volatile bool did_longjmp = false;
    jmp_buf jbuf;
    bz_sort_jmp_buf = &jbuf;
    if (::setjmp(jbuf) == 0) {
        // Normal path: run std::sort
        int64_t count = volatile_count;
        void** ptrs = (void**)malloc((size_t)count * sizeof(void*));
        if (!ptrs) {
            bz_sort_in_progress_arr = nullptr;
            bz_sort_jmp_buf = nullptr;
            return;  // allocation failure: skip sort
        }
        volatile_ptrs = (volatile void**)ptrs;
        char* base = (char*)arr->data;
        for (int64_t i = 0; i < count; i++)
            ptrs[i] = base + i * elem_size;
        std::sort(ptrs, ptrs + count,
            [cmpfn, closure](void* a, void* b) -> bool {
                return cmpfn(closure, a, b) < 0;
            });
        // Permute elements in place to match sorted order.
        // ptrs[i] now points to the slot holding the element that belongs at position i.
        // Walk the permutation chain: at position i, if ptrs[i] != &base[i], swap the
        // bytes and follow where the displaced element came from until we return to i.
        // This is the standard in-place permutation algorithm (heap's algorithm variant).
        for (int64_t i = 0; i < count; i++) {
            char* target_slot = base + i * elem_size;  // slot that should hold element i
            char* elem_slot = (char*)ptrs[i];         // slot currently holding element i
            if (elem_slot == target_slot) continue;    // already in place
            // Swap the bytes at target_slot and elem_slot
            char tmp[256];
            if (elem_size > 256) {
                for (int64_t b = 0; b < elem_size; b++) {
                    char t = target_slot[b];
                    target_slot[b] = elem_slot[b];
                    elem_slot[b] = t;
                }
            } else {
                memcpy(tmp, target_slot, (size_t)elem_size);
                memcpy(target_slot, elem_slot, (size_t)elem_size);
                memcpy(elem_slot, tmp, (size_t)elem_size);
            }
            // Now elem_slot holds the bytes that were at target_slot.
            // Find which sorted position these bytes belong at: search ptrs for the slot
            // that pointed to target_slot before the swap.
            char* displaced = target_slot;  // the slot whose bytes we just moved to elem_slot
            for (int64_t j = i + 1; j < count; j++) {
                if ((char*)ptrs[j] == displaced) {
                    ptrs[j] = elem_slot;  // update ptrs so it stays consistent
                    break;
                }
            }
        }
        free(ptrs);
        bz_sort_in_progress_arr = nullptr;
        bz_sort_jmp_buf = nullptr;
        arr->mod_count++;  // §29.12 / #4631
    } else {
        // longjmp path: a mutation was attempted during the sort
        did_longjmp = true;
        bz_sort_in_progress_arr = nullptr;
        bz_sort_jmp_buf = nullptr;
        // volatile_ptrs still holds the malloc'd pointer if we got it before the jump
        if (volatile_ptrs) free((void*)volatile_ptrs);
    }
    if (did_longjmp) {
        // Throw InvalidOperationException so the Bezel landing pad catches it
        bz_throw_invalid_operation();
    }
}

// §29.12, v0.307: record an in-place repositioning that the CALLER performed.
//
// The in-place `Array<T>`/`OwningArray<T>` `Reverse()` (stdlib §4.4) is lowered
// as an inlined element-swap loop in codegen (`Emitter::tryLowerArrayCall`'s
// `Reverse` arm) rather than as a call into this file, so unlike every other
// structural mutator here it had no site at which to bump the modification
// epoch. §29.12 names it in the same breath as `Sort()` — "together with the
// in-place `Sort()`/`Reverse()` (they reposition)" — so a `for` over the array
// that reverses it in the loop body must raise `InvalidOperationException` at
// the next advance, exactly as `Sort()` already does via `bz_array_sort`.
//
// The `count > 1` guard mirrors `bz_array_sort`'s: an array of 0 or 1 elements
// repositions nothing, so no enumerator is invalidated.
void bz_array_note_repositioned(void* arrp) {
    auto* arr = (bz_array_t*)arrp;
    if (arr && arr->count > 1)
        arr->mod_count++;  // §29.12 / #4631: structural mutation (Reverse repositions)
}

// stdlib §3 / #472: Shallow-copy `arr` — allocate a new bz_array_t with the
// same element bytes.  The copy does NOT deep-clone pointer-typed elements;
// for the borrowing Array<T> this is correct (Array<T> does not own its class
// elements — ownership stays with the original owner).  The returned array is
// fresh-owned by the caller; dispose with bz_array_free(result, nullptr).
void* bz_array_copy(void* arrp, int64_t elem_size) {
    auto* src = (bz_array_t*)arrp;
    if (!src) return bz_array_alloc(elem_size, 0);
    auto* dst = (bz_array_t*)bz_array_alloc(elem_size, src->count);
    if (src->count > 0 && src->data) {
        memcpy(dst->data, src->data, (size_t)(src->count * elem_size));
        dst->count = src->count;
        if (dst->capacity < src->count) dst->capacity = src->count;
    }
    return dst;
}

// fix #682 / §15.6: copy elements [start, end) from `arr` into a fresh array.
// `end` is exclusive (half-open interval). Precondition: callers have already
// verified start >= 0, end <= Count, and start <= end via codegen bounds-checks
// (emitGuardThrow before this call).
void* bz_array_slice(void* arrp, int64_t start, int64_t end, int64_t elem_size) {
    auto* src = (bz_array_t*)arrp;
    int64_t count = end - start;
    if (count < 0) count = 0;
    auto* dst = (bz_array_t*)bz_array_alloc(elem_size, count > 0 ? count : 0);
    if (count > 0 && src && src->data) {
        memcpy(dst->data, (char*)src->data + start * elem_size,
               (size_t)(count * elem_size));
        dst->count = count;
    }
    return dst;
}

// #429 / §4.6 / §28.9: retain every non-null pointer-sized string element so a
// freshly copied / materialized Array<string> independently co-owns a +1 per
// element. Each slot is a bz_string*; bz_string_retain is a no-op on a
// sentinel/literal or null, so a partial/empty array is safe. This pairs with
// the per-element bz_string_release the array's later free/dispose issues — an
// insert-built array retains at Add (codegen), a copy/materialize-built array
// retains here, so both are symmetric and release exactly their +1 at free.
void bz_array_retain_strings(void* arrp) {
    if (!arrp) return;
    auto* arr = (bz_array_t*)arrp;
    if (!arr->data) return;
    for (int64_t i = 0; i < arr->count; ++i) {
        void* elem = ((void**)arr->data)[i];
        if (elem) bz_string_retain(elem);
    }
}

void* bz_array_ptr_at(void* arrp, int64_t idx, int64_t elem_size) {
    auto* arr = (bz_array_t*)arrp;
    return (char*)arr->data + idx * elem_size;
}

void bz_array_reserve(void* arrp, int64_t elem_size, int64_t cap) {
    auto* arr = (bz_array_t*)arrp;
    if (arr && cap > arr->capacity) bz_array_grow(arr, elem_size, cap);
}

// ---------------------------------------------------------------------------
// bz_dict_t — open-addressing hash table (string keys, void* values)
// ---------------------------------------------------------------------------

struct bz_entry_t {
    char*   key;        // malloc'd copy, null = empty slot
    int64_t key_len;    // string mode: CODE POINTS (§4.6); generic mode: key_size bytes
    void*   val;
    uint32_t hash;
    bool    deleted;
    // fix #4499 (spec §4.6 / §8.3, v0.306): the storage WIDTH (1/2/4 bytes per
    // code point) of `key` in string-key mode. The pre-fix table stored only
    // (bytes, code-point count) and used the code-point count as a BYTE count,
    // so a width>1 key was truncated to its first `len/width` code points and
    // distinct keys collided. Width is not recoverable from the bytes — the
    // width-2 key `"日"` and the width-1 key `"\u00E5e"` have the SAME two
    // bytes — so identity needs it. Zero in generic-key mode (unused there).
    int8_t  key_w;
};

struct bz_dict_t {
    bz_entry_t* buckets;
    int64_t     count;
    int64_t     capacity;  // always a power of 2
    // Generic-key mode (issue #343, §4.8 "K must provide consistent Equals and
    // GetHashCode"). When key_size > 0 the table holds raw key bytes and uses
    // the synthesized hash/eq function pointers instead of the string path.
    int64_t     key_size;  // 0 = string-key mode; >0 = generic key byte count
    uint64_t  (*hash_fn)(const void* key);
    bool      (*eq_fn)(const void* a, const void* b);
    // issue #622 / stdlib §4.8: insertion-order tracking for Dictionary<K,V>.
    // order[] holds bucket indices in insertion order; -1 entries are tombstones
    // (from removes). order_count is the used length (including tombstones);
    // order_cap is the allocated size. Iterated in 0..order_count order; -1
    // slots are skipped. Compacted on rehash (tombstones dropped).
    // OwningDictionary does NOT promise insertion order (§4.9) but shares this
    // struct; we still maintain order for it (cheap, harmless).
    int64_t*    order;       // malloc'd array of bucket indices
    int64_t     order_count; // used length (including -1 tombstones)
    int64_t     order_cap;   // allocated slots in order[]
    // §28.2 v0.135 / issue #627: checked-disposal-mode quarantine flag.
    // Set by bz_dict_mark_disposed (called instead of bz_dict_free in checked
    // builds); tested by bz_dict_is_disposed for method guards.
    int8_t      disposed;
    // §29.12 / issue #1232: modification epoch. Bumped on every mutation
    // (Add/Update/Remove/Clear). Captured by for-in codegen at loop start;
    // checked on each iteration — mismatch → InvalidOperationException.
    int64_t     mod_count;
    // fix #3026: kind tag (BZ_RUNTIME_KIND_DICT) set by bz_dict_alloc /
    // bz_dict_alloc_generic. APPENDED last so prior offsets (buckets@0,
    // count@8, capacity@16, key_size@24, hash_fn@32, eq_fn@40, order@48,
    // order_count@56, order_cap@64, disposed@72, mod_count@80) are
    // preserved — existing codegen reads them at fixed offsets.
    int64_t     kind;
};

// fix #4499 (spec §4.6 / §8.3, v0.306): read one code point out of a stored
// string key's raw buffer at code-point index `i`, honouring the key's storage
// width. Mirrors bezel_runtime_strings.cpp's `bz_str_cp`, which reads the same
// layout off a full bz_string; this one takes the (data, width) pair because a
// table entry is raw bytes, not a string object.
static inline uint32_t bz_key_cp(const char* d, int8_t w, int64_t i) {
    if (w == 2) return ((const uint16_t*)d)[i];
    if (w == 4) return ((const uint32_t*)d)[i];
    return ((const uint8_t*)d)[i];
}

// fix #4499: hash the key's CODE-POINT SEQUENCE, not its storage bytes.
// §8.3 requires `a.Equals(b)` ⇒ equal hashes, and §4.6 makes equality the
// code-point sequence with the storage width "invisible at the language
// level" — so the hash may not depend on the width either. Mixing each code
// point as four bytes makes the value width-independent by construction.
// (The pre-fix `fnv1a` mixed the first `len` BYTES — for a width>1 key that is
// the first `len/width` code points only, which is the collision this fixes.)
static uint32_t fnv1a_cp(const char* s, int64_t cp_len, int8_t w) {
    uint32_t h = 2166136261u;
    for (int64_t i = 0; i < cp_len; i++) {
        uint32_t c = bz_key_cp(s, w, i);
        h = (h ^ (uint8_t)( c        & 0xFFu)) * 16777619u;
        h = (h ^ (uint8_t)((c >>  8) & 0xFFu)) * 16777619u;
        h = (h ^ (uint8_t)((c >> 16) & 0xFFu)) * 16777619u;
        h = (h ^ (uint8_t)((c >> 24) & 0xFFu)) * 16777619u;
    }
    return h;
}

// fix #4499: string-key equality is §4.6's code-point-sequence equality. Equal
// widths take the memcmp fast path (same encoding, so byte equality IS code
// point equality); mixed widths decode both sides, so a width-2 and a width-1
// spelling of the same code points still compare equal — §4.6's width
// invisibility, and the same policy bz_str_compare_cp (#4487) applies to
// ordering.
static bool bz_key_eq(const char* a, int64_t alen, int8_t aw,
                      const char* b, int64_t blen, int8_t bw) {
    if (alen != blen) return false;
    if (!a || !b) return a == b;
    if (aw == bw) {
        size_t wb = (size_t)(aw > 0 ? aw : 1);
        return memcmp(a, b, (size_t)alen * wb) == 0;
    }
    for (int64_t i = 0; i < alen; i++)
        if (bz_key_cp(a, aw, i) != bz_key_cp(b, bw, i)) return false;
    return true;
}

// fix #4493 (stdlib §4.17 ordering is K's CompareTo; stdlib §9.6 ordinal,
// code-point-based; spec §4.6): ordinal code-point order over two key buffers,
// each read at its OWN storage width. Returns -1/0/+1 (sign is the contract,
// stdlib §2.8). Byte order and NUL-terminated strcmp are both wrong here: a
// width-2 buffer's low bytes do not order code points, and a zero high byte
// would end a strcmp early. Same relation as bz_str_compare_cp (#4487).
static int bz_key_cmp_cp(const char* a, int64_t alen, int8_t aw,
                         const char* b, int64_t blen, int8_t bw) {
    if (!a) alen = 0;
    if (!b) blen = 0;
    int64_t n = alen < blen ? alen : blen;
    for (int64_t i = 0; i < n; i++) {
        uint32_t ca = bz_key_cp(a, aw > 0 ? aw : 1, i);
        uint32_t cb = bz_key_cp(b, bw > 0 ? bw : 1, i);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (alen == blen) return 0;
    return alen < blen ? -1 : 1;
}

// fix #4499 (Defect B): copy a string key's raw bytes with malloc+memcpy.
// The pre-fix `strndup(k, k_len)` was wrong twice over: `k` is a bz_string
// data buffer, which is NOT a C string, and `k_len` is a code-point count, not
// a byte count. For a key holding a genuine U+0000 code point (a legal `char`,
// §4.5) strndup's internal strnlen stopped at the embedded NUL and returned a
// SHORTER allocation while the caller still recorded the full `key_len` —
// every later length-bounded read of that entry (dict_find's memcmp) then read
// past the allocation. Measured pre-fix: AddressSanitizer heap-buffer-overflow,
// "0 bytes after 2-byte region", on `d["a\u0000b"] := 1;` twice over.
// One extra width-sized zero slot terminates the copy the way bz_string's own
// buffers are terminated, keeping the ordering scans below (#4493) memory-safe
// (the code-point comparator reads by length, so the slot is defensive only).
static char* bz_key_dup(const char* k, int64_t cp_len, int8_t w) {
    size_t wb = (size_t)(w > 0 ? w : 1);
    size_t n  = (size_t)cp_len * wb;
    char* p = (char*)malloc(n + wb);
    if (!p) bz_alloc_fail("Dictionary<K,V> string key copy");
    if (n && k) memcpy(p, k, n);
    memset(p + n, 0, wb);
    return p;
}

// issue #622 / stdlib §4.8: append a bucket index to the insertion-order array.
static void dict_order_append(bz_dict_t* d, int64_t slot) {
    if (d->order_count == d->order_cap) {
        int64_t new_cap = d->order_cap < 8 ? 8 : d->order_cap * 2;
        // fix #1762 finding R5: same realloc anti-pattern as bz_array_grow, and
        // the same §31.4 posture — the write below is unconditional, so a
        // non-terminating failure would append past the un-grown buffer.
        int64_t* tmp = (int64_t*)realloc(d->order, (size_t)new_cap * sizeof(int64_t));
        if (!tmp) bz_alloc_fail("collection growth (Dictionary insertion-order array)");
        d->order = tmp;
        d->order_cap = new_cap;
    }
    d->order[d->order_count++] = slot;
}

// issue #622 / stdlib §4.8: mark a bucket index as removed in the order array
// (set its entry to -1). Linear scan — dictionaries are typically small.
static void dict_order_remove(bz_dict_t* d, int64_t slot) {
    for (int64_t i = 0; i < d->order_count; i++) {
        if (d->order[i] == slot) {
            d->order[i] = -1;
            return;
        }
    }
}

// fix #4499: `klen` is a CODE-POINT count and `kw` the operand's storage width
// (§4.6); the pre-fix signature carried only `klen` and used it as a byte count
// in the memcmp, which is the truncation/collision defect. Equality is
// delegated to bz_key_eq (code-point equality, width-independent).
static int64_t dict_find(bz_dict_t* d, const char* k, int64_t klen, int8_t kw,
                         uint32_t h) {
    int64_t mask = d->capacity - 1;
    int64_t idx = h & mask;
    for (int64_t probe = 0; probe < d->capacity; probe++) {
        int64_t i = (idx + probe) & mask;
        bz_entry_t& e = d->buckets[i];
        if (!e.key && !e.deleted) return -(i + 1); // empty: not found, insert here
        if (e.deleted) continue;
        if (e.hash == h && bz_key_eq(e.key, e.key_len, e.key_w, k, klen, kw))
            return i; // found
    }
    return -1; // table full (shouldn't happen with load factor management)
}

static void dict_rehash(bz_dict_t* d) {
    int64_t old_cap = d->capacity;
    bz_entry_t* old = d->buckets;
    int64_t new_cap = old_cap < 8 ? 8 : old_cap * 2;
    // §31.4: a failed rehash allocation terminates. Checking here is what makes
    // #2239's ctor hardening actually reachable — without it, `capacity` is
    // advanced past a NULL `buckets` and the very next insert writes through
    // NULL, which is the crash #2239 set out to remove.
    d->buckets = (bz_entry_t*)calloc((size_t)new_cap, sizeof(bz_entry_t));
    if (!d->buckets) bz_alloc_fail("collection growth (Dictionary bucket array)");
    d->capacity = new_cap;
    d->count = 0;

    // issue #622 / stdlib §4.8: build a translation table old_slot → new_slot
    // so we can compact and remap the order array after re-inserting all entries.
    int64_t* slot_map = (int64_t*)malloc((size_t)old_cap * sizeof(int64_t));
    if (!slot_map) bz_alloc_fail("collection growth (Dictionary rehash slot map)");
    for (int64_t i = 0; i < old_cap; i++) slot_map[i] = -1;

    for (int64_t i = 0; i < old_cap; i++) {
        bz_entry_t& e = old[i];
        if (!e.key || e.deleted) continue;
        // Re-insert into new buckets, record new slot index.
        int64_t pos = e.hash & (new_cap - 1);
        for (int64_t p = 0; p < new_cap; p++) {
            int64_t j = (pos + p) & (new_cap - 1);
            if (!d->buckets[j].key) {
                d->buckets[j] = e;
                d->count++;
                slot_map[i] = j;
                break;
            }
        }
    }
    free(old);

    // Compact the order array: skip tombstones (-1 entries), translate surviving
    // old bucket indices to their new positions via slot_map.
    int64_t new_oc = 0;
    for (int64_t i = 0; i < d->order_count; i++) {
        int64_t old_slot = d->order[i];
        if (old_slot < 0) continue;                  // tombstone: skip
        int64_t new_slot = (old_slot < old_cap) ? slot_map[old_slot] : -1;
        if (new_slot >= 0)
            d->order[new_oc++] = new_slot;
    }
    d->order_count = new_oc;
    free(slot_map);
}

// fix #2239 / stdlib §4.8 / spec §31.4: OOM-harden the string-key Dictionary
// ctor. Pre-fix called calloc(1, sizeof(bz_dict_t)) and calloc(8,
// sizeof(bz_entry_t)) without null-checking; a failed buckets allocation left
// d->buckets == nullptr and the first bz_dict_set_str dereferenced
// d->buckets[ins].key = ... to SIGSEGV.
//
// Returning NULL (the first cut of this fix) did not remove that crash — it
// moved it one line, to bz_dict_set_str's unchecked `d->count` read, because
// neither the codegen ctor site nor any bz_dict_* entry point checks for a
// null handle. §31.4 requires termination anyway, so terminate at the
// allocation itself, where the report can name what failed.
void* bz_dict_alloc(void) {
    auto* d = (bz_dict_t*)calloc(1, sizeof(bz_dict_t));
    if (!d) bz_alloc_fail("Dictionary<K,V> header");
    d->kind = BZ_RUNTIME_KIND_DICT;  // fix #3026
    d->capacity = 8;
    d->buckets = (bz_entry_t*)calloc(8, sizeof(bz_entry_t));
    if (!d->buckets) bz_alloc_fail("Dictionary<K,V> bucket array");
    // issue #622: order array starts null; lazily grown on first insert.
    // order_count and order_cap are zero from calloc.
    return d;
}

void bz_dict_free(void* dp, void (*dispose_val)(void*)) {
    if (!dp) return;
    auto* d = (bz_dict_t*)dp;
    // §28.2 v0.135 / issue #627: in checked-disposal builds, bz_dict_mark_disposed
    // quarantines the header (disposed=1, buckets freed) without freeing the block.
    // A subsequent scope-end bz_dict_free must be a no-op — skip if already disposed.
    if (d->disposed) return;
    for (int64_t i = 0; i < d->capacity; i++) {
        bz_entry_t& e = d->buckets[i];
        if (e.key && !e.deleted) {
            free(e.key);
            if (dispose_val && e.val) dispose_val(e.val);
        }
    }
    free(d->buckets);
    free(d->order);  // issue #622: release the insertion-order tracking array
    free(d);
}

// §28.2 v0.135 / issue #627: checked-disposal-mode quarantine for bz_dict_t.
// Mirrors bz_array_mark_disposed: disposes values, frees the bucket/order
// arrays, marks disposed=1 on the header, but does NOT free the header block —
// keeping it alive so subsequent method-entry guards can read the disposed flag.
// Idempotent (safe to call twice). Deliberate memory leak in checked (debug)
// builds, consistent with the class-instance and array quarantine patterns.
void bz_dict_mark_disposed(void* dp, void (*dispose_val)(void*)) {
    if (!dp) return;
    auto* d = (bz_dict_t*)dp;
    if (d->disposed) return;  // idempotent
    for (int64_t i = 0; i < d->capacity; i++) {
        bz_entry_t& e = d->buckets[i];
        if (e.key && !e.deleted) {
            free(e.key);
            if (dispose_val && e.val) dispose_val(e.val);
        }
    }
    free(d->buckets);
    d->buckets = nullptr;
    d->count = 0;
    free(d->order);
    d->order = nullptr;
    d->order_count = 0;
    d->disposed = 1;
    // Do NOT free(d) — header stays alive for the disposed-flag guard.
}

// §28.2 v0.135 / issue #627: return non-zero if the dict has been disposed
// via bz_dict_mark_disposed (checked mode only).
int8_t bz_dict_is_disposed(void* dp) {
    if (!dp) return 0;
    return ((bz_dict_t*)dp)->disposed;
}

bool bz_dict_try_get_str(void* dp, const char* k, int64_t k_len, int8_t k_w,
                         void** out) {
    auto* d = (bz_dict_t*)dp;
    if (!d || !k) return false;
    uint32_t h = fnv1a_cp(k, k_len, k_w);
    int64_t idx = dict_find(d, k, k_len, k_w, h);
    if (idx < 0) return false;
    if (out) *out = d->buckets[idx].val;
    return true;
}

void bz_dict_set_str(void* dp, const char* k, int64_t k_len, int8_t k_w,
                     void* val) {
    auto* d = (bz_dict_t*)dp;
    // Rehash at 75% load
    if (d->count * 4 >= d->capacity * 3) dict_rehash(d);
    uint32_t h = fnv1a_cp(k, k_len, k_w);
    int64_t idx = dict_find(d, k, k_len, k_w, h);
    if (idx >= 0) {
        // Key already present: update value, no order change (§4.8).
        // fix #4606 / §29.12, v0.307: an element-level write to a present key
        // is NOT structural ("dict[existingKey] := v updating a present key
        // ... leaves the enumerator valid") — do not bump mod_count here.
        d->buckets[idx].val = val;
        return;
    }
    // Fresh insertion: record slot in insertion order (§4.8 / issue #622).
    int64_t ins = -(idx + 1);
    d->buckets[ins].key = bz_key_dup(k, k_len, k_w);   // fix #4499 (was strndup)
    d->buckets[ins].key_len = k_len;
    d->buckets[ins].key_w = k_w;                       // fix #4499
    d->buckets[ins].val = val;
    d->buckets[ins].hash = h;
    d->buckets[ins].deleted = false;
    dict_order_append(d, ins);
    d->count++;
    d->mod_count++;  // §29.12 / #1232: structural mutation
}

// issue #388 / stdlib §4.9: OwningDictionary<string,V>'s `set(owned)` indexer.
// If `k` is already present, dispose the value stored under it before replacing
// it (exactly-once disposal of the displaced owner); a fresh key just inserts.
void bz_dict_set_str_disposing(void* dp, const char* k, int64_t k_len,
                               int8_t k_w, void* val,
                               void (*dispose_val)(void*)) {
    auto* d = (bz_dict_t*)dp;
    if (!d) return;
    uint32_t h = fnv1a_cp(k, k_len, k_w);
    int64_t idx = dict_find(d, k, k_len, k_w, h);
    if (idx >= 0) {
        // fix #4606 / §29.12, v0.307: value update of a present key is not
        // structural — no mod_count bump (see bz_dict_set_str above).
        if (dispose_val && d->buckets[idx].val) dispose_val(d->buckets[idx].val);
        d->buckets[idx].val = val;
        return;
    }
    bz_dict_set_str(dp, k, k_len, k_w, val);  // fresh key: bz_dict_set_str bumps mod_count
}

bool bz_dict_remove_str(void* dp, const char* k, int64_t k_len, int8_t k_w) {
    auto* d = (bz_dict_t*)dp;
    uint32_t h = fnv1a_cp(k, k_len, k_w);
    int64_t idx = dict_find(d, k, k_len, k_w, h);
    if (idx < 0) return false;
    dict_order_remove(d, idx);  // issue #622: untrack this slot from insertion order
    free(d->buckets[idx].key);
    d->buckets[idx].key = nullptr;
    d->buckets[idx].deleted = true;
    d->count--;
    d->mod_count++;  // §29.12 / #1232: structural mutation
    return true;
}

// issue M1 / stdlib §4.9: OwningDictionary<string,V>.Remove(key) must dispose
// the removed owned value (exactly-once disposal), then unlink the entry.
// Mirrors bz_dict_clear's per-entry dispose pattern. The non-owning
// Dictionary.Remove still calls bz_dict_remove_str (no disposer).
bool bz_dict_remove_str_disposing(void* dp, const char* k, int64_t k_len,
                                  int8_t k_w, void (*dispose_val)(void*)) {
    auto* d = (bz_dict_t*)dp;
    uint32_t h = fnv1a_cp(k, k_len, k_w);
    int64_t idx = dict_find(d, k, k_len, k_w, h);
    if (idx < 0) return false;
    dict_order_remove(d, idx);  // issue #622: untrack this slot from insertion order
    if (dispose_val && d->buckets[idx].val) dispose_val(d->buckets[idx].val);
    free(d->buckets[idx].key);
    d->buckets[idx].key = nullptr;
    d->buckets[idx].val = nullptr;
    d->buckets[idx].deleted = true;
    d->count--;
    d->mod_count++;  // §29.12 / #1232: structural mutation
    return true;
}

bool bz_dict_contains_str(void* dp, const char* k, int64_t k_len, int8_t k_w) {
    return bz_dict_try_get_str(dp, k, k_len, k_w, nullptr);
}

int64_t bz_dict_count(void* dp) {
    return dp ? ((bz_dict_t*)dp)->count : 0;
}

// fix #4499: the snapshot hands back THREE parallel arrays — the raw key
// bytes, the code-point count, and the storage WIDTH — because a key's byte
// size is `len * width` and the width is not recoverable from the bytes
// (§4.6). Pre-fix the caller rebuilt every key as a width-1 string of `len`
// bytes, which is the same code-point-count-as-byte-count confusion the
// storage layer had; now `decodeDictStringKeyAt` reads the width back.
void bz_dict_keys_str(void* dp, char*** keys_out, int64_t** lens_out,
                      int8_t** widths_out, int64_t* n_out) {
    auto* d = (bz_dict_t*)dp;
    int64_t n = d ? d->count : 0;
    auto** keys = (char**)malloc((size_t)(n > 0 ? n : 1) * sizeof(char*));
    auto*  lens = (int64_t*)malloc((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
    auto*  wids = (int8_t*)malloc((size_t)(n > 0 ? n : 1) * sizeof(int8_t));
    if (!keys || !lens || !wids) bz_alloc_fail("Dictionary<K,V> key snapshot");
    int64_t j = 0;
    if (d) {
        // issue #622 / stdlib §4.8: iterate in insertion order via order[].
        for (int64_t oi = 0; oi < d->order_count && j < n; oi++) {
            int64_t slot = d->order[oi];
            if (slot < 0) continue;  // tombstone from a remove
            bz_entry_t& e = d->buckets[slot];
            if (e.key && !e.deleted) {
                keys[j] = bz_key_dup(e.key, e.key_len, e.key_w);  // fix #4499
                lens[j] = e.key_len;
                wids[j] = e.key_w ? e.key_w : 1;
                j++;
            }
        }
    }
    if (keys_out)   *keys_out   = keys; else free(keys);
    if (lens_out)   *lens_out   = lens; else free(lens);
    if (widths_out) *widths_out = wids; else free(wids);
    if (n_out)      *n_out      = j;
}

// Generic-key variants (issue #343, §4.8). Keys are raw byte blobs of
// `key_size` bytes; hashing and equality are delegated to the synthesized
// GetHashCode / Equals of the key type (§8.3 / §9.1 / §9.2) via the function
// pointers the codegen passes at allocation time. This makes records (and any
// value type with the equality contract) usable as Dictionary/Set keys.
//
// Storage reuses bz_entry_t: `key` holds a malloc'd copy of the key bytes and
// `key_len` holds key_size, so bz_dict_free / dict_rehash work unchanged.

// Find a generic key in the table. Returns the bucket index, or -(idx+1) for
// the empty insertion slot, or -1 if full. Mirrors dict_find but compares with
// the user-provided equality function (and matches the cached hash first).
static int64_t dict_find_gen(bz_dict_t* d, const void* k, uint32_t h) {
    int64_t mask = d->capacity - 1;
    int64_t idx = h & mask;
    for (int64_t probe = 0; probe < d->capacity; probe++) {
        int64_t i = (idx + probe) & mask;
        bz_entry_t& e = d->buckets[i];
        if (!e.key && !e.deleted) return -(i + 1); // empty: not found, insert here
        if (e.deleted) continue;
        if (e.hash == h && d->eq_fn(e.key, k))
            return i; // found
    }
    return -1; // table full (shouldn't happen with load factor management)
}

// fix #2239 / stdlib §4.8 / spec §31.4: OOM-harden the generic-key Dictionary
// ctor. Mirrors bz_dict_alloc above — terminate at the failed allocation rather
// than handing back a handle no caller null-checks.
void* bz_dict_alloc_generic(int64_t key_size,
                              uint64_t (*hash)(const void* key),
                              bool     (*eq)(const void* a, const void* b)) {
    auto* d = (bz_dict_t*)calloc(1, sizeof(bz_dict_t));
    if (!d) bz_alloc_fail("Dictionary<K,V> header");
    d->kind = BZ_RUNTIME_KIND_DICT;  // fix #3026
    d->capacity = 8;
    d->buckets = (bz_entry_t*)calloc(8, sizeof(bz_entry_t));
    if (!d->buckets) bz_alloc_fail("Dictionary<K,V> bucket array");
    d->key_size = key_size > 0 ? key_size : 1;
    d->hash_fn = hash;
    d->eq_fn = eq;
    // issue #622: order array starts null; lazily grown on first insert.
    // order_count and order_cap are zero from calloc.
    return d;
}

bool bz_dict_try_get_gen(void* dp, const void* key, void** out) {
    auto* d = (bz_dict_t*)dp;
    if (!d || !key || d->key_size <= 0) return false;
    uint32_t h = (uint32_t)d->hash_fn(key);
    int64_t idx = dict_find_gen(d, key, h);
    if (idx < 0) return false;
    if (out) *out = d->buckets[idx].val;
    return true;
}

void bz_dict_set_gen(void* dp, const void* key, void* val) {
    auto* d = (bz_dict_t*)dp;
    if (!d || !key || d->key_size <= 0) return;
    // Rehash at 75% load (same policy as the string variant).
    if (d->count * 4 >= d->capacity * 3) dict_rehash(d);
    uint32_t h = (uint32_t)d->hash_fn(key);
    int64_t idx = dict_find_gen(d, key, h);
    if (idx >= 0) {
        // Key already present: update value, no order change (§4.8).
        // fix #4606 / §29.12, v0.307: element-level write to a present key is
        // not structural — do not bump mod_count here.
        d->buckets[idx].val = val;
        return;
    }
    // Fresh insertion: record slot in insertion order (§4.8 / issue #622).
    int64_t ins = -(idx + 1);
    char* kc = (char*)malloc((size_t)d->key_size);
    memcpy(kc, key, (size_t)d->key_size);
    d->buckets[ins].key = kc;
    d->buckets[ins].key_len = d->key_size;
    d->buckets[ins].val = val;
    d->buckets[ins].hash = h;
    d->buckets[ins].deleted = false;
    dict_order_append(d, ins);
    d->count++;
    d->mod_count++;  // §29.12 / #1232: structural mutation
}

// issue #388 / stdlib §4.9: generic-key counterpart to
// bz_dict_set_str_disposing — dispose the value under `key` (if present) before
// replacing it; a fresh key just inserts.
void bz_dict_set_gen_disposing(void* dp, const void* key, void* val,
                               void (*dispose_val)(void*)) {
    auto* d = (bz_dict_t*)dp;
    if (!d || !key || d->key_size <= 0) return;
    uint32_t h = (uint32_t)d->hash_fn(key);
    int64_t idx = dict_find_gen(d, key, h);
    if (idx >= 0) {
        // fix #4606 / §29.12, v0.307: value update of a present key is not
        // structural — no mod_count bump (see bz_dict_set_gen above).
        if (dispose_val && d->buckets[idx].val) dispose_val(d->buckets[idx].val);
        d->buckets[idx].val = val;
        return;
    }
    bz_dict_set_gen(dp, key, val);        // fresh key: bz_dict_set_gen bumps mod_count
}

bool bz_dict_remove_gen(void* dp, const void* key) {
    auto* d = (bz_dict_t*)dp;
    if (!d || !key || d->key_size <= 0) return false;
    uint32_t h = (uint32_t)d->hash_fn(key);
    int64_t idx = dict_find_gen(d, key, h);
    if (idx < 0) return false;
    dict_order_remove(d, idx);  // issue #622: untrack this slot from insertion order
    free(d->buckets[idx].key);
    d->buckets[idx].key = nullptr;
    d->buckets[idx].deleted = true;
    d->count--;
    d->mod_count++;  // §29.12 / #1232: structural mutation
    return true;
}

// issue M1 / stdlib §4.9: OwningDictionary<K,V>.Remove(key) for generic keys —
// dispose the removed owned value (exactly-once), then unlink. Mirrors the
// string-key bz_dict_remove_str_disposing; the non-owning Dict uses
// bz_dict_remove_gen (no disposer).
bool bz_dict_remove_gen_disposing(void* dp, const void* key,
                                  void (*dispose_val)(void*)) {
    auto* d = (bz_dict_t*)dp;
    if (!d || !key || d->key_size <= 0) return false;
    uint32_t h = (uint32_t)d->hash_fn(key);
    int64_t idx = dict_find_gen(d, key, h);
    if (idx < 0) return false;
    dict_order_remove(d, idx);  // issue #622: untrack this slot from insertion order
    if (dispose_val && d->buckets[idx].val) dispose_val(d->buckets[idx].val);
    free(d->buckets[idx].key);
    d->buckets[idx].key = nullptr;
    d->buckets[idx].val = nullptr;
    d->buckets[idx].deleted = true;
    d->count--;
    d->mod_count++;  // §29.12 / #1232: structural mutation
    return true;
}

bool bz_dict_contains_gen(void* dp, const void* key) {
    return bz_dict_try_get_gen(dp, key, nullptr);
}

// fix #643 / stdlib §4.9: PopValue(key) — retrieve the stored value pointer,
// unlink the entry WITHOUT disposing the value (ownership transfers to caller),
// and return true.  Returns false when the key is absent (callee raises).
// String-key variant.
bool bz_dict_pop_value_str(void* dp, const char* k, int64_t k_len, int8_t k_w,
                            void** out) {
    auto* d = (bz_dict_t*)dp;
    if (!d || !k) return false;
    uint32_t h = fnv1a_cp(k, k_len, k_w);
    int64_t idx = dict_find(d, k, k_len, k_w, h);
    if (idx < 0) return false;
    if (out) *out = d->buckets[idx].val;
    dict_order_remove(d, idx);
    free(d->buckets[idx].key);
    d->buckets[idx].key = nullptr;
    d->buckets[idx].val = nullptr;
    d->buckets[idx].deleted = true;
    d->count--;
    d->mod_count++;  // §29.12 / #1232: structural mutation
    return true;
}

// Generic-key variant.
bool bz_dict_pop_value_gen(void* dp, const void* key, void** out) {
    auto* d = (bz_dict_t*)dp;
    if (!d || !key || d->key_size <= 0) return false;
    uint32_t h = (uint32_t)d->hash_fn(key);
    int64_t idx = dict_find_gen(d, key, h);
    if (idx < 0) return false;
    if (out) *out = d->buckets[idx].val;
    dict_order_remove(d, idx);
    free(d->buckets[idx].key);
    d->buckets[idx].key = nullptr;
    d->buckets[idx].val = nullptr;
    d->buckets[idx].deleted = true;
    d->count--;
    d->mod_count++;  // §29.12 / #1232: structural mutation
    return true;
}

// stdlib §4.10 / §4.8: empty a hash table in place, keeping it allocated for
// reuse. Frees each live key's malloc'd copy and, if `dispose_val` is non-null,
// disposes each live value; then zeroes every bucket and resets the count. Used
// by Set<T>.Clear() / Dictionary<K,V>.Clear() over the generic table. The
// capacity, key_size, and hash/eq function pointers are preserved. For a
// dict-backed Set the value slots are non-owned sentinels, so callers pass null
// for `dispose_val`.
void bz_dict_clear(void* dp, void (*dispose_val)(void*)) {
    if (!dp) return;
    auto* d = (bz_dict_t*)dp;
    for (int64_t i = 0; i < d->capacity; i++) {
        bz_entry_t& e = d->buckets[i];
        if (e.key && !e.deleted) {
            free(e.key);
            if (dispose_val && e.val) dispose_val(e.val);
        }
    }
    memset(d->buckets, 0, (size_t)d->capacity * sizeof(bz_entry_t));
    d->count = 0;
    // issue #622 / stdlib §4.8: Clear() resets insertion order (§4.8: "Clear()
    // resets the order"). Reuse the allocated order buffer but set count to 0.
    d->order_count = 0;
    d->mod_count++;  // §29.12 / #1232: structural mutation
}

// Enumerate the generic table's keys (issue #370, §4.8 Dictionary.Keys /
// §4.10 Set iteration). Returns a malloc'd array of `n` pointers, each a
// malloc'd copy of one live key's `key_size` bytes. Mirrors bz_dict_keys_str
// (which strdup's string keys) but copies the raw key blob instead, so it
// serves int/enum/string/record keys uniformly.
// issue #622 / stdlib §4.8: iteration is now in insertion order via order[].
void bz_dict_keys_gen(void* dp, void*** keys_out, int64_t* n_out) {
    auto* d = (bz_dict_t*)dp;
    int64_t n = d ? d->count : 0;
    auto** keys = (void**)malloc((size_t)(n > 0 ? n : 1) * sizeof(void*));
    int64_t j = 0;
    if (d && d->key_size > 0) {
        // issue #622 / stdlib §4.8: iterate in insertion order via order[].
        for (int64_t oi = 0; oi < d->order_count && j < n; oi++) {
            int64_t slot = d->order[oi];
            if (slot < 0) continue;  // tombstone from a remove
            bz_entry_t& e = d->buckets[slot];
            if (e.key && !e.deleted) {
                void* kc = malloc((size_t)d->key_size);
                memcpy(kc, e.key, (size_t)d->key_size);
                keys[j++] = kc;
            }
        }
    }
    if (keys_out) *keys_out = keys; else free(keys);
    if (n_out)    *n_out    = j;
}

// issue #537 / stdlib §4.8: snapshot value arrays for `for var v in d.Values`.
// Both functions return a malloc'd void** array of `n` slot values (one per live
// entry).  The caller owns and must free() the array; the values themselves are
// owned by the dictionary and must NOT be freed by the caller.
//
// bz_dict_values_str — string-keyed table (key_size == 0).
// issue #622 / stdlib §4.8: values are returned in insertion order via order[].
void bz_dict_values_str(void* dp, void*** vals_out, int64_t* n_out) {
    auto* d = (bz_dict_t*)dp;
    int64_t n = d ? d->count : 0;
    auto** vals = (void**)malloc((size_t)(n > 0 ? n : 1) * sizeof(void*));
    int64_t j = 0;
    if (d) {
        // issue #622 / stdlib §4.8: iterate in insertion order via order[].
        for (int64_t oi = 0; oi < d->order_count && j < n; oi++) {
            int64_t slot = d->order[oi];
            if (slot < 0) continue;  // tombstone from a remove
            bz_entry_t& e = d->buckets[slot];
            if (e.key && !e.deleted)
                vals[j++] = e.val;
        }
    }
    if (vals_out) *vals_out = vals; else free(vals);
    if (n_out)    *n_out    = j;
}

// bz_dict_values_gen — generic-key table (key_size > 0). Mirrors
// bz_dict_values_str; the key_size check guards against null/uninitialized dicts.
// issue #622 / stdlib §4.8: values are returned in insertion order via order[].
void bz_dict_values_gen(void* dp, void*** vals_out, int64_t* n_out) {
    auto* d = (bz_dict_t*)dp;
    int64_t n = d ? d->count : 0;
    auto** vals = (void**)malloc((size_t)(n > 0 ? n : 1) * sizeof(void*));
    int64_t j = 0;
    if (d && d->key_size > 0) {
        // issue #622 / stdlib §4.8: iterate in insertion order via order[].
        for (int64_t oi = 0; oi < d->order_count && j < n; oi++) {
            int64_t slot = d->order[oi];
            if (slot < 0) continue;  // tombstone from a remove
            bz_entry_t& e = d->buckets[slot];
            if (e.key && !e.deleted)
                vals[j++] = e.val;
        }
    }
    if (vals_out) *vals_out = vals; else free(vals);
    if (n_out)    *n_out    = j;
}

// fix #1426 / stdlib §4.17–§4.18: SortedDictionary<K,V> and SortedSet<T>
// ordered-key accessors and sorted-snapshot helpers.
//
// bz_dict_min_key_gen / bz_dict_max_key_gen — O(n) scan of a generic-key dict
// (key_size > 0) for the least or greatest key.  Returns a malloc'd copy of the
// winning key's bytes (key_size bytes), or NULL if the dict is empty or null.
// Caller must free the returned pointer.
// cmp(a, b): a, b are pointers to key_size bytes (the raw key bytes directly —
// NOT void** pointers to pointers) — matching buildSortedKeyComparator() in codegen.
// Empty-dict check is performed by codegen before calling (InvalidOperationException
// path), but these guard NULL/empty defensively anyway.

void* bz_dict_min_key_gen(void* dp,
                           int (*cmp)(const void* a, const void* b)) {
    auto* d = (bz_dict_t*)dp;
    if (!d || d->count == 0 || d->key_size <= 0) return nullptr;
    const void* best = nullptr;
    for (int64_t oi = 0; oi < d->order_count; oi++) {
        int64_t slot = d->order[oi];
        if (slot < 0) continue;
        bz_entry_t& e = d->buckets[slot];
        if (!e.key || e.deleted) continue;
        if (!best || cmp(e.key, best) < 0) best = e.key;
    }
    if (!best) return nullptr;
    void* out = malloc((size_t)d->key_size);
    memcpy(out, best, (size_t)d->key_size);
    return out;
}

void* bz_dict_max_key_gen(void* dp,
                           int (*cmp)(const void* a, const void* b)) {
    auto* d = (bz_dict_t*)dp;
    if (!d || d->count == 0 || d->key_size <= 0) return nullptr;
    const void* best = nullptr;
    for (int64_t oi = 0; oi < d->order_count; oi++) {
        int64_t slot = d->order[oi];
        if (slot < 0) continue;
        bz_entry_t& e = d->buckets[slot];
        if (!e.key || e.deleted) continue;
        if (!best || cmp(e.key, best) > 0) best = e.key;
    }
    if (!best) return nullptr;
    void* out = malloc((size_t)d->key_size);
    memcpy(out, best, (size_t)d->key_size);
    return out;
}

// bz_dict_min_key_str / bz_dict_max_key_str — O(n) scan of a string-key dict
// (key_size == 0) for the least or greatest key.
// Fills *out_data (a malloc'd copy of len*width bytes), *out_len (code points)
// and *out_w (storage width, fix #4499).  Returns false if empty.
// Caller must free(*out_data) after use.
// Ordinal code-point comparison per §9.6 / §4.17 (fix #4493: was strcmp).

bool bz_dict_min_key_str(void* dp, char** out_data, int64_t* out_len,
                         int8_t* out_w) {
    auto* d = (bz_dict_t*)dp;
    if (!d || d->count == 0) return false;
    const char* best = nullptr;
    int64_t best_len = 0;
    int8_t  best_w = 1;
    for (int64_t oi = 0; oi < d->order_count; oi++) {
        int64_t slot = d->order[oi];
        if (slot < 0) continue;
        bz_entry_t& e = d->buckets[slot];
        if (!e.key || e.deleted) continue;
        if (!best || bz_key_cmp_cp(e.key, e.key_len, e.key_w, best, best_len, best_w) < 0) {
            best = e.key; best_len = e.key_len; best_w = e.key_w ? e.key_w : 1;
        }
    }
    if (!best) return false;
    if (out_data) *out_data = bz_key_dup(best, best_len, best_w);  // fix #4499
    if (out_len)  *out_len  = best_len;
    if (out_w)    *out_w    = best_w;
    return true;
}

bool bz_dict_max_key_str(void* dp, char** out_data, int64_t* out_len,
                         int8_t* out_w) {
    auto* d = (bz_dict_t*)dp;
    if (!d || d->count == 0) return false;
    const char* best = nullptr;
    int64_t best_len = 0;
    int8_t  best_w = 1;
    for (int64_t oi = 0; oi < d->order_count; oi++) {
        int64_t slot = d->order[oi];
        if (slot < 0) continue;
        bz_entry_t& e = d->buckets[slot];
        if (!e.key || e.deleted) continue;
        if (!best || bz_key_cmp_cp(e.key, e.key_len, e.key_w, best, best_len, best_w) > 0) {
            best = e.key; best_len = e.key_len; best_w = e.key_w ? e.key_w : 1;
        }
    }
    if (!best) return false;
    if (out_data) *out_data = bz_key_dup(best, best_len, best_w);  // fix #4499
    if (out_len)  *out_len  = best_len;
    if (out_w)    *out_w    = best_w;
    return true;
}

// Sorted-snapshot helpers for SortedDictionary/SortedSet ordered iteration.
// O(n log n) per enumeration (hash-backing limitation).
// A real ordered rep (tree/skiplist) rides #545 (self-hosted collections).
// Each helper takes snapshot arrays produced by bz_dict_keys_gen /
// bz_dict_values_gen and reorders them IN PLACE.

struct BzKVSortCtxGen {
    void** keys;
    int (*cmp)(const void* a, const void* b);
};
static int bz_kv_idx_cmp_gen(const void* ia, const void* ib, void* c) {
    auto* ctx = (BzKVSortCtxGen*)c;
    int64_t i = *(const int64_t*)ia, j = *(const int64_t*)ib;
    // keys[i]/keys[j] are void* pointing to key bytes; pass them directly.
    return ctx->cmp(ctx->keys[i], ctx->keys[j]);
}
struct BzKVSortCtxStr { char** keys; int64_t* lens; int8_t* widths; };
// fix #4493 (stdlib §4.17 via §9.6): order string keys by code point, reading
// each key at its own storage width. The pre-fix byte-wise strcmp ordered by
// storage bytes and stopped at the first zero byte of a width>1 key.
// lens/widths are the snapshot's parallel spines; a null spine falls back to
// the width-1 reading (strlen for the length).
static int bz_kv_idx_cmp_str(const void* ia, const void* ib, void* c) {
    auto* ctx = (BzKVSortCtxStr*)c;
    int64_t i = *(const int64_t*)ia, j = *(const int64_t*)ib;
    const char* a = ctx->keys[i];
    const char* b = ctx->keys[j];
    int64_t alen = ctx->lens ? ctx->lens[i] : (a ? (int64_t)strlen(a) : 0);
    int64_t blen = ctx->lens ? ctx->lens[j] : (b ? (int64_t)strlen(b) : 0);
    int8_t  aw = ctx->widths ? ctx->widths[i] : 1;
    int8_t  bw = ctx->widths ? ctx->widths[j] : 1;
    return bz_key_cmp_cp(a, alen, aw, b, blen, bw);
}

// bz_sort_kv_gen: reorder keys[] and values[] together in ascending key order.
// keys[i] = void* (pointer to raw key bytes); values[i] = void* (stored value).
// cmp(a, b): a, b = direct pointers to key bytes (NOT void** of void*).
void bz_sort_kv_gen(void** keys, void** values, int64_t n,
                    int (*cmp)(const void* a, const void* b)) {
    if (n <= 1 || !keys || !cmp) return;
    int64_t* idx = (int64_t*)malloc((size_t)n * sizeof(int64_t));
    if (!idx) return;
    for (int64_t i = 0; i < n; i++) idx[i] = i;
    BzKVSortCtxGen ctx = {keys, cmp};
    qsort_r(idx, (size_t)n, sizeof(int64_t), bz_kv_idx_cmp_gen, &ctx);
    void** ks = (void**)malloc((size_t)n * sizeof(void*));
    void** vs = values ? (void**)malloc((size_t)n * sizeof(void*)) : nullptr;
    if (!ks) { free(idx); return; }
    for (int64_t i = 0; i < n; i++) {
        ks[i] = keys[idx[i]];
        if (vs) vs[i] = values[idx[i]];
    }
    memcpy(keys, ks, (size_t)n * sizeof(void*));
    if (vs) { memcpy(values, vs, (size_t)n * sizeof(void*)); free(vs); }
    free(ks); free(idx);
}

// bz_sort_kv_str: reorder char** keys + int64_t* lens + void** values together
// in ascending lexicographic string order (§9.6).
void bz_sort_kv_str(char** keys, int64_t* lens, int8_t* widths, void** values,
                    int64_t n) {
    if (n <= 1 || !keys) return;
    // fix #1762 finding R1 + spec §31.4: every scratch allocation here
    // terminates on failure. Returning early (the pre-fix and first-fix
    // behaviour) leaves the caller's keys/values SILENTLY UNSORTED — a wrong
    // answer with no diagnostic, which is the outcome §31.4 exists to prevent.
    int64_t* idx = (int64_t*)malloc((size_t)n * sizeof(int64_t));
    if (!idx) bz_alloc_fail("sort scratch (index array)");
    for (int64_t i = 0; i < n; i++) idx[i] = i;
    BzKVSortCtxStr ctx = {keys, lens, widths};
    qsort_r(idx, (size_t)n, sizeof(int64_t), bz_kv_idx_cmp_str, &ctx);
    char**   ks = (char**)malloc((size_t)n * sizeof(char*));
    if (!ks) bz_alloc_fail("sort scratch (key array)");
    int64_t* ls = lens   ? (int64_t*)malloc((size_t)n * sizeof(int64_t)) : nullptr;
    if (lens && !ls) bz_alloc_fail("sort scratch (length array)");
    // fix #4499: the WIDTH array is a third parallel spine and must be
    // permuted with the other two, or a key's bytes and its width come from
    // different entries after the sort.
    int8_t*  ws = widths ? (int8_t*)malloc((size_t)n * sizeof(int8_t)) : nullptr;
    if (widths && !ws) bz_alloc_fail("sort scratch (width array)");
    void**   vs = values ? (void**)malloc((size_t)n * sizeof(void*)) : nullptr;
    if (values && !vs) bz_alloc_fail("sort scratch (value array)");
    for (int64_t i = 0; i < n; i++) {
        ks[i] = keys[idx[i]];
        if (ls) ls[i] = lens[idx[i]];
        if (ws) ws[i] = widths[idx[i]];
        if (vs) vs[i] = values[idx[i]];
    }
    memcpy(keys, ks, (size_t)n * sizeof(char*));
    if (ls) { memcpy(lens, ls, (size_t)n * sizeof(int64_t)); free(ls); }
    if (ws) { memcpy(widths, ws, (size_t)n * sizeof(int8_t)); free(ws); }
    if (vs) { memcpy(values, vs, (size_t)n * sizeof(void*)); free(vs); }
    free(ks); free(idx);
}

// bz_sort_keys_gen: sort a void** key-bytes-pointer array in ascending order.
// Thin wrapper over bz_sort_kv_gen (values=null).
// Used for SortedSet<T> element iteration (§4.18).
void bz_sort_keys_gen(void** keys, int64_t n,
                      int (*cmp)(const void* a, const void* b)) {
    bz_sort_kv_gen(keys, nullptr, n, cmp);
}

// bz_sort_keys_str: sort char** key-data + int64_t* lens in ascending order.
// Thin wrapper over bz_sort_kv_str (values=null).
// Used for SortedDictionary<string,V>.Keys sorted iteration (§4.17).
void bz_sort_keys_str(char** keys, int64_t* lens, int8_t* widths, int64_t n) {
    bz_sort_kv_str(keys, lens, widths, nullptr, n);
}

// issue #628 / stdlib §4.9 v0.135: OwningDictionary<K,V>.Drain() per-entry pop.
//
// bz_dict_drain_next_str — string-keyed dict (key_size == 0).
// Pops the next live entry in insertion order from the dict: removes it from
// the bucket (marks deleted) and returns its raw key bytes and value pointer.
// The caller takes ownership of the value; the key bytes are a freshly malloc'd
// copy that the caller must free(). Returns true if an entry was popped; false
// if the dict is empty (drain complete).
//
// The dict's `order[]` array is consumed from the front: we scan forward from
// the current scan position (tracked by a caller-supplied cursor, starting at 0)
// for the first live slot, pop it, and return. Because the caller advances the
// cursor each call, the scan is amortized O(1) per live entry over the full drain.
// The cursor is purely a scan hint; the caller passes a pointer to an int64_t
// that starts at 0 and is updated here.
bool bz_dict_drain_next_str(void* dp, int64_t* cursor,
                             char** key_out, int64_t* key_len_out,
                             int8_t* key_w_out, void** val_out) {
    auto* d = (bz_dict_t*)dp;
    if (!d || d->count == 0) return false;
    // Advance cursor to the first live slot in the order array.
    while (*cursor < d->order_count) {
        int64_t slot = d->order[*cursor];
        (*cursor)++;
        if (slot < 0) continue;  // tombstone
        bz_entry_t& e = d->buckets[slot];
        if (!e.key || e.deleted) continue;  // already gone
        // Found a live entry: hand ownership of key+val to caller.
        // fix #4499: malloc+memcpy of `key_len * key_w` bytes; the width
        // rides along so the caller can rebuild the key at its true width.
        if (key_out)     *key_out     = bz_key_dup(e.key, e.key_len, e.key_w);
        if (key_len_out) *key_len_out = e.key_len;
        if (key_w_out)   *key_w_out   = e.key_w ? e.key_w : 1;
        if (val_out)     *val_out     = e.val;
        // Remove from the bucket (mark deleted, free key storage, nil val).
        free(e.key);
        e.key     = nullptr;
        e.val     = nullptr;
        e.deleted = true;
        d->count--;
        return true;
    }
    return false;
}

// bz_dict_drain_next_gen — generic-keyed dict (key_size > 0).
// Mirrors bz_dict_drain_next_str; the key is returned as a malloc'd copy of the
// raw bytes (size = d->key_size). The caller must free() it when done.
bool bz_dict_drain_next_gen(void* dp, int64_t* cursor,
                             void** key_out, void** val_out) {
    auto* d = (bz_dict_t*)dp;
    if (!d || d->count == 0 || d->key_size <= 0) return false;
    while (*cursor < d->order_count) {
        int64_t slot = d->order[*cursor];
        (*cursor)++;
        if (slot < 0) continue;  // tombstone
        bz_entry_t& e = d->buckets[slot];
        if (!e.key || e.deleted) continue;
        // Pop: return copies of key bytes and the value pointer.
        if (key_out) {
            void* kc = malloc((size_t)d->key_size);
            memcpy(kc, e.key, (size_t)d->key_size);
            *key_out = kc;
        }
        if (val_out) *val_out = e.val;
        free(e.key);
        e.key     = nullptr;
        e.val     = nullptr;
        e.deleted = true;
        d->count--;
        return true;
    }
    return false;
}

// issue #639 / stdlib §4.5/§4.7/§4.11 v0.135: OwningArray<T> / OwningLinkedList<T>
// / OwningStack<T> / OwningQueue<T> .Drain() per-element pop.
//
// ---------------------------------------------------------------------------
// §29.12 / issue #1232: modification-epoch accessors.
// These are called by codegen at the START of a for-in loop (to capture the
// epoch) and at the start of EACH ITERATION (to check for mutations).
// A mismatch → codegen calls emitGuardThrow("InvalidOperationException").
// ---------------------------------------------------------------------------

int64_t bz_array_get_mod(void* arrp) {
    return arrp ? ((bz_array_t*)arrp)->mod_count : 0;
}

int64_t bz_dict_get_mod(void* dp) {
    return dp ? ((bz_dict_t*)dp)->mod_count : 0;
}

// bz_llist_get_mod is placed after the bz_llist_t struct definition below
// (forward reference — the struct is declared at that point in the file).

// ---------------------------------------------------------------------------
// bz_array_drain_next — pops index 0 (front-to-back).
// Used by OwningArray<T>.Drain() (front-to-back per §28.11 "iteration order")
// and OwningQueue<T>.Drain() (FIFO, also front-to-back).
// Returns true if an element was popped into *val_out; false when the array is
// empty (drain complete). The caller takes ownership of the popped pointer.
// The element slot is closed by bz_array_remove_at so count shrinks by 1.
bool bz_array_drain_next(void* arrp, void** val_out) {
    auto* arr = (bz_array_t*)arrp;
    if (!arr || arr->count == 0) return false;
    // Read the pointer-sized element at index 0 (class-typed element).
    void* elem = ((void**)arr->data)[0];
    if (val_out) *val_out = elem;
    // Remove the slot without disposal — ownership transferred to caller.
    bz_array_remove_at(arrp, 0, (int64_t)sizeof(void*));
    return true;
}

// bz_array_drain_next_lifo — pops the last element (LIFO / back-to-front).
// Used by OwningStack<T>.Drain() which pops in top-first order (the stack's
// Pop order), matching "iteration order" per §28.11.
bool bz_array_drain_next_lifo(void* arrp, void** val_out) {
    auto* arr = (bz_array_t*)arrp;
    if (!arr || arr->count == 0) return false;
    int64_t last = arr->count - 1;
    void* elem = ((void**)arr->data)[last];
    if (val_out) *val_out = elem;
    // Shrink count by 1 (the last element's slot needs no shift).
    arr->count = last;
    return true;
}

// issue #389 / stdlib §4.10: fold a snapshot of `other`'s elements into/against
// a dict-backed Set `self`. See the header for the `op` codes and the snapshot
// contract. Frees the snapshot before returning. UnionWith/ExceptWith iterate
// the snapshot and mutate self (no mutate-while-iterate hazard — the snapshot is
// a private copy). IntersectWith and the subset/superset queries first build a
// temporary membership table from the snapshot using self's own hash/eq/key_size
// (so equality follows T's contract, §8.3), then compare; IntersectWith also
// snapshots self's keys before removing, so it never mutates a table it iterates.
int32_t bz_set_algebra_gen(void* selfp, void** keys, int64_t n, int32_t op) {
    auto* self = (bz_dict_t*)selfp;
    int32_t result = 0;
    if (!self || self->key_size <= 0) { /* fall through to free */ }
    else if (op == 0) {                 // UnionWith
        for (int64_t i = 0; i < n; i++)
            bz_dict_set_gen(self, keys[i], (void*)1);
    } else if (op == 1) {               // ExceptWith
        for (int64_t i = 0; i < n; i++)
            bz_dict_remove_gen(self, keys[i]);
    } else {                            // IntersectWith / IsSubsetOf / IsSupersetOf
        // Build a temp membership set from the snapshot, keyed like self.
        auto* other = (bz_dict_t*)bz_dict_alloc_generic(
            self->key_size, self->hash_fn, self->eq_fn);
        for (int64_t i = 0; i < n; i++)
            bz_dict_set_gen(other, keys[i], (void*)1);
        if (op == 2) {                  // IntersectWith: keep self ∩ other
            // Snapshot self's keys first, then remove every key not in `other`.
            void** selfKeys = nullptr; int64_t sn = 0;
            bz_dict_keys_gen(self, &selfKeys, &sn);
            for (int64_t i = 0; i < sn; i++) {
                if (!bz_dict_contains_gen(other, selfKeys[i]))
                    bz_dict_remove_gen(self, selfKeys[i]);
                free(selfKeys[i]);
            }
            free(selfKeys);
        } else if (op == 3) {           // IsSubsetOf: every self element in other
            result = 1;
            void** selfKeys = nullptr; int64_t sn = 0;
            bz_dict_keys_gen(self, &selfKeys, &sn);
            for (int64_t i = 0; i < sn; i++) {
                if (!bz_dict_contains_gen(other, selfKeys[i])) result = 0;
                free(selfKeys[i]);
            }
            free(selfKeys);
        } else if (op == 4) {           // IsSupersetOf: every other element in self
            result = 1;
            for (int64_t i = 0; i < n; i++)
                if (!bz_dict_contains_gen(self, keys[i])) result = 0;
        }
        bz_dict_free(other, nullptr);   // sentinel values: nothing to dispose
    }
    for (int64_t i = 0; i < n; i++) free(keys[i]);
    free(keys);
    return result;
}

// issue #429 / §28.9 / §4.10: release every live string element's +1.
// A Set<string> stores each element as a bz_string* in the generic-key
// table's key slot (key_size == sizeof(void*)). This function reads the
// string pointer from each key's raw bytes and calls bz_string_release on
// it BEFORE bz_dict_free frees the key copy. Call this immediately before
// bz_dict_free(d, null) — after this call the RC accounting is discharged;
// the subsequent bz_dict_free reclaims the key copies and the table itself.
// null/empty-table safe; a no-op on a non-generic table.
void bz_set_string_keys_release(void* dp) {
    if (!dp) return;
    auto* d = (bz_dict_t*)dp;
    if (d->key_size != (int64_t)sizeof(void*)) return;  // not a pointer-key set
    for (int64_t i = 0; i < d->capacity; i++) {
        bz_entry_t& e = d->buckets[i];
        if (e.key && !e.deleted) {
            void* strPtr;
            memcpy(&strPtr, e.key, sizeof(void*));
            if (strPtr) bz_string_release(strPtr);
        }
    }
}

// fix #4091 / §28.11 / §28.9: free each live element of a dict-backed
// `OwningSet<T>` whose T is a BORROWING built-in collection (Array / Stack /
// Queue / LinkedList / FrozenArray / Dictionary / SortedDictionary). The set
// stores each element as a void* in the generic-key table's key slot
// (key_size == sizeof(void*)); for each non-null live key this reads the
// pointer value and calls `dispose_coll` on it (the per-TypeRef thunk codegen
// builds in elementPolicyFor's #4054 borrowing-builtin arm, which calls the
// matching runtime free fn — bz_array_free / bz_llist_free / bz_dict_free —
// with NULL inner disposer). The borrow family does not own its elements, so
// the recursive element-axis is fixed at NULL; the dispose_coll thunk reclaims
// the inner collection's header + backing exactly once.
//
// Call this immediately before bz_dict_free(d, null) — after this call the
// inner collections have been freed; the subsequent bz_dict_free reclaims the
// key copies and the table itself. null/empty-table safe; a no-op on a
// non-generic table (key_size != sizeof(void*)). The dispose_coll argument may
// be null, in which case the helper is a no-op — the caller is responsible for
// never passing an OwningSet whose element type lacks a built-in collection
// disposer (codegen never sets dictKeyDisposer to null, by construction).
void bz_set_coll_keys_dispose(void* dp, void (*dispose_coll)(void*)) {
    if (!dp || !dispose_coll) return;
    auto* d = (bz_dict_t*)dp;
    if (d->key_size != (int64_t)sizeof(void*)) return;  // not a pointer-key set
    for (int64_t i = 0; i < d->capacity; i++) {
        bz_entry_t& e = d->buckets[i];
        if (e.key && !e.deleted) {
            void* collPtr;
            memcpy(&collPtr, e.key, sizeof(void*));
            if (collPtr) dispose_coll(collPtr);
        }
    }
}

// issue #429 / §28.9 / §4.10: like bz_dict_clear(d, null) but also releases
// each live element's string +1 before freeing the key copy. Used by
// Set<string>.Clear() so that cleared string elements are reclaimed (else
// they would leak). The table itself stays allocated and usable afterward,
// as with bz_dict_clear.
void bz_dict_clear_set_strings(void* dp) {
    if (!dp) return;
    auto* d = (bz_dict_t*)dp;
    if (d->key_size != (int64_t)sizeof(void*)) return;
    for (int64_t i = 0; i < d->capacity; i++) {
        bz_entry_t& e = d->buckets[i];
        if (e.key && !e.deleted) {
            // Release the string element (+1 discharge) before freeing key copy.
            void* strPtr;
            memcpy(&strPtr, e.key, sizeof(void*));
            if (strPtr) bz_string_release(strPtr);
            free(e.key);
            // value is a sentinel (1) — nothing to dispose
        }
    }
    memset(d->buckets, 0, (size_t)d->capacity * sizeof(bz_entry_t));
    d->count = 0;
    d->order_count = 0;  // issue #622: reset insertion-order tracking on Clear
    // fix #4623 / §29.12, v0.307: Clear() is a listed structural operation
    // ("Add, ..., Clear, ...") — this string-releasing twin of bz_dict_clear
    // must bump mod_count exactly as bz_dict_clear does, else a `for` over a
    // Set<string> that Clear()s the same set mid-iteration never raises
    // InvalidOperationException (a false negative — the enumerator keeps
    // walking a table that was just emptied underneath it).
    d->mod_count++;  // §29.12 / #4623: structural mutation
}

// issue #429 / §28.9 / §4.10: like bz_dict_remove_gen but releases the
// removed string element's +1 before freeing the key copy. Used by
// Set<string>.Remove() so that the removed element is reclaimed (else it
// would leak). Returns true iff the element was present and removed.
bool bz_dict_remove_set_string(void* dp, const void* key) {
    auto* d = (bz_dict_t*)dp;
    if (!d || !key || d->key_size <= 0) return false;
    uint32_t h = (uint32_t)d->hash_fn(key);
    int64_t idx = dict_find_gen(d, key, h);
    if (idx < 0) return false;
    dict_order_remove(d, idx);  // issue #622: untrack this slot from insertion order
    // Release the string element's +1 before freeing the key copy.
    if (d->buckets[idx].key) {
        void* strPtr;
        memcpy(&strPtr, d->buckets[idx].key, sizeof(void*));
        if (strPtr) bz_string_release(strPtr);
    }
    free(d->buckets[idx].key);
    d->buckets[idx].key = nullptr;
    d->buckets[idx].val = nullptr;
    d->buckets[idx].deleted = true;
    d->count--;
    // fix #4623 / §29.12, v0.307: Remove() is a listed structural operation
    // ("Add, ..., Remove, ..."), same as bz_dict_remove_gen (the non-string
    // twin this function otherwise mirrors) and bz_dict_remove_str — this
    // string-releasing Set<string> path was the one sibling that skipped the
    // bump, so a `for` over a Set<string> that Remove()s the same set
    // mid-iteration never raised InvalidOperationException (false negative).
    d->mod_count++;  // §29.12 / #4623: structural mutation
    return true;
}

// ---------------------------------------------------------------------------
// bz_llist_t — doubly-linked list (stdlib §4.6 / §4.7)
// ---------------------------------------------------------------------------

// Node layout: prev*, next*, then elem_size bytes of element value inline.
// The list header tracks head, tail, count, and the fixed elem_size used to
// allocate nodes (so remove/free don't need a separately-passed elem_size).
struct bz_llist_node_t {
    bz_llist_node_t* prev;
    bz_llist_node_t* next;
    // #869 / §28.11: monotonic insertion sequence, so OwningLinkedList disposal
    // can run in REVERSE-INSERTION order regardless of whether elements were
    // added at the head (AddFirst) or tail (AddLast) — the head/tail POSITION is
    // not insertion order once AddFirst/AddBefore are mixed in.
    int64_t          seq;
    // element bytes follow immediately — flexible array trick via alloc
};

struct bz_llist_t {
    bz_llist_node_t* head;
    bz_llist_node_t* tail;
    int64_t          count;
    int64_t          elem_size;
    int64_t          next_seq;  // #869: next insertion sequence to assign.
    // §29.12 / issue #1232: modification epoch. Bumped on every structural
    // mutation (AddFirst/Last/Before/After, Remove*, Pop*). Captured by
    // lowerLinkedListFor at loop start; checked per iteration.
    int64_t          mod_count;
    // fix #3026: kind tag (BZ_RUNTIME_KIND_LLIST) set by bz_llist_alloc at
    // offset 48 — the same offset bz_array_t's kind uses, so the runtime
    // helper bz_runtime_dispose can probe ONE offset (48) for both array-
    // and llist-backed polymorphic collection interfaces. Sized to keep
    // the prior offsets (head@0, tail@8, count@16, elem_size@24,
    // next_seq@32, mod_count@40) unchanged — existing codegen and
    // runtime read those at fixed offsets.
    int64_t          kind;
};

// Allocate a node with elem_size bytes of value storage, stamping it with the
// list's next insertion sequence (#869).
static bz_llist_node_t* llist_node_alloc(bz_llist_t* l, int64_t elem_size,
                                         const void* elem) {
    auto* n = (bz_llist_node_t*)calloc(
        1, sizeof(bz_llist_node_t) + (size_t)elem_size);
    if (elem && elem_size > 0)
        memcpy((char*)n + sizeof(bz_llist_node_t), elem, (size_t)elem_size);
    n->seq = l->next_seq++;  // #869: monotonic insertion order
    l->mod_count++;  // §29.12 / #1232: structural mutation (covers all Add*)
    return n;
}

void* bz_llist_alloc(int64_t elem_size) {
    auto* l = (bz_llist_t*)calloc(1, sizeof(bz_llist_t));
    if (!l) bz_alloc_fail("LinkedList<T> header");
    l->kind = BZ_RUNTIME_KIND_LLIST;  // fix #3026
    l->elem_size = elem_size;
    return l;
}

int64_t bz_llist_count(void* listp) {
    return listp ? ((bz_llist_t*)listp)->count : 0;
}

int64_t bz_llist_get_mod(void* listp) {
    return listp ? ((bz_llist_t*)listp)->mod_count : 0;
}

void* bz_llist_add_first(void* listp, const void* elem, int64_t elem_size) {
    auto* l = (bz_llist_t*)listp;
    if (!l) return nullptr;
    bz_llist_node_t* n = llist_node_alloc(l, elem_size, elem);
    n->next = l->head;
    n->prev = nullptr;
    if (l->head) l->head->prev = n;
    l->head = n;
    if (!l->tail) l->tail = n;
    l->count++;
    return n;
}

void* bz_llist_add_last(void* listp, const void* elem, int64_t elem_size) {
    auto* l = (bz_llist_t*)listp;
    if (!l) return nullptr;
    bz_llist_node_t* n = llist_node_alloc(l, elem_size, elem);
    n->prev = l->tail;
    n->next = nullptr;
    if (l->tail) l->tail->next = n;
    l->tail = n;
    if (!l->head) l->head = n;
    l->count++;
    return n;
}

void* bz_llist_add_before(void* listp, void* nodep, const void* elem,
                           int64_t elem_size) {
    auto* l = (bz_llist_t*)listp;
    auto* node = (bz_llist_node_t*)nodep;
    if (!l || !node) return nullptr;
    if (node == l->head) return bz_llist_add_first(listp, elem, elem_size);
    bz_llist_node_t* n = llist_node_alloc(l, elem_size, elem);
    n->next = node;
    n->prev = node->prev;
    if (node->prev) node->prev->next = n;
    node->prev = n;
    l->count++;
    return n;
}

void* bz_llist_add_after(void* listp, void* nodep, const void* elem,
                          int64_t elem_size) {
    auto* l = (bz_llist_t*)listp;
    auto* node = (bz_llist_node_t*)nodep;
    if (!l || !node) return nullptr;
    if (node == l->tail) return bz_llist_add_last(listp, elem, elem_size);
    bz_llist_node_t* n = llist_node_alloc(l, elem_size, elem);
    n->prev = node;
    n->next = node->next;
    if (node->next) node->next->prev = n;
    node->next = n;
    l->count++;
    return n;
}

// Internal: unlink and free `node`, calling dispose_elem if non-null.
static void llist_unlink(bz_llist_t* l, bz_llist_node_t* node,
                         void (*dispose_elem)(void*)) {
    if (node->prev) node->prev->next = node->next;
    else l->head = node->next;
    if (node->next) node->next->prev = node->prev;
    else l->tail = node->prev;
    l->count--;
    l->mod_count++;  // §29.12 / #1232: structural mutation (covers all Remove*/Pop*)
    if (dispose_elem) {
        void* vp = (char*)node + sizeof(bz_llist_node_t);
        // For OwningLinkedList<T: class>, element bytes hold a pointer; load it.
        void* obj = *(void**)vp;
        if (obj) dispose_elem(obj);
    }
    free(node);
}

void bz_llist_remove_first(void* listp, void (*dispose_elem)(void*)) {
    auto* l = (bz_llist_t*)listp;
    if (!l || !l->head) return;
    llist_unlink(l, l->head, dispose_elem);
}

void bz_llist_remove_last(void* listp, void (*dispose_elem)(void*)) {
    auto* l = (bz_llist_t*)listp;
    if (!l || !l->tail) return;
    llist_unlink(l, l->tail, dispose_elem);
}

void bz_llist_remove_node(void* listp, void* nodep,
                           void (*dispose_elem)(void*)) {
    auto* l = (bz_llist_t*)listp;
    auto* node = (bz_llist_node_t*)nodep;
    if (!l || !node) return;
    llist_unlink(l, node, dispose_elem);
}

// fix #642 / stdlib §4.7: PopFirst() / PopLast() — copy the first/last element
// into *out, transferring ownership to the caller, then unlink WITHOUT disposal.
// Unlike llist_unlink (which calls dispose_elem), these never invoke a disposer.
void bz_llist_pop_first(void* listp, void* out, int64_t elem_size) {
    auto* l = (bz_llist_t*)listp;
    if (!l || !l->head || !out) return;
    bz_llist_node_t* node = l->head;
    // Copy the inline element bytes into the caller's output buffer.
    memcpy(out, (char*)node + sizeof(bz_llist_node_t), (size_t)elem_size);
    // Unlink without disposal (ownership transferred to caller).
    llist_unlink(l, node, /*dispose_elem=*/nullptr);
}

void bz_llist_pop_last(void* listp, void* out, int64_t elem_size) {
    auto* l = (bz_llist_t*)listp;
    if (!l || !l->tail || !out) return;
    bz_llist_node_t* node = l->tail;
    memcpy(out, (char*)node + sizeof(bz_llist_node_t), (size_t)elem_size);
    llist_unlink(l, node, /*dispose_elem=*/nullptr);
}

// Find by memcmp equality on the inline element bytes.
// For class-typed elements (pointer-sized), elem points to a pointer; we
// compare the pointer value (reference identity, per §8.3 / §4.6).
void* bz_llist_find(void* listp, const void* elem, int64_t elem_size) {
    auto* l = (bz_llist_t*)listp;
    if (!l || !elem) return nullptr;
    for (bz_llist_node_t* n = l->head; n; n = n->next) {
        void* vp = (char*)n + sizeof(bz_llist_node_t);
        if (memcmp(vp, elem, (size_t)elem_size) == 0) return n;
    }
    return nullptr;
}

void* bz_llist_first_node(void* listp) {
    return listp ? ((bz_llist_t*)listp)->head : nullptr;
}

void* bz_llist_last_node(void* listp) {
    return listp ? ((bz_llist_t*)listp)->tail : nullptr;
}

void* bz_llist_node_value_ptr(void* nodep) {
    return nodep ? (char*)nodep + sizeof(bz_llist_node_t) : nullptr;
}

void* bz_llist_node_next(void* nodep) {
    return nodep ? ((bz_llist_node_t*)nodep)->next : nullptr;
}

void* bz_llist_node_prev(void* nodep) {
    return nodep ? ((bz_llist_node_t*)nodep)->prev : nullptr;
}

void bz_llist_free(void* listp, void (*dispose_elem)(void*)) {
    auto* l = (bz_llist_t*)listp;
    if (!l) return;
    // §28.11 (#869): dispose elements in REVERSE-INSERTION order — last-inserted
    // first. The positional tail→head walk only equals reverse-insertion when
    // every element was AddLast'd; once AddFirst/AddBefore are mixed in, the
    // head/tail position no longer tracks insertion order, so dispose by
    // DESCENDING insertion sequence (n->seq). Collect the live nodes, sort by
    // seq descending, dispose in that order, then free the nodes and the list.
    // dispose_elem is null for value-typed T (nothing to dispose), so the
    // collect+sort is skipped in that common case.
    if (dispose_elem && l->count > 0) {
        int64_t cap = l->count;
        bz_llist_node_t** arr =
            (bz_llist_node_t**)malloc((size_t)cap * sizeof(bz_llist_node_t*));
        if (arr) {
            int64_t i = 0;
            for (bz_llist_node_t* p = l->head; p && i < cap; p = p->next)
                arr[i++] = p;
            std::sort(arr, arr + i,
                      [](bz_llist_node_t* a, bz_llist_node_t* b) {
                          return a->seq > b->seq;  // descending = reverse-insertion
                      });
            for (int64_t k = 0; k < i; k++) {
                void* obj = *(void**)((char*)arr[k] + sizeof(bz_llist_node_t));
                if (obj) dispose_elem(obj);
            }
            free(arr);
        } else {
            // Allocation failed: fall back to the positional reverse walk (still
            // correct for the AddLast-only case; best effort otherwise).
            for (bz_llist_node_t* p = l->tail; p; p = p->prev) {
                void* obj = *(void**)((char*)p + sizeof(bz_llist_node_t));
                if (obj) dispose_elem(obj);
            }
        }
    }
    // Free all nodes (order irrelevant now) and the list header.
    bz_llist_node_t* n = l->head;
    while (n) {
        bz_llist_node_t* nx = n->next;
        free(n);
        n = nx;
    }
    free(l);
}

// #639 / stdlib §4.7 v0.135: OwningLinkedList<T>.Drain() per-element pop.
// Pops the head node (front-to-back / head-to-tail per §28.11 "iteration order").
// Returns the pointer stored inline in the head node into *val_out, then
// unlinks and frees the node WITHOUT disposal (ownership transferred to caller).
// Returns true if a node was popped; false if the list is empty.
bool bz_llist_drain_next(void* listp, void** val_out) {
    auto* l = (bz_llist_t*)listp;
    if (!l || !l->head) return false;
    bz_llist_node_t* node = l->head;
    // Element bytes stored inline after the node header; for class-typed T this
    // is a single void* (the owned class pointer).
    void* elem = *(void**)((char*)node + sizeof(bz_llist_node_t));
    if (val_out) *val_out = elem;
    // Unlink without disposal — ownership transferred to caller.
    llist_unlink(l, node, /*dispose_elem=*/nullptr);
    return true;
}

// ---------------------------------------------------------------------------
// Random
// ---------------------------------------------------------------------------

struct bz_rng_t { uint64_t state[2]; }; // xorshift128+

static uint64_t xorshift128_next(uint64_t* s) {
    uint64_t t = s[0];
    uint64_t const r = s[1];
    s[0] = r;
    t ^= t << 23;
    t ^= t >> 17;
    t ^= r ^ (r >> 26);
    s[1] = t;
    return t + r;
}

// per stdlib §2.7: Random(seed) ctor — allocate an xorshift128+ state.
// fix #2238 / stdlib §2.7 / spec §31.4: OOM-harden the malloc. The pre-fix code
// wrote through `r` without a null check after malloc(sizeof(bz_rng_t)),
// SIGSEGVing on the next store. Returning NULL instead did not help: every
// consumer (bz_random_next_int / next_double / next_bytes) dereferences the
// handle unchecked, so the crash simply moved one call later. §31.4 requires
// termination — do it here, where the report can say what failed.
void* bz_random_alloc(int64_t seed) {
    auto* r = (bz_rng_t*)malloc(sizeof(bz_rng_t));
    if (!r) bz_alloc_fail("Random state");
    r->state[0] = (uint64_t)seed ^ 0xdeadbeef12345678ULL;
    r->state[1] = (uint64_t)seed * 6364136223846793005ULL + 1442695040888963407ULL;
    if (r->state[0] == 0) r->state[0] = 1;
    if (r->state[1] == 0) r->state[1] = 1;
    return r;
}

void bz_random_free(void* r) { free(r); }

int32_t bz_random_next_int(void* rp, int32_t lo, int32_t hi_exclusive) {
    auto* r = (bz_rng_t*)rp;
    uint64_t v = xorshift128_next(r->state);
    if (hi_exclusive <= lo) return lo;
    return lo + (int32_t)(v % (uint64_t)(hi_exclusive - lo));
}

double bz_random_next_double(void* rp) {
    auto* r = (bz_rng_t*)rp;
    uint64_t v = xorshift128_next(r->state);
    // Map to [0, 1)
    return (double)(v >> 11) * (1.0 / 9007199254740992.0);
}

void bz_random_next_bytes(void* rp, uint8_t* buf, int64_t len) {
    auto* r = (bz_rng_t*)rp;
    for (int64_t i = 0; i < len; i += 8) {
        uint64_t v = xorshift128_next(r->state);
        int64_t chunk = (len - i) < 8 ? (len - i) : 8;
        memcpy(buf + i, &v, (size_t)chunk);
    }
}

// per stdlib §2.7 / #1359: NextInt64 — full 64-bit unsigned xorshift128+ value
// cast to int64_t (signed). Maps the full 64-bit range to [Long.MinValue..Long.MaxValue].
int64_t bz_random_next_long(void* rp) {
    auto* r = (bz_rng_t*)rp;
    return (int64_t)xorshift128_next(r->state);
}

// per stdlib §2.7 / #1359: NextBoolean — test the low bit of a fresh PRNG word.
int32_t bz_random_next_bool(void* rp) {
    auto* r = (bz_rng_t*)rp;
    return (int32_t)(xorshift128_next(r->state) & 1ULL);
}

// ---------------------------------------------------------------------------
// StringBuilder helper
// ---------------------------------------------------------------------------

// The bz.string struct layout: {ptr data, i64 len, i8 width}

// bz_array_enum_t: vtable-backed array enumerator for IEnumerator<T> protocol.
// Vtable layout matches our iterator state machines: [td, Dispose, MoveNext, Current].
struct bz_array_enum_t {
    void* vtable;      // points to bz_array_enum_vtable
    void* arr;         // the underlying bz_array_t*
    int64_t idx;       // current index (-1 = before first)
    int64_t esz;       // element size in bytes
};

static int32_t bz_array_enum_movenext(bz_array_enum_t* e) {
    e->idx++;
    return (e->idx < bz_array_len(e->arr)) ? 1 : 0;
}
static void* bz_array_enum_current(bz_array_enum_t* e) {
    return bz_array_ptr_at(e->arr, e->idx, e->esz);
}
static void bz_array_enum_dispose(bz_array_enum_t* e) {
    free(e);
}

// Static vtable: [self-ptr, Dispose, MoveNext, get_Current]
static void* bz_array_enum_vtable[4] = {
    nullptr,  // slot 0: filled at startup (see bz_array_enum_alloc)
    (void*)bz_array_enum_dispose,
    (void*)bz_array_enum_movenext,
    (void*)bz_array_enum_current
};

void* bz_array_enum_alloc(void* arr, int64_t elem_size) {
    auto* e = (bz_array_enum_t*)calloc(1, sizeof(bz_array_enum_t));
    bz_array_enum_vtable[0] = bz_array_enum_vtable;  // self-referential td
    e->vtable = bz_array_enum_vtable;
    e->arr = arr;
    e->idx = -1;
    e->esz = elem_size;
    return e;
}

// ---------------------------------------------------------------------------
// Lazy LINQ enumerators (stdlib §5: "All are lazy unless documented otherwise").
//
// Every LINQ operator returns a deferred enumerator that does no work until it
// is enumerated (by a for-loop or a materializer like ToArray). The enumerators
// share the iterator vtable protocol [td, Dispose, MoveNext, Current] used by
// the codegen iterator state machines (LlvmBackend_Iterators.cpp) and the array
// enumerator above:
//   slot 0 = type descriptor (self-pointer sentinel)
//   slot 1 = Dispose(self) -> void
//   slot 2 = MoveNext(self) -> int32 (1 = advanced, 0 = exhausted)
//   slot 3 = Current(self)  -> void* (pointer to the current element's bytes)
//
// Element data is type-erased (raw bytes of `esz` bytes). Closures (predicates,
// selectors, key functions) are invoked through codegen-emitted trampolines
// with fixed C signatures so the runtime stays element-type-agnostic:
//   predicate trampoline: int32_t f(void* closure, const void* elem_ptr)
//   selector  trampoline: void    f(void* closure, const void* elem_ptr, void* out_ptr)
//   key       trampoline: int64_t f(void* closure, const void* elem_ptr)
// A lazy operator over an Array<T> wraps the array via bz_lazy_from_array; over
// another lazy enumerator it stores the upstream enumerator pointer directly,
// so chains (`a.Where(..).Select(..).Take(n)`) compose without materializing.
// ---------------------------------------------------------------------------

typedef int32_t (*bz_pred_fn)(void* closure, const void* elem);
typedef void    (*bz_sel_fn)(void* closure, const void* elem, void* out);
typedef int64_t (*bz_key_fn)(void* closure, const void* elem);
// Element equality trampoline (Distinct): content-aware comparison the runtime
// cannot do over type-erased bytes (e.g. string content equality vs. raw bytes).
typedef int32_t (*bz_eq_fn)(const void* a, const void* b);
// Element hash trampoline (Union/Intersect/Except membership dict): maps an
// element's raw bytes to a uint64 hash value for the generic dict backing.
typedef uint64_t (*bz_hash_fn)(const void* elem);
// SelectMany selector trampoline: maps one element to a fresh lazy enumerator
// (the inner IEnumerable<U>). Returns an opaque bz_lazy_t* the runtime drains.
typedef void* (*bz_flatsel_fn)(void* closure, const void* elem);
// Zip combine trampoline: pairs one element from each source into the output.
typedef void  (*bz_comb_fn)(void* closure, const void* a, const void* b,
                            void* out);

enum bz_lazy_kind {
    BZ_LAZY_ARRAY,     // source = bz_array_t*, iterate by index
    BZ_LAZY_WHERE,     // filter via predicate
    BZ_LAZY_SELECT,    // project via selector (esz = output element size)
    BZ_LAZY_TAKE,      // first `n` elements
    BZ_LAZY_SKIP,      // skip first `n` elements
    BZ_LAZY_DISTINCT,  // de-duplicate (raw-byte equality, order preserved)
    BZ_LAZY_ORDERBY,   // materialize-on-first-MoveNext: sorted snapshot
    BZ_LAZY_SET,       // dict-backed Set<T> source: owned key snapshot by index
    BZ_LAZY_CONCAT,    // §5 Combination: yield all of `up`, then all of `down`
    BZ_LAZY_TAKEWHILE, // §5 Slicing: prefix while predicate holds, then stop
    BZ_LAZY_SKIPWHILE, // §5 Slicing: drop leading run satisfying predicate
    BZ_LAZY_SELECTMANY,// §5 Projection: flatten (T)->IEnumerable<U>
    BZ_LAZY_ZIP,       // §5 Combination: pair `up` and `down` via combine
    // stdlib §5 Set-like lazy operators (added milestone):
    BZ_LAZY_UNION,     // §5 Set-like: unique elements from `up` ∪ `down`
    BZ_LAZY_INTERSECT, // §5 Set-like: elements from `up` that are in `down`
    BZ_LAZY_EXCEPT,    // §5 Set-like: elements from `up` not in `down`
    // stdlib §5 Ordering secondary key:
    BZ_LAZY_THENBY,    // §5 Ordering: composite (primary, secondary) sort
    // stdlib §5 Lazy Reverse:
    BZ_LAZY_REVERSE,   // §5 Ordering: materialize then yield backwards
    // per §4.6/§4.7 + §5 fix #450:
    BZ_LAZY_LLIST,     // source = bz_llist_t*, walk nodes head-to-tail
    // issue #775 / §16.3: a `Range<T>` source — a counted sequence from
    // idx(start)→n(end) by up_esz(step), inclusive/exclusive per phase bit0.
    BZ_LAZY_RANGE,
};

struct bz_lazy_t {
    void*   vtable;     // shared bz_lazy_vtable
    int32_t kind;
    int64_t esz;        // element size in bytes (output size for SELECT)
    // Source: either an upstream lazy enumerator (`up`) or an array (`arr`).
    bz_lazy_t* up;      // upstream enumerator (null for ARRAY)
    bz_lazy_t* down;    // second source (CONCAT/ZIP/UNION/INTERSECT/EXCEPT)
    void*   arr;        // bz_array_t* (ARRAY kind)
    int64_t idx;        // ARRAY: current index; TAKE/SKIP: count consumed
    int64_t n;          // TAKE/SKIP threshold
    // Closure + trampoline (WHERE/SELECT/ORDERBY).
    void*   closure;
    bz_pred_fn pred;
    bz_sel_fn  sel;
    bz_key_fn  keyfn;
    bz_eq_fn   eqfn;    // DISTINCT/UNION/INTERSECT/EXCEPT: element equality
    bz_hash_fn hashfn;  // UNION/INTERSECT/EXCEPT: element hash (membership dict)
    bz_flatsel_fn flatsel;  // SELECTMANY: element -> inner lazy enumerator
    bz_comb_fn comb;        // ZIP: (a, b) -> out
    int32_t descending; // ORDERBY/THENBY primary direction
    int64_t up_esz;     // ZIP: byte stride of the `up`/left element
    int64_t down_esz;   // ZIP: byte stride of the `down`/right element
    // Scratch storage for the current element bytes (Current returns cur).
    void*   cur;        // malloc'd, esz bytes
    // DISTINCT: a dynamic array of seen elements.
    void*   seen;       // bz_array_t* of element bytes (DISTINCT) OR
                        // bz_dict_t* (UNION/INTERSECT/EXCEPT: elements seen so far)
    // ORDERBY/THENBY/REVERSE: materialized + sorted snapshot, iterated by index.
    // UNION/INTERSECT/EXCEPT: pre-built dict of `down` elements (membership test).
    void*   sorted;     // bz_array_t* (ORDERBY/THENBY/REVERSE) or bz_dict_t*
    int32_t skipped;    // SKIP/SKIPWHILE: have we already dropped the prefix?
    int32_t materialized;
    int32_t phase;      // CONCAT: 0 = draining `up`, 1 = draining `down`
                        // UNION: 0 = draining `up` (unique), 1 = from `other_keys`
    int32_t stopped;    // TAKEWHILE: predicate has failed; sequence ended
    bz_lazy_t* inner;   // SELECTMANY: current inner enumerator (null = none)
    // #422 / §28.9 / §28.3: when the operator's closure was produced INLINE (a
    // lambda literal — a +1 transient this enumerator owns), codegen sets
    // own_closure := 1 so bz_lazy_dispose releases it; a closure arriving via a
    // variable/field is a BORROW (own_closure stays 0 from calloc) and is never
    // freed here. Mirrors the GetOrAdd factoryIsInlineLambda guard.
    int32_t own_closure;
    // THENBY secondary sort key:
    void*      closure2;   // secondary key closure (THENBY)
    bz_key_fn  keyfn2;    // secondary key trampoline (THENBY)
    int32_t    descending2; // secondary descending flag (THENBY)
    int32_t    own_closure2; // secondary closure ownership (THENBY)
    // UNION phase-1 iteration: pointer to the `down`-side key array snapshot.
    // Built from the `sorted` (other_dict) when `up` is exhausted.
    void**  other_keys;   // malloc'd array of element pointers for UNION phase 1
    int64_t other_n;      // count of other_keys
    // issue #995 / stdlib §5: ARRAY-kind ownership. A normal `bz_lazy_from_array`
    // BORROWS its array (the array is owned by the Array<T> local and freed
    // elsewhere — own_arr == 0). When the array is a FRESH temporary the lazy
    // source owns — e.g. materialized from a vtable-backed IEnumerable<T> via
    // bz_lazy_from_owned_array — own_arr == 1, so bz_lazy_dispose frees it (with a
    // null disposer: the snapshot borrows its elements, it owns no element). This
    // makes the materialize-then-enumerate bridge leak-free for both the terminal
    // (bz_lazy_to_array) and the operator-chain (Where/Select/…) consumers.
    int32_t own_arr;
    // stdlib §5 (#1914): when the owned ARRAY (own_arr == 1) is a fresh
    // `Array<IGrouping>` from bz_groupby_to_array, its SLOTS are bz_grouping_t*
    // pointers this lazy source also owns — bz_lazy_dispose must free each
    // grouping (bz_groupby_free_groups) BEFORE freeing the outer array. Set only
    // by bz_lazy_from_owned_grouping_array (the bare for-in GroupBy path); 0 (the
    // borrowing bz_lazy_from_array / element-borrowing bz_lazy_from_owned_array
    // paths) leaves the slots untouched. Disjoint from own_closure/seen/sorted.
    int32_t arr_is_groupings;
    // #2544 (stdlib §4.8/§4.12; spec §29.1, v0.236): when the owned ARRAY
    // (own_arr == 1) is a FRESH materialisation of a string-keyed
    // Dictionary<string,V> from the Calls3 LINQ source path
    // (lowerToLazyLinqSource Dictionary case), each slot is a {bz_string*, V}
    // STRUCT whose Key field is a +1 rebuilt string (initStringRc -> rc=1).
    // bz_lazy_dispose must walk the array and bz_string_release each Key at
    // struct offset 0 BEFORE bz_array_free. Set only by
    // bz_lazy_from_owned_str_dict_array; 0 (the plain bz_lazy_from_array /
    // bz_lazy_from_owned_array / bz_lazy_from_owned_grouping_array paths)
    // leaves the slots untouched. Disjoint from arr_is_groupings.
    int32_t arr_str_keys;
    // #6380 (§28.3 *unbound owned temporaries*, §28.9, stdlib §4.4–§4.11 /
    // §4.17–§4.18, v0.338): the SOURCE this lazy wraps when that source is an
    // UNBOUND fresh `owned` collection temporary — an `owned`-returning producer
    // (or a fresh materializer call) used directly as a LINQ receiver, which no
    // name, field or parameter owns and which therefore nothing else will free.
    // `own_src_kind` is 0 (no owned source), 1 `bz_array_t*`, 2 `bz_llist_t*`,
    // 3 `bz_dict_t*` — the SAME three-way backing classification
    // freeUnboundOwnedBorrowingArrayTemp dispatches on, so one unwritten
    // temporary is reclaimed by exactly one runtime free no matter which LINQ
    // consumer drains the pipeline. DISJOINT from `own_arr` (the SNAPSHOT this
    // source built and owns, freed with a null element disposer above); a
    // `bz_lazy_t` may hold both — a Dictionary receiver owns both its own table
    // and the entry array materialized from it. Set only by bz_lazy_own_source,
    // which codegen calls under the unbound-temporary gate.
    int32_t own_src_kind;
    void*   owned_src;
};

// Advance the upstream enumerator and return a pointer to its current element,
// or null when exhausted.
static const void* bz_lazy_up_next(bz_lazy_t* up) {
    typedef int32_t (*mn_t)(void*);
    typedef void*   (*cur_t)(void*);
    void** vt = (void**)up->vtable;
    if (!((mn_t)vt[2])(up)) return nullptr;
    return ((cur_t)vt[3])(up);
}

extern "C" void bz_closure_release(void* c);  // defined below; released on dispose
extern "C" void bz_groupby_free_groups(void* groups_arr);  // defined below (#1914)
static void bz_lazy_release_str_dict_keys(void* arr, int64_t esz);  // #2544, defined below

static int32_t bz_lazy_movenext(bz_lazy_t* e);
static void*   bz_lazy_current(bz_lazy_t* e) { return e->cur; }
static void    bz_lazy_dispose(bz_lazy_t* e) {
    if (!e) return;
    if (e->up) bz_lazy_dispose(e->up);
    if (e->down) bz_lazy_dispose(e->down);   // CONCAT/ZIP second source
    // LLIST uses `inner` as a bz_llist_node_t* borrow (not an owned enumerator).
    if (e->inner && e->kind != BZ_LAZY_LLIST) bz_lazy_dispose(e->inner);  // SELECTMANY
    if (e->cur) free(e->cur);
    // DISTINCT uses `seen` as a bz_array_t*; UNION/INTERSECT/EXCEPT use it as a
    // bz_dict_t* (membership dict). Both are heap-allocated and must be freed.
    if (e->seen) {
        if (e->kind == BZ_LAZY_UNION || e->kind == BZ_LAZY_INTERSECT ||
            e->kind == BZ_LAZY_EXCEPT)
            bz_dict_free(e->seen, nullptr);  // seen dict
        else
            bz_array_free(e->seen, nullptr);
    }
    // `sorted` is bz_array_t* for ORDERBY/THENBY/REVERSE/SET, bz_dict_t* for
    // UNION/INTERSECT/EXCEPT (the `other` membership dict).
    if (e->sorted) {
        if (e->kind == BZ_LAZY_UNION || e->kind == BZ_LAZY_INTERSECT ||
            e->kind == BZ_LAZY_EXCEPT)
            bz_dict_free(e->sorted, nullptr);  // other dict
        else
            bz_array_free(e->sorted, nullptr);
    }
    // UNION phase-1 other_keys snapshot (if built).
    if (e->other_keys) {
        for (int64_t i = 0; i < e->other_n; i++) free(e->other_keys[i]);
        free(e->other_keys);
    }
    // issue #995 / stdlib §5: free a fresh temporary ARRAY this lazy source OWNS
    // (own_arr set by bz_lazy_from_owned_array — e.g. a snapshot materialized from
    // a vtable-backed IEnumerable<T>). A borrowed array (own_arr == 0, the normal
    // bz_lazy_from_array path) is left untouched. The snapshot owns no elements, so
    // the disposer is null (no element disposal) — sound, no double-free.
    // #1914: when the owned array is a fresh Array<IGrouping> (arr_is_groupings),
    // this lazy source also owns each grouping — free them (element array + key
    // data + group object) BEFORE the outer array. bz_groupby_free_groups runs
    // exactly once (dispose is once per enumerator), so no double-free.
    // #2544: when the owned array is a fresh Dictionary<string,V> materialisation
    // (arr_str_keys), each slot is a {bz_string*, V} struct and the Key at offset
    // 0 is a +1 rebuilt string — walk the array and bz_string_release each Key
    // BEFORE bz_array_free. bz_string_release is null-safe; the array element
    // disposer is null because the {K, V} struct layout is fixed (not pointer-elem).
    if (e->own_arr && e->arr) {
        if (e->arr_is_groupings) bz_groupby_free_groups(e->arr);
        if (e->arr_str_keys) bz_lazy_release_str_dict_keys(e->arr, e->esz);
        bz_array_free(e->arr, nullptr);
    }
    // #6380 (§28.3 K8, §28.9): reclaim the UNBOUND fresh `owned` collection
    // temporary this source WRAPS, if codegen marked it owned
    // (bz_lazy_own_source). Nothing else holds that value — it was produced by a
    // call and consumed directly as a receiver — so leaving it to dispose is the
    // only place it can be freed. Disposal runs exactly once per enumerator
    // (every consumer drains then disposes: bz_lazy_to_array / bz_lazy_to_dict /
    // the for-loop), so this is a single release, never a double free.
    //
    // NULL element disposer, and that is the SAFE direction rather than a
    // shortcut: this source's ELEMENTS are borrows (every family named here is a
    // borrowing collection, stdlib §3/§5 — the producer owns its elements, not
    // this receiver), so releasing them here would be a use-after-free for any
    // result that aliased a slot. The cost is that an `Array<string>` source
    // leaves its per-element co-owned +1 unreleased — a bounded per-element leak
    // on a shape that leaked the whole array before this path existed.
    //
    // Free AFTER the `own_arr` snapshot above: a Dictionary receiver owns both
    // its own table and the entry array materialized from it, and the entry
    // array's slots are COPIES of the table's key/value bytes, so releasing the
    // copy first cannot dangle the table (which the runtime free below owns).
    if (e->own_src_kind && e->owned_src) {
        if (e->own_src_kind == 2)      bz_llist_free(e->owned_src, nullptr);
        else if (e->own_src_kind == 3) bz_dict_free(e->owned_src, nullptr);
        else                           bz_array_free(e->owned_src, nullptr);
        e->owned_src = nullptr;
        e->own_src_kind = 0;
    }
    // #422 / §28.9 / §28.3: release the operator's closure ONLY when it was an
    // inline lambda we own (own_closure set by codegen). The closure BORROWS its
    // captures, so this frees only the cell + env, never a capture — sound, no
    // double-dispose. A borrowed (variable-form) closure has own_closure == 0 and
    // is left untouched. Dispose runs exactly once per enumerator (cascaded from
    // the for-loop / ToArray drain), so this is a single release, no double-free.
    if (e->own_closure && e->closure) bz_closure_release(e->closure);
    // THENBY secondary closure release (same ownership convention).
    if (e->own_closure2 && e->closure2) bz_closure_release(e->closure2);
    free(e);
}

static void* bz_lazy_vtable[4] = {
    nullptr,
    (void*)bz_lazy_dispose,
    (void*)bz_lazy_movenext,
    (void*)bz_lazy_current,
};

static bz_lazy_t* bz_lazy_new(int32_t kind, int64_t esz) {
    auto* e = (bz_lazy_t*)calloc(1, sizeof(bz_lazy_t));
    bz_lazy_vtable[0] = bz_lazy_vtable;  // self-referential td sentinel
    e->vtable = bz_lazy_vtable;
    e->kind = kind;
    e->esz = esz;
    e->idx = 0;
    if (esz > 0) e->cur = malloc((size_t)esz);
    return e;
}

extern "C" void* bz_lazy_from_array(void* arr, int64_t esz) {
    auto* e = bz_lazy_new(BZ_LAZY_ARRAY, esz);
    e->arr = arr;
    return e;
}

// issue #995 / stdlib §5 / §29.11: wrap a FRESH temporary array as a lazy ARRAY
// source that OWNS the array (own_arr := 1 → bz_lazy_dispose frees it with a null
// disposer, the snapshot owning no element). Used by the vtable-backed
// IEnumerable<T> bridge (materializeVtableIEnumerableToArray): the source object's
// elements are snapshotted into a fresh array by driving the iterator vtable once,
// and that array has no other owner — so it is freed when the lazy pipeline (a
// terminal's bz_lazy_to_array drain, or a Where/Select/… for-in) disposes.
extern "C" void* bz_lazy_from_owned_array(void* arr, int64_t esz) {
    auto* e = bz_lazy_new(BZ_LAZY_ARRAY, esz);
    e->arr = arr;
    e->own_arr = 1;
    return e;
}

// stdlib §5 (#1914): wrap a FRESH `Array<IGrouping>` (bz_array_t* of
// bz_grouping_t* pointers, from bz_groupby_to_array) as a lazy ARRAY source that
// OWNS the array AND every grouping inside it (own_arr := 1, arr_is_groupings :=
// 1 → bz_lazy_dispose runs bz_groupby_free_groups then frees the outer array).
// Used by the bare `for var g in seq.GroupBy(sel)` path: the groups temporary
// has no other owner, the loop consumes each grouping during iteration, and the
// lazy is disposed only after the loop drains — so freeing the groups at dispose
// is exactly-once and never a use-after-free.
extern "C" void* bz_lazy_from_owned_grouping_array(void* arr, int64_t esz) {
    auto* e = bz_lazy_new(BZ_LAZY_ARRAY, esz);
    e->arr = arr;
    e->own_arr = 1;
    e->arr_is_groupings = 1;
    return e;
}

// #2544 (stdlib §4.8/§4.12; spec §29.1, v0.236): walk a fresh Dictionary<string,V>
// materialisation ({bz_string*, V} struct array) and bz_string_release each Key
// at struct offset 0. Used by bz_lazy_dispose when arr_str_keys is set. The Key
// strings were +1 rebuilt by codegen's lowerToLazyLinqSource Dictionary case
// (initStringRc → rc=1) and would otherwise leak — bz_array_free's element
// disposer is `void(*)(void*)` cast on each slot, which is for pointer-element
// arrays only, never reaching the rebuilt string at struct offset 0. The array's
// element stride is `esz` bytes ({bz_string*, V} struct, normally 16 on 64-bit
// with 8-byte align). bz_string_release is null-safe per §8.6.
static void bz_lazy_release_str_dict_keys(void* arr, int64_t esz) {
    if (!arr || esz <= 0) return;
    int64_t n = bz_array_len(arr);
    for (int64_t i = 0; i < n; i++) {
        char* slot = (char*)bz_array_ptr_at(arr, i, esz);
        if (slot) bz_string_release(*(void**)slot);
    }
}

// #2544 (stdlib §4.8/§4.12; spec §29.1, v0.236): wrap a FRESH
// Dictionary<string,V> materialisation (bz_array_t* of {bz_string*, V} structs)
// as a lazy ARRAY source that OWNS the array AND each rebuilt string Key
// (own_arr := 1, arr_str_keys := 1 → bz_lazy_dispose walks the array and
// bz_string_release each Key at offset 0 before bz_array_free). Sibling of
// bz_lazy_from_owned_grouping_array (#1914). Used by the Calls3 LINQ source
// path (lowerToLazyLinqSource Dictionary case) when the dictionary is
// string-keyed. The Values are copied by value into the slot — NOT retrained
// copies — so they need no disposal here; the source dictionary still owns
// its values.
extern "C" void* bz_lazy_from_owned_str_dict_array(void* arr, int64_t esz) {
    auto* e = bz_lazy_new(BZ_LAZY_ARRAY, esz);
    e->arr = arr;
    e->own_arr = 1;
    e->arr_str_keys = 1;
    return e;
}

// #422 / §28.9 / §28.3: mark a freshly-built lazy enumerator as the OWNER of its
// operator closure, so bz_lazy_dispose releases it (frees the +1 inline-lambda
// cell + env once the pipeline is disposed). Codegen calls this immediately
// after a lazy-operator constructor ONLY when the closure argument is an inline
// lambda literal — never for a borrowed (variable-form) closure. Returns the
// enumerator so the call can be threaded inline at the lowering site. Null-safe.
extern "C" void* bz_lazy_own_closure(void* lazy) {
    if (lazy) ((bz_lazy_t*)lazy)->own_closure = 1;
    return lazy;
}

// stdlib §5 Ordering: ThenBy secondary-closure ownership. Mirrors
// bz_lazy_own_closure for the secondary sort key closure stored in closure2.
// Called by codegen immediately after bz_lazy_thenby when the secondary key
// argument was an inline lambda literal (own_closure2 := 1 → released at
// bz_lazy_dispose). Returns the enumerator for inline threading. Null-safe.
extern "C" void* bz_lazy_own_closure2(void* lazy) {
    if (lazy) ((bz_lazy_t*)lazy)->own_closure2 = 1;
    return lazy;
}

// issue #389 / stdlib §5 / §4.10: a dict-backed Set<T> as a lazy source. Snapshot
// the set's element keys (bz_dict_keys_gen returns malloc'd esz-byte blobs) into
// a fresh owned bz_array_t iterated by index, then free the snapshot pointers.
// The array is stored in `sorted` so bz_lazy_dispose frees it (the set itself is
// not owned by the enumerator). Set elements are value-semantic, so copying the
// raw key bytes into the snapshot array is the correct (and only) representation.
extern "C" void* bz_lazy_from_set(void* set, int64_t esz) {
    auto* e = bz_lazy_new(BZ_LAZY_SET, esz);
    e->sorted = bz_array_alloc(esz, 8);
    void** keys = nullptr; int64_t n = 0;
    bz_dict_keys_gen(set, &keys, &n);
    for (int64_t i = 0; i < n; i++) {
        bz_array_add(e->sorted, keys[i], esz);
        free(keys[i]);
    }
    free(keys);
    return e;
}

// #6380 (§28.3 K8 / §28.9 / stdlib §4.4–§4.11 / §4.17–§4.18, v0.338): mark the
// SOURCE this lazy enumerator wraps as OWNED, so bz_lazy_dispose reclaims it.
// Codegen calls this only for a source no name owns — an `owned`-returning
// producer call, or a fresh materializer call, used DIRECTLY as a LINQ receiver
// (the same population freeUnboundOwnedBorrowingArrayTemp gates on) — so the
// transfer is sound: nothing else holds the value, and the lazy's lifetime is
// exactly the enumeration that still reads it.
//
// `kind` names the source's backing (1 `bz_array_t*`, 2 `bz_llist_t*`,
// 3 `bz_dict_t*`) so dispose can pick the matching runtime free. A second call
// on an already-owned source is a no-op rather than an overwrite: two different
// frees of one temporary is exactly the double free this path must not create.
// Returns `lazy` so the caller can inline it into the wrap expression.
extern "C" void* bz_lazy_own_source(void* lazy, void* src, int32_t kind) {
    auto* e = (bz_lazy_t*)lazy;
    if (!e || !src) return lazy;
    if (kind < 1 || kind > 3) return lazy;
    if (e->own_src_kind) return lazy;
    e->owned_src = src;
    e->own_src_kind = kind;
    return lazy;
}

// per §4.6/§4.7 + §5 fix #450: wrap a bz_llist_t* as a lazy source that walks
extern "C" void* bz_lazy_from_llist(void* llist, int64_t esz) {
    auto* e = bz_lazy_new(BZ_LAZY_LLIST, esz);
    e->arr = llist;     // bz_llist_t*
    e->inner = nullptr; // current node, null = not yet started
    e->materialized = 0;// 0 = not yet advanced to head
    return e;
}

// issue #775 / §16.3: wrap a `Range<T>` value (the codegen { i64 start, i64 end,
// i64 step, i8 flags } struct) as a lazy IEnumerable<T> source so SelectMany and
// the receiver-position LINQ operators (Count/Where/Select/…) can enumerate it.
// `esz` is T's byte width; the i64 working value is truncated to its low esz
// bytes on store (little-endian), matching the for-in counted-loop semantics.
// flags bit0 = IsEndInclusive, bit1 = startOpen, bit2 = endOpen (§16.3 layout).
// Open-ended ranges have no bounded enumeration: the receiver lowering raises
// InvalidOperationException before calling this, and as a safe fallback for any
// other path the enumerator is marked empty (movenext yields nothing).
extern "C" void* bz_lazy_from_range(int64_t start, int64_t end, int64_t step,
                                    int8_t flags, int64_t esz) {
    auto* e = bz_lazy_new(BZ_LAZY_RANGE, esz);
    e->idx = start;       // current working value
    e->n = end;           // end bound
    e->up_esz = step;     // signed stride (§16.4; sign decides direction)
    e->phase = (int32_t)(uint8_t)flags;
    if (((uint8_t)flags & 0x6u) != 0) e->stopped = 1;  // open-ended → empty
    return e;
}

// ---------------------------------------------------------------------------
// §5 (#651): GroupBy — bz_grouping_t wraps one group's key + element array.
// bz_groupby_to_array(lazy, val_esz, key_esz, key_fn, closure) materializes
// the source pipeline and groups elements by key, using memcmp for key equality
// (value-type keys of known byte size).  Returns a bz_array_t* of
// bz_grouping_t* pointers (each pointer is one group's heap object).
//
// The key function has the same `bz_sel_fn` signature as Select's selector:
//   void key_fn(void* closure, const void* elem, void* key_out)
// It writes `key_esz` bytes of the computed key into `key_out`.
//
// bz_grouping_count(g): number of elements in group g.
// bz_grouping_key(g, out): copies the key bytes into `out`.
// bz_grouping_elements(g): returns the bz_array_t* for indexed access.
// bz_grouping_free(g): disposes the element array and frees the group object.
// ---------------------------------------------------------------------------
struct bz_grouping_t {
    void*    elements;  // bz_array_t*
    int64_t  key_esz;
    void*    key_data;  // malloc'd key_esz bytes
};

static bz_grouping_t* bz_grouping_alloc(int64_t val_esz, int64_t key_esz,
                                         const void* key) {
    auto* g = (bz_grouping_t*)calloc(1, sizeof(bz_grouping_t));
    g->elements = bz_array_alloc(val_esz, 4);
    g->key_esz  = key_esz;
    g->key_data = malloc((size_t)key_esz);
    memcpy(g->key_data, key, (size_t)key_esz);
    return g;
}

extern "C" int64_t bz_grouping_count(void* grp) {
    if (!grp) return 0;
    return bz_array_len(((bz_grouping_t*)grp)->elements);
}

// Copy the key bytes into `out` (caller allocates `key_esz` bytes).
extern "C" void bz_grouping_key(void* grp, void* out) {
    if (!grp || !out) return;
    auto* g = (bz_grouping_t*)grp;
    memcpy(out, g->key_data, (size_t)g->key_esz);
}

// Returns the bz_array_t* of elements so the caller can index into it.
extern "C" void* bz_grouping_elements(void* grp) {
    return grp ? ((bz_grouping_t*)grp)->elements : nullptr;
}

extern "C" void bz_grouping_free(void* grp) {
    if (!grp) return;
    auto* g = (bz_grouping_t*)grp;
    bz_array_free(g->elements, nullptr);
    free(g->key_data);
    free(g);
}

// bz_groupby_to_array: materialize a lazy source, group by key, return an
// Array<bz_grouping_t*>.  Caller owns the returned array and each grouping
// inside it; call bz_groupby_free_groups to release all groupings, then
// bz_array_free for the outer array.
extern "C" void* bz_groupby_to_array(void* lazy, int64_t val_esz,
                                      int64_t key_esz,
                                      void* closure, void* key_fn_ptr) {
    typedef void (*key_fn_t)(void*, const void*, void*);
    auto  key_fn = (key_fn_t)key_fn_ptr;
    auto* e = (bz_lazy_t*)lazy;
    // Outer array holds bz_grouping_t* pointers.
    void* groups = bz_array_alloc(sizeof(void*), 4);
    void* key_buf = malloc((size_t)key_esz);
    // Closure over the groups array to find/create a group by key.
    auto find_or_create = [&](const void* key) -> bz_grouping_t* {
        auto* garr = (bz_array_t*)groups;
        for (int64_t i = 0; i < garr->count; ++i) {
            void** slot = (void**)((char*)garr->data + i * sizeof(void*));
            auto* g = (bz_grouping_t*)(*slot);
            if (memcmp(g->key_data, key, (size_t)key_esz) == 0) return g;
        }
        auto* g = bz_grouping_alloc(val_esz, key_esz, key);
        bz_array_add(groups, &g, sizeof(void*));
        return g;
    };
    void** vt = (void**)e->vtable;
    typedef int32_t (*mn_t)(void*);
    typedef void*   (*cur_t)(void*);
    while (((mn_t)vt[2])(e)) {
        const void* elem = ((cur_t)vt[3])(e);
        key_fn(closure, elem, key_buf);
        bz_grouping_t* g = find_or_create(key_buf);
        bz_array_add(g->elements, elem, val_esz);
    }
    bz_lazy_dispose(e);
    free(key_buf);
    return groups;
}

// Release all groupings inside a groups array (produced by bz_groupby_to_array).
// After this call, the outer array itself must be freed with bz_array_free(...,null).
extern "C" void bz_groupby_free_groups(void* groups_arr) {
    auto* garr = (bz_array_t*)groups_arr;
    if (!garr) return;
    for (int64_t i = 0; i < garr->count; ++i) {
        void** slot = (void**)((char*)garr->data + i * sizeof(void*));
        bz_grouping_free(*slot);
    }
}

// #1930 / stdlib §5 / §28.9: release every grouping inside a groups array EXCEPT
// the one whose pointer equals `keep` — used when a bare `GroupBy(sel)` is drained
// by an element-RETURNING terminal (First/Single/Last/ElementAt/*OrNull/no-seed
// Aggregate) whose result IS one of the grouping pointers. The returned grouping
// (`keep`) must stay live (the caller now holds it — freeing it would dangle the
// result: use-after-free); its SIBLING groups are unreachable and are reclaimed
// here. After this call the outer array spine must be freed with
// bz_array_free(..., null) — the spine holds only the pointers, not the grouping
// objects, so freeing it does not touch the surviving `keep`. `keep` may be null
// (an *OrNull empty result): no grouping matches, so every grouping is freed
// (correct — nothing is returned to keep alive).
extern "C" void bz_groupby_free_groups_except(void* groups_arr, void* keep) {
    auto* garr = (bz_array_t*)groups_arr;
    if (!garr) return;
    for (int64_t i = 0; i < garr->count; ++i) {
        void** slot = (void**)((char*)garr->data + i * sizeof(void*));
        if (*slot != keep) bz_grouping_free(*slot);
    }
}

extern "C" void* bz_lazy_where(void* up, void* closure, void* pred) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_WHERE, u->esz);
    e->up = u;
    e->closure = closure;
    e->pred = (bz_pred_fn)pred;
    return e;
}

extern "C" void* bz_lazy_select(void* up, int64_t out_esz,
                                void* closure, void* sel) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_SELECT, out_esz);
    e->up = u;
    e->closure = closure;
    e->sel = (bz_sel_fn)sel;
    return e;
}

extern "C" void* bz_lazy_take(void* up, int64_t n) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_TAKE, u->esz);
    e->up = u;
    e->n = n;
    return e;
}

extern "C" void* bz_lazy_skip(void* up, int64_t n) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_SKIP, u->esz);
    e->up = u;
    e->n = n;
    return e;
}

extern "C" void* bz_lazy_distinct(void* up, void* eqfn) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_DISTINCT, u->esz);
    e->up = u;
    e->eqfn = (bz_eq_fn)eqfn;
    e->seen = bz_array_alloc(u->esz, 8);
    return e;
}

extern "C" void* bz_lazy_orderby(void* up, void* closure, void* keyfn,
                                 int32_t descending) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_ORDERBY, u->esz);
    e->up = u;
    e->closure = closure;
    e->keyfn = (bz_key_fn)keyfn;
    e->descending = descending;
    return e;
}

// stdlib §5 Combination: Concat yields all of `up`, then all of `down`. Both
// have the same element size (same T). Element bytes are copied through `cur`.
extern "C" void* bz_lazy_concat(void* up, void* down) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_CONCAT, u->esz);
    e->up = u;
    e->down = (bz_lazy_t*)down;
    e->phase = 0;
    return e;
}

// stdlib §5 Slicing: TakeWhile yields the prefix while `pred` holds, then ends
// the sequence permanently (later passing elements are NOT yielded).
extern "C" void* bz_lazy_takewhile(void* up, void* closure, void* pred) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_TAKEWHILE, u->esz);
    e->up = u;
    e->closure = closure;
    e->pred = (bz_pred_fn)pred;
    return e;
}

// stdlib §5 Slicing: SkipWhile drops the leading run satisfying `pred`, then
// yields the remainder verbatim (including elements that satisfy `pred`).
extern "C" void* bz_lazy_skipwhile(void* up, void* closure, void* pred) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_SKIPWHILE, u->esz);
    e->up = u;
    e->closure = closure;
    e->pred = (bz_pred_fn)pred;
    return e;
}

// stdlib §5 Projection: SelectMany flattens. `out_esz` is the inner element U's
// byte size. The selector trampoline returns a fresh inner lazy enumerator per
// upstream element; the runtime drains each before pulling the next upstream.
extern "C" void* bz_lazy_selectmany(void* up, int64_t out_esz,
                                    void* closure, void* sel) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_SELECTMANY, out_esz);
    e->up = u;
    e->closure = closure;
    e->flatsel = (bz_flatsel_fn)sel;
    e->inner = nullptr;
    return e;
}

// stdlib §5 Combination: Zip pairs `up` (left) and `down` (right) positionally,
// applying `comb` to produce one `out_esz`-byte output per pair. Ends when
// either source is exhausted (result length = min of the two).
extern "C" void* bz_lazy_zip(void* up, void* down, int64_t out_esz,
                             void* closure, void* comb) {
    auto* u = (bz_lazy_t*)up;
    auto* d = (bz_lazy_t*)down;
    auto* e = bz_lazy_new(BZ_LAZY_ZIP, out_esz);
    e->up = u;
    e->down = d;
    e->closure = closure;
    e->comb = (bz_comb_fn)comb;
    e->up_esz = u->esz;
    e->down_esz = d->esz;
    return e;
}

// stdlib §5 Set-like (lazy): Union/Intersect/Except with deduplication.
// `down` is the `other` IEnumerable<T> lazy source, pre-built by the caller.
// `hashfn` and `eqfn` are element-wise hash/equality trampolines (matching the
// generic-dict contract). At construction time, `down` is drained into `sorted`
// (the `other` membership dict) and disposed; `seen` is the dedup dict for `up`.
//
// Union:     yield unique elements from `up`, then elements from `down` not yet
//            seen. Phase 0 = draining `up` (insert into seen, yield if new).
//            Phase 1 = iterate other_keys snapshot and yield those not in seen.
// Intersect: yield unique elements from `up` that appear in the `other` dict.
// Except:    yield unique elements from `up` that do NOT appear in the `other` dict.
//
// Each needs a unique `seen` dict to prevent repeats on the self side. `sorted`
// holds the pre-built `other` dict (Intersect/Except) or the full other-dict
// (Union phase 1). `other_keys` / `other_n` are used by Union phase-1 iteration.
static void bz_lazy_setop_build_other(bz_lazy_t* e, bz_lazy_t* down) {
    // Materialize `down` into the `other` membership dict (`sorted`).
    e->sorted = bz_dict_alloc_generic(e->esz,
        (uint64_t(*)(const void*))e->hashfn,
        (bool(*)(const void*, const void*))e->eqfn);
    // Drain `down` into the other dict.
    const void* p;
    while ((p = bz_lazy_up_next(down)) != nullptr)
        bz_dict_set_gen(e->sorted, p, (void*)1);
    bz_lazy_dispose(down);
    // Build the `seen` dedup dict (same hash/eq).
    e->seen = bz_dict_alloc_generic(e->esz,
        (uint64_t(*)(const void*))e->hashfn,
        (bool(*)(const void*, const void*))e->eqfn);
}

extern "C" void* bz_lazy_union(void* up, void* down, void* hashfn, void* eqfn) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_UNION, u->esz);
    e->up = u;
    e->hashfn = (bz_hash_fn)hashfn;
    e->eqfn   = (bz_eq_fn)eqfn;
    bz_lazy_setop_build_other(e, (bz_lazy_t*)down);
    e->phase = 0;
    return e;
}

extern "C" void* bz_lazy_intersect(void* up, void* down, void* hashfn, void* eqfn) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_INTERSECT, u->esz);
    e->up = u;
    e->hashfn = (bz_hash_fn)hashfn;
    e->eqfn   = (bz_eq_fn)eqfn;
    bz_lazy_setop_build_other(e, (bz_lazy_t*)down);
    return e;
}

extern "C" void* bz_lazy_except(void* up, void* down, void* hashfn, void* eqfn) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_EXCEPT, u->esz);
    e->up = u;
    e->hashfn = (bz_hash_fn)hashfn;
    e->eqfn   = (bz_eq_fn)eqfn;
    bz_lazy_setop_build_other(e, (bz_lazy_t*)down);
    return e;
}

// stdlib §5 Ordering: ThenBy / ThenByDescending — secondary sort key applied
// to the already-sorted output of an upstream OrderBy. On first MoveNext,
// materializes all upstream elements and stable-sorts by (primary_key, secondary_key)
// composite, accessing the primary key by reaching into the upstream ORDERBY node.
extern "C" void* bz_lazy_thenby(void* up, void* closure2, void* keyfn2,
                                int32_t descending2) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_THENBY, u->esz);
    e->up = u;
    // Copy the primary key from the upstream ORDERBY/THENBY node so the
    // composite sort can apply it without re-visiting the chain structure.
    e->closure   = u->closure;    // primary closure (BORROW, not owned)
    e->keyfn     = u->keyfn;      // primary key trampoline
    e->descending = u->descending; // primary direction
    // Secondary key:
    e->closure2   = closure2;
    e->keyfn2     = (bz_key_fn)keyfn2;
    e->descending2 = descending2;
    return e;
}

// stdlib §5 Ordering: Reverse — materialize the upstream sequence, then yield
// elements backwards. Per §5: "Reverse(): IEnumerable<T>".
extern "C" void* bz_lazy_reverse(void* up) {
    auto* u = (bz_lazy_t*)up;
    auto* e = bz_lazy_new(BZ_LAZY_REVERSE, u->esz);
    e->up = u;
    return e;
}

// ORDERBY: drain upstream into a snapshot, then stable-sort by the int64 key.
static void bz_lazy_orderby_materialize(bz_lazy_t* e) {
    e->sorted = bz_array_alloc(e->esz, 8);
    const void* p;
    while ((p = bz_lazy_up_next(e->up)) != nullptr)
        bz_array_add(e->sorted, p, e->esz);
    auto* arr = (bz_array_t*)e->sorted;
    int64_t cnt = arr->count;
    // Stable insertion sort by int64 key (matches the spec's stable-ordering
    // guarantee for OrderBy; n is small in test scenarios).
    void* tmp = malloc((size_t)e->esz);
    for (int64_t i = 1; i < cnt; ++i) {
        memcpy(tmp, (char*)arr->data + i * e->esz, (size_t)e->esz);
        int64_t key_i = e->keyfn(e->closure, tmp);
        int64_t j = i - 1;
        while (j >= 0) {
            int64_t key_j = e->keyfn(e->closure, (char*)arr->data + j * e->esz);
            int shift = e->descending ? (key_j < key_i) : (key_j > key_i);
            if (!shift) break;
            memcpy((char*)arr->data + (j + 1) * e->esz,
                   (char*)arr->data + j * e->esz, (size_t)e->esz);
            --j;
        }
        memcpy((char*)arr->data + (j + 1) * e->esz, tmp, (size_t)e->esz);
    }
    free(tmp);
    e->idx = 0;
    e->materialized = 1;
}

// §16.3 (v0.183, #1586): the runtime-side mirror of codegen's
// emitCharRangeStep (LlvmBackend_Stmts.cpp) — used by BZ_LAZY_RANGE below
// when the enumerator was flagged char-stepping (phase bit3, set by
// lowerToLazyEnumerator / buildSelectManyTrampoline's rangeInner path). See
// emitCharRangeStep's comment for the dense-index-bijection derivation;
// `cur` is assumed already a valid (non-surrogate) char code point.
static int64_t bz_char_range_step(int64_t cur, int64_t step) {
    int64_t d = (cur >= 0xD800) ? (cur - 0x800) : cur;
    int64_t d2 = d + step;
    return (d2 >= 0xD800) ? (d2 + 0x800) : d2;
}

static int32_t bz_lazy_movenext(bz_lazy_t* e) {
    switch (e->kind) {
    case BZ_LAZY_ARRAY: {
        if (e->idx >= bz_array_len(e->arr)) return 0;
        memcpy(e->cur, bz_array_ptr_at(e->arr, e->idx, e->esz), (size_t)e->esz);
        e->idx++;
        return 1;
    }
    // issue #775 / §16.3: counted iteration of a Range<T>. The direction is the
    // sign of the step; inclusive/exclusive of the upper bound is phase bit0.
    // This mirrors the for-in counted-loop semantics (a zero step yields nothing
    // rather than looping forever; open-ended ranges were marked e->stopped).
    case BZ_LAZY_RANGE: {
        if (e->stopped) return 0;
        int64_t cur = e->idx, end = e->n, step = e->up_esz;
        if (step == 0) return 0;
        bool incl = (e->phase & 1) != 0;
        bool inRange = (step < 0) ? (incl ? cur >= end : cur > end)
                                  : (incl ? cur <= end : cur < end);
        if (!inRange) return 0;
        memcpy(e->cur, &cur, (size_t)e->esz);  // low esz bytes = truncation to T
        // §16.3 (v0.183, #1586): phase bit3 flags a Range<char> — step across
        // the UTF-16 surrogate gap instead of plain arithmetic.
        bool isChar = (e->phase & 0x8) != 0;
        e->idx = isChar ? bz_char_range_step(cur, step) : cur + step;
        return 1;
    }
    case BZ_LAZY_WHERE: {
        const void* p;
        while ((p = bz_lazy_up_next(e->up)) != nullptr) {
            if (e->pred(e->closure, p)) {
                memcpy(e->cur, p, (size_t)e->esz);
                return 1;
            }
        }
        return 0;
    }
    case BZ_LAZY_SELECT: {
        const void* p = bz_lazy_up_next(e->up);
        if (!p) return 0;
        e->sel(e->closure, p, e->cur);  // selector writes esz output bytes
        return 1;
    }
    case BZ_LAZY_TAKE: {
        if (e->idx >= e->n) return 0;
        const void* p = bz_lazy_up_next(e->up);
        if (!p) return 0;
        memcpy(e->cur, p, (size_t)e->esz);
        e->idx++;
        return 1;
    }
    case BZ_LAZY_SKIP: {
        if (!e->skipped) {
            const void* p;
            while (e->idx < e->n && (p = bz_lazy_up_next(e->up)) != nullptr)
                e->idx++;
            e->skipped = 1;
        }
        const void* p = bz_lazy_up_next(e->up);
        if (!p) return 0;
        memcpy(e->cur, p, (size_t)e->esz);
        return 1;
    }
    case BZ_LAZY_DISTINCT: {
        const void* p;
        auto* seen = (bz_array_t*)e->seen;
        while ((p = bz_lazy_up_next(e->up)) != nullptr) {
            int dup = 0;
            for (int64_t i = 0; i < seen->count; ++i) {
                const void* s = (char*)seen->data + i * e->esz;
                int same = e->eqfn ? e->eqfn(s, p)
                                   : (memcmp(s, p, (size_t)e->esz) == 0);
                if (same) { dup = 1; break; }
            }
            if (!dup) {
                bz_array_add(e->seen, p, e->esz);
                memcpy(e->cur, p, (size_t)e->esz);
                return 1;
            }
        }
        return 0;
    }
    case BZ_LAZY_ORDERBY: {
        if (!e->materialized) bz_lazy_orderby_materialize(e);
        auto* arr = (bz_array_t*)e->sorted;
        if (e->idx >= arr->count) return 0;
        memcpy(e->cur, bz_array_ptr_at(e->sorted, e->idx, e->esz), (size_t)e->esz);
        e->idx++;
        return 1;
    }
    case BZ_LAZY_SET: {
        // issue #389: iterate the owned key snapshot (built in bz_lazy_from_set).
        auto* arr = (bz_array_t*)e->sorted;
        if (e->idx >= arr->count) return 0;
        memcpy(e->cur, bz_array_ptr_at(e->sorted, e->idx, e->esz), (size_t)e->esz);
        e->idx++;
        return 1;
    }
    case BZ_LAZY_CONCAT: {
        // §5 Combination: drain `up` (phase 0), then `down` (phase 1).
        if (e->phase == 0) {
            const void* p = bz_lazy_up_next(e->up);
            if (p) { memcpy(e->cur, p, (size_t)e->esz); return 1; }
            e->phase = 1;  // fall through to the second source
        }
        const void* p = bz_lazy_up_next(e->down);
        if (!p) return 0;
        memcpy(e->cur, p, (size_t)e->esz);
        return 1;
    }
    case BZ_LAZY_TAKEWHILE: {
        // §5 Slicing: yield while the predicate holds; the first failure ends
        // the sequence (subsequent MoveNext calls return 0).
        if (e->stopped) return 0;
        const void* p = bz_lazy_up_next(e->up);
        if (!p) { e->stopped = 1; return 0; }
        if (!e->pred(e->closure, p)) { e->stopped = 1; return 0; }
        memcpy(e->cur, p, (size_t)e->esz);
        return 1;
    }
    case BZ_LAZY_SKIPWHILE: {
        // §5 Slicing: on first MoveNext, drop the leading run satisfying the
        // predicate and yield the first element that fails it; thereafter yield
        // every remaining element verbatim.
        if (!e->skipped) {
            const void* p;
            while ((p = bz_lazy_up_next(e->up)) != nullptr) {
                if (!e->pred(e->closure, p)) {
                    e->skipped = 1;
                    memcpy(e->cur, p, (size_t)e->esz);  // first non-skipped elem
                    return 1;
                }
            }
            e->skipped = 1;  // upstream exhausted entirely within the skip run
            return 0;
        }
        const void* p = bz_lazy_up_next(e->up);
        if (!p) return 0;
        memcpy(e->cur, p, (size_t)e->esz);
        return 1;
    }
    case BZ_LAZY_SELECTMANY: {
        // §5 Projection: pull from the current inner enumerator; when it is
        // exhausted, advance the upstream to obtain the next inner sequence.
        for (;;) {
            if (e->inner) {
                const void* p = bz_lazy_up_next(e->inner);
                if (p) { memcpy(e->cur, p, (size_t)e->esz); return 1; }
                bz_lazy_dispose(e->inner);  // inner drained — release it
                e->inner = nullptr;
            }
            const void* up_elem = bz_lazy_up_next(e->up);
            if (!up_elem) return 0;          // upstream exhausted
            e->inner = (bz_lazy_t*)e->flatsel(e->closure, up_elem);
            if (!e->inner) return 0;          // defensive: null inner sequence
        }
    }
    case BZ_LAZY_ZIP: {
        // §5 Combination: advance both sources; stop at the shorter. Each source
        // owns its own `cur`, so read the left/right element pointers directly.
        const void* a = bz_lazy_up_next(e->up);
        if (!a) return 0;
        const void* b = bz_lazy_up_next(e->down);
        if (!b) return 0;
        e->comb(e->closure, a, b, e->cur);  // writes esz output bytes
        return 1;
    }
    case BZ_LAZY_UNION: {
        // §5 Set-like: yield unique elements from `up`, then unique elements from
        // `down` (those not already yielded from `up`).
        // Phase 0: drain `up`, yielding elements not yet in `seen` dict.
        if (e->phase == 0) {
            const void* p;
            while ((p = bz_lazy_up_next(e->up)) != nullptr) {
                if (!bz_dict_contains_gen(e->seen, p)) {
                    bz_dict_set_gen(e->seen, p, (void*)1);
                    memcpy(e->cur, p, (size_t)e->esz);
                    return 1;
                }
            }
            // `up` exhausted. Build the other_keys snapshot from `sorted` (other dict).
            bz_dict_keys_gen(e->sorted, &e->other_keys, &e->other_n);
            e->idx = 0;
            e->phase = 1;
        }
        // Phase 1: iterate other_keys snapshot, yield elements not in `seen`.
        while (e->idx < e->other_n) {
            const void* k = e->other_keys[e->idx++];
            if (!bz_dict_contains_gen(e->seen, k)) {
                bz_dict_set_gen(e->seen, k, (void*)1);
                memcpy(e->cur, k, (size_t)e->esz);
                return 1;
            }
        }
        return 0;
    }
    case BZ_LAZY_INTERSECT: {
        // §5 Set-like: yield unique elements from `up` that appear in `other` dict.
        const void* p;
        while ((p = bz_lazy_up_next(e->up)) != nullptr) {
            if (bz_dict_contains_gen(e->sorted, p) &&
                !bz_dict_contains_gen(e->seen, p)) {
                bz_dict_set_gen(e->seen, p, (void*)1);
                memcpy(e->cur, p, (size_t)e->esz);
                return 1;
            }
        }
        return 0;
    }
    case BZ_LAZY_EXCEPT: {
        // §5 Set-like: yield unique elements from `up` that are NOT in `other` dict.
        const void* p;
        while ((p = bz_lazy_up_next(e->up)) != nullptr) {
            if (!bz_dict_contains_gen(e->sorted, p) &&
                !bz_dict_contains_gen(e->seen, p)) {
                bz_dict_set_gen(e->seen, p, (void*)1);
                memcpy(e->cur, p, (size_t)e->esz);
                return 1;
            }
        }
        return 0;
    }
    case BZ_LAZY_THENBY: {
        // §5 Ordering: composite (primary, secondary) sort. On first MoveNext,
        // drain upstream, sort by (primary_key, secondary_key) stably, then
        // iterate from index 0.
        if (!e->materialized) {
            e->sorted = bz_array_alloc(e->esz, 8);
            const void* p;
            while ((p = bz_lazy_up_next(e->up)) != nullptr)
                bz_array_add(e->sorted, p, e->esz);
            auto* arr = (bz_array_t*)e->sorted;
            int64_t cnt = arr->count;
            void* tmp = malloc((size_t)e->esz);
            // Stable insertion sort by (primary_key, secondary_key) composite.
            for (int64_t i = 1; i < cnt; ++i) {
                memcpy(tmp, (char*)arr->data + i * e->esz, (size_t)e->esz);
                int64_t pk_i = e->keyfn  ? e->keyfn(e->closure, tmp)   : 0;
                int64_t sk_i = e->keyfn2 ? e->keyfn2(e->closure2, tmp) : 0;
                int64_t j = i - 1;
                while (j >= 0) {
                    void* jptr = (char*)arr->data + j * e->esz;
                    int64_t pk_j = e->keyfn  ? e->keyfn(e->closure, jptr)   : 0;
                    int64_t sk_j = e->keyfn2 ? e->keyfn2(e->closure2, jptr) : 0;
                    // Compare primary first, then secondary on ties.
                    int shift;
                    if (pk_j != pk_i) {
                        shift = e->descending ? (pk_j < pk_i) : (pk_j > pk_i);
                    } else {
                        shift = e->descending2 ? (sk_j < sk_i) : (sk_j > sk_i);
                    }
                    if (!shift) break;
                    memcpy((char*)arr->data + (j + 1) * e->esz, jptr, (size_t)e->esz);
                    --j;
                }
                memcpy((char*)arr->data + (j + 1) * e->esz, tmp, (size_t)e->esz);
            }
            free(tmp);
            e->idx = 0;
            e->materialized = 1;
        }
        auto* arr = (bz_array_t*)e->sorted;
        if (e->idx >= arr->count) return 0;
        memcpy(e->cur, bz_array_ptr_at(e->sorted, e->idx, e->esz), (size_t)e->esz);
        e->idx++;
        return 1;
    }
    case BZ_LAZY_REVERSE: {
        // §5 Ordering: materialize upstream, then yield in reverse order.
        if (!e->materialized) {
            e->sorted = bz_array_alloc(e->esz, 8);
            const void* p;
            while ((p = bz_lazy_up_next(e->up)) != nullptr)
                bz_array_add(e->sorted, p, e->esz);
            auto* arr = (bz_array_t*)e->sorted;
            e->idx = arr->count - 1;  // start from last element
            e->materialized = 1;
        }
        auto* arr = (bz_array_t*)e->sorted;
        if (e->idx < 0) return 0;
        memcpy(e->cur, bz_array_ptr_at(e->sorted, e->idx, e->esz), (size_t)e->esz);
        e->idx--;
        return 1;
    }
    case BZ_LAZY_LLIST: {
        // per §4.6/§4.7 + §5 fix #450: walk bz_llist_t nodes head-to-tail.
        // `arr` = bz_llist_t*; `inner` = current node (null = not yet started).
        // On the first call, initialize `inner` to the list's head.
        if (!e->materialized) {
            auto* l = (bz_llist_t*)e->arr;
            e->inner = l ? (bz_lazy_t*)l->head : nullptr;
            e->materialized = 1;
        }
        if (!e->inner) return 0;
        auto* node = (bz_llist_node_t*)e->inner;
        const void* vp = (const char*)node + sizeof(bz_llist_node_t);
        memcpy(e->cur, vp, (size_t)e->esz);
        e->inner = (bz_lazy_t*)node->next;
        return 1;
    }
    }
    return 0;
}

// Materialize a lazy enumerator into a fresh owned Array<T> (stdlib §5 ToArray,
// the eager materialization point). Consumes (drains + disposes) the pipeline.
extern "C" void* bz_lazy_to_array(void* lazy, int64_t esz) {
    auto* e = (bz_lazy_t*)lazy;
    void* arr = bz_array_alloc(esz, 8);
    void** vt = (void**)e->vtable;
    typedef int32_t (*mn_t)(void*);
    typedef void*   (*cur_t)(void*);
    while (((mn_t)vt[2])(e))
        bz_array_add(arr, ((cur_t)vt[3])(e), esz);
    bz_lazy_dispose(e);
    return arr;
}

// stdlib §5 ToSet: materialize a lazy enumerator into a fresh owned Set<T>.
// Set<T> is dict-backed (bz_dict_t); all elements use the generic-key path
// (hash/eq trampolines). Consumes (drains + disposes) the pipeline.
// `hashfn` and `eqfn` are the element-wise hash/equality trampolines.
extern "C" void* bz_lazy_to_set(void* lazy, int64_t esz, void* hashfn, void* eqfn) {
    auto* e = (bz_lazy_t*)lazy;
    void* set = bz_dict_alloc_generic(esz,
        (uint64_t(*)(const void*))hashfn,
        (bool(*)(const void*, const void*))eqfn);
    void** vt = (void**)e->vtable;
    typedef int32_t (*mn_t)(void*);
    typedef void*   (*cur_t)(void*);
    while (((mn_t)vt[2])(e))
        bz_dict_set_gen(set, ((cur_t)vt[3])(e), (void*)1);
    bz_lazy_dispose(e);
    return set;
}

// fix #2803 / §4.10 / §28.9: a CLASS or INTERFACE element takes the ARRAY-backed
// Set<T> path — the same shape `new Set<C>()` / `new OwningSet<C>()` builds
// (a `bz_array_t` scanned with §8.3 equality via `emitArrayBackedSetInsert`).
// The disposal site at LlvmBackend_Disposal.cpp:3375 frees an `OwningSet<T>`
// as `bz_array_free`; the dict-backed `bz_lazy_to_set` result disagreed with
// that contract and was freed by `bz_array_free` (or UAF'd on a subsequent
// `Equals`/`GetHashCode` read of the freed region — #2773's trampolines are
// the load-bearing reader). The dict path is preserved for every element
// type that the §4.10/§4.8 generic-key dict backing admits (record, primitive,
// enum, choice, string) — those go through `bz_lazy_to_set` and that path is
// unchanged. Codegen dispatches via `genericBackingType(collTy, 0, isSet=true)`
// returning null for class/interface elements (the `isSet` exclusion at
// `genericBackingType`'s line 405).
//
// Drain the lazy enumerator into a fresh `bz_array_t`. For each element,
// linear-scan the existing array using `eqfn` (the element equality trampoline
// built the same way Distinct/Union/Intersect/Except build theirs — §8.3
// for a class, `Equals` override or reference identity; §8.4 when an override
// is declared; §9.2 for an auto-hashable record; content equality for a
// string). On a duplicate, silently drop the new one (no disposer — `Set<T>`
// is the borrowing family, the element's source owns its +1). On a fresh
// element, append via `bz_array_add` (the array becomes the sole owner of
// the borrowed element's reference, the same contract `emitArrayBackedSet-
// Insert` honours). Returns the array as `void*`, cast to the `Set<T>` it
// shape-matches. Consumes (drains + disposes) the pipeline.
extern "C" void* bz_lazy_to_set_arraybacked(void* lazy, int64_t esz, void* eqfn) {
    auto* e = (bz_lazy_t*)lazy;
    void* set = bz_array_alloc(esz, 8);
    void** vt = (void**)e->vtable;
    typedef int32_t (*mn_t)(void*);
    typedef void*   (*cur_t)(void*);
    typedef int32_t (*eq_t)(const void*, const void*);
    eq_t eq = (eq_t)eqfn;
    while (((mn_t)vt[2])(e)) {
        void* item = ((cur_t)vt[3])(e);
        int64_t len = bz_array_len(set);
        bool found = false;
        for (int64_t i = 0; i < len; ++i) {
            void* existing = bz_array_ptr_at(set, i, esz);
            if (eq(existing, item)) { found = true; break; }
        }
        if (!found) bz_array_add(set, item, esz);
    }
    bz_lazy_dispose(e);
    return set;
}

// stdlib §5 ToDictionary<TKey>(key: (T)->TKey): materialize into a fresh
// Dictionary<TKey, T>. Each element T is stored as value; the key is extracted
// by the `keyfn` trampoline. Uses the generic-key dict path — EXCEPT when
// TKey is `string` (key_is_string != 0): a Dictionary<string,V>'s own
// ContainsKey/indexer/Remove codegen unconditionally dispatches to the
// dedicated string fast path (bz_dict_contains_str / bz_dict_try_get_str /
// ...), which requires the underlying bz_dict_t to be in STRING-KEY mode
// (key_size == 0, populated by bz_dict_set_str) — never the generic-key mode
// bz_dict_alloc_generic always produces. Building a `key_is_string` result via
// the generic path left every subsequent lookup unable to find any key (#1692:
// `xs.ToDictionary((x) => $"k{x}")` then raised KeyNotFoundException on every
// `d[...]`), independent of whether the extracted key BYTES were even correct.
// `key_esz` = byte size of TKey (unused in string mode); `val_esz` = byte size
// of T (same as esz). `hashfn` and `eqfn` are key-wise trampolines (unused in
// string mode — bz_dict_alloc()'s string path hashes/compares by content
// itself, like any other Dictionary<string,V>).
// The element T bytes are copied into a fresh heap block and stored as the value.
// fix #4693 / stdlib §5 (v0.303): ToDictionary raises ArgumentException on
// duplicate key. Before inserting each key-value pair, probe the dict for the
// key using bz_dict_contains_str / bz_dict_contains_gen. If the key is present,
// allocate an ArgumentException object (using the arg_exc_vtable passed from
// codegen) and store it in bz_exc so emitPendingCheck() in the generated code
// branches to the catch handler. The lazy enumerator is disposed and NULL is
// returned so the generated code's assign-to-dest-dict slot is not written.
extern "C" void* bz_lazy_to_dict(void* lazy, int64_t val_esz, int64_t key_esz,
                                 void* keyfn, void* closure,
                                 void* hashfn, void* eqfn, int64_t val_inline,
                                 void* valfn, void* valclosure,
                                 int64_t key_is_string,
                                 void* arg_exc_vtable) {
    auto* e = (bz_lazy_t*)lazy;
    // The dict is keyed by TKey (key_esz bytes) in generic mode, or by string
    // content in string mode. #738: the VALUE must use the same generic-dict
    // encoding the codegen Dictionary<TKey,T> read expects (encodeValue): a
    // SCALAR value (val_inline != 0 — int/enum/char/class-ref, ≤ 8 bytes) is
    // stored INLINE in the void* slot (the indexer read does ptrtoint, not a
    // deref); a STRUCT value (record/decimal) is heap-boxed. Previously every
    // value was heap-boxed, so an indexed read of a scalar value returned the
    // pointer bits reinterpreted as the value — garbage.
    void* dict = key_is_string
        ? bz_dict_alloc()
        : bz_dict_alloc_generic(key_esz,
              (uint64_t(*)(const void*))hashfn,
              (bool(*)(const void*, const void*))eqfn);
    void** vt = (void**)e->vtable;
    typedef int32_t (*mn_t)(void*);
    typedef void*   (*cur_t)(void*);
    typedef int64_t (*kfn_t)(void* cl, const void* elem);
    // We need to store the key in a local buffer (key_esz may be ≤8 for
    // primitive types — just use a stack buffer of max 8 bytes for the
    // common case, or alloca for larger keys). Since key_esz is always a
    // primitive or record and ≤ sizeof(void*) in most practical cases, a
    // fixed 64-byte scratch covers all key types.
    char key_buf[64] = {};
    while (((mn_t)vt[2])(e)) {
        void* elem = ((cur_t)vt[3])(e);
        // Extract the key. The keyfn trampoline (kind 2) always returns an
        // int64: for a string TKey it is ptrtoint of the selector's returned
        // bz_string*; for a scalar TKey it is the (possibly sign-extended)
        // scalar key value.
        int64_t k = ((kfn_t)keyfn)(closure, elem);
        // fix #4693 / stdlib §5: check for duplicate key BEFORE insertion.
        if (key_is_string) {
            auto* s = (bz_string_struct*)(intptr_t)k;
            if (s) {
                const char* kd = (const char*)s->data;
                int64_t kl = s->len;
                int8_t  kw = s->width;
                if (bz_dict_contains_str(dict, kd, kl, kw)) {
                    // fix #4693 / #5421: the partially-built dict must be freed
                    // before throwing. The assignment to `d` never completes (the
                    // pending-check branches to catch before the result is stored),
                    // so the dict is unreachable once we return.
                    bz_dict_free(dict, nullptr);
                    if (arg_exc_vtable) {
                        void* exc = calloc(1, (size_t)BZ_EXC_OBJ_SIZE);
                        if (exc) {
                            *(void**)exc = arg_exc_vtable;
                            bz_exc = exc;  // emitPendingCheck() branches to catch
                        }
                    }
                    bz_lazy_dispose(e);
                    // fix #5421: release the key/value closures. On the normal
                    // (non-throw) path the caller's codegen releases them after
                    // bz_lazy_to_dict returns; on this throw path we never return
                    // to the caller so those releases are skipped. The closures are
                    // inline-lambda +1 refs (owned), so bz_closure_release is safe.
                    bz_closure_release(closure);
                    bz_closure_release(valclosure);
                    return nullptr;
                }
                // Not a duplicate: pack key bytes for the set call below.
                memcpy(key_buf, &k, (size_t)(key_esz < 8 ? key_esz : 8));
            }
        } else {
            memcpy(key_buf, &k, (size_t)(key_esz < 8 ? key_esz : 8));
            if (bz_dict_contains_gen(dict, key_buf)) {
                // fix #4693 / #5421: the partially-built dict must be freed
                // before throwing. Same rationale as the string-key path above.
                bz_dict_free(dict, nullptr);
                if (arg_exc_vtable) {
                    void* exc = calloc(1, (size_t)BZ_EXC_OBJ_SIZE);
                    if (exc) {
                        *(void**)exc = arg_exc_vtable;
                        bz_exc = exc;  // emitPendingCheck() branches to catch
                    }
                }
                bz_lazy_dispose(e);
                // fix #5421: release the key/value closures (same rationale as
                // the string-key path above).
                bz_closure_release(closure);
                bz_closure_release(valclosure);
                return nullptr;
            }
        }
        void* val;
        if (valfn) {
            // Two-selector ToDictionary(key, value): apply the value selector to
            // the element. The value trampoline (kind 2) returns the scalar
            // TValue widened to i64; pack its low val_esz bytes inline (a value
            // selector always produces a scalar value in this milestone, so
            // val_inline is 1). (§5 / #747 R43)
            int64_t vv = ((kfn_t)valfn)(valclosure, elem);
            val = nullptr;
            memcpy(&val, &vv, (size_t)(val_esz < 8 ? val_esz : 8));
        } else if (val_inline) {
            // Scalar: pack the value bytes directly into the void* slot.
            val = nullptr;
            memcpy(&val, elem, (size_t)(val_esz < 8 ? val_esz : 8));
        } else {
            // Struct (record/decimal): heap copy of the T element bytes.
            val = malloc((size_t)val_esz);
            memcpy(val, elem, (size_t)val_esz);
        }
        if (key_is_string) {
            auto* s = (bz_string_struct*)(intptr_t)k;
            const char* kd = s ? (const char*)s->data : "";
            int64_t kl = s ? s->len : 0;
            int8_t  kw = s ? s->width : (int8_t)1;   // fix #4499
            bz_dict_set_str(dict, kd, kl, kw, val);
        } else {
            bz_dict_set_gen(dict, key_buf, val);
        }
    }
    bz_lazy_dispose(e);
    return dict;
}

// stdlib §5 FirstOrNull / LastOrNull / SingleOrNull: eager terminals returning
// a nullable T (via a {hasValue:i8, value:T} tagged struct for value types, or
// a null pointer sentinel for class types). Since these lower into LLVM IR
// inline (not via a C call), we provide runtime helpers only for the
// lazy-source path where we need to drain the enumerator.
//
// For the lazy-source path, the codegen calls:
//   bz_lazy_first_or_null(lazy, esz, out) → 1 if found (copies into *out), 0 if empty
//   bz_lazy_last_or_null(lazy, esz, out)  → 1 if found (copies into *out), 0 if empty
//   bz_lazy_single_or_null(lazy, esz, out)→ 1 if exactly one (copies into *out),
//                                           0 if empty or more than one
// These drain and dispose the enumerator.
extern "C" int32_t bz_lazy_first_or_null(void* lazy, int64_t esz, void* out) {
    auto* e = (bz_lazy_t*)lazy;
    void** vt = (void**)e->vtable;
    typedef int32_t (*mn_t)(void*);
    typedef void*   (*cur_t)(void*);
    int32_t found = 0;
    if (((mn_t)vt[2])(e)) {
        memcpy(out, ((cur_t)vt[3])(e), (size_t)esz);
        found = 1;
    }
    bz_lazy_dispose(e);
    return found;
}

extern "C" int32_t bz_lazy_last_or_null(void* lazy, int64_t esz, void* out) {
    auto* e = (bz_lazy_t*)lazy;
    void** vt = (void**)e->vtable;
    typedef int32_t (*mn_t)(void*);
    typedef void*   (*cur_t)(void*);
    int32_t found = 0;
    while (((mn_t)vt[2])(e)) {
        memcpy(out, ((cur_t)vt[3])(e), (size_t)esz);
        found = 1;
    }
    bz_lazy_dispose(e);
    return found;
}

extern "C" int32_t bz_lazy_single_or_null(void* lazy, int64_t esz, void* out) {
    auto* e = (bz_lazy_t*)lazy;
    void** vt = (void**)e->vtable;
    typedef int32_t (*mn_t)(void*);
    typedef void*   (*cur_t)(void*);
    int32_t found = 0;
    if (((mn_t)vt[2])(e)) {
        memcpy(out, ((cur_t)vt[3])(e), (size_t)esz);
        found = 1;
        if (((mn_t)vt[2])(e)) found = 2;  // more than one: signal failure
    }
    bz_lazy_dispose(e);
    return (found == 1) ? 1 : 0;
}

// issue #389 / stdlib §4.10: drain a lazy enumerator into the bz_dict_keys_gen
// snapshot shape — a malloc'd array of `n` pointers, each a malloc'd esz-byte
// copy of one element. Used to feed a non-Set IEnumerable<T> `other` into
// bz_set_algebra_gen. Disposes the drained enumerator. First materialize into a
// growable array (count unknown up front), then repackage as pointer-per-element.
extern "C" void bz_set_collect_enumerator(void* lazy, int64_t esz,
                                          void*** keys_out, int64_t* n_out) {
    auto* e = (bz_lazy_t*)lazy;
    auto* tmp = (bz_array_t*)bz_array_alloc(esz, 8);
    void** vt = (void**)e->vtable;
    typedef int32_t (*mn_t)(void*);
    typedef void*   (*cur_t)(void*);
    while (((mn_t)vt[2])(e))
        bz_array_add(tmp, ((cur_t)vt[3])(e), esz);
    bz_lazy_dispose(e);
    int64_t n = tmp->count;
    auto** keys = (void**)malloc((size_t)(n > 0 ? n : 1) * sizeof(void*));
    for (int64_t i = 0; i < n; i++) {
        void* kc = malloc((size_t)esz);
        memcpy(kc, (char*)tmp->data + i * esz, (size_t)esz);
        keys[i] = kc;
    }
    bz_array_free(tmp, nullptr);
    if (keys_out) *keys_out = keys; else free(keys);
    if (n_out)    *n_out    = n;
}

// ---------------------------------------------------------------------------
// Buffer<T> — fixed-capacity compiler-intrinsic primitive (issue #545).
//
// Layout: 16-byte header {int64 len, int64 elem_size} followed immediately by
// the zero-initialised payload (calloc'd as one contiguous allocation).
// bz_buffer_get_ptr: pointer arithmetic on the payload region (no bounds check
// — caller emits §16 bounds check before calling this).
// bz_buffer_free: plain free (value elements carry no ownership obligation).
// ---------------------------------------------------------------------------

struct bz_buffer_hdr_t {
    int64_t len;
    int64_t elem_size;
    // payload follows immediately in the same calloc block.
};

extern "C" {

// bz_buffer_new — allocate a zero-initialised Buffer<T>.
// The allocation is: sizeof(bz_buffer_hdr_t) + len * elem_size bytes,
// all zeroed by calloc.
void* bz_buffer_new(int64_t len, int64_t elem_size) {
    if (len < 0) len = 0;
    if (elem_size <= 0) elem_size = 1;
    // Overflow guard: use calloc for both zero-init and implicit overflow
    // detection.  calloc(nmemb, size) multiplies safely on POSIX; the header
    // fits in one extra elem_size-aligned block — allocate header + payload
    // as a byte array of total_bytes.
    int64_t payload = len * elem_size;      // len >= 0, elem_size >= 1
    int64_t total   = (int64_t)sizeof(bz_buffer_hdr_t) + payload;
    auto* hdr = static_cast<bz_buffer_hdr_t*>(calloc(1, (size_t)total));
    if (!hdr) return nullptr;
    hdr->len       = len;
    hdr->elem_size = elem_size;
    return hdr;
}

// bz_buffer_get_ptr — pointer to element at index idx inside the payload.
// Does NOT bounds-check; caller emits the §16 guard before calling.
void* bz_buffer_get_ptr(void* buf, int64_t idx) {
    if (!buf) return nullptr;
    auto* hdr = static_cast<bz_buffer_hdr_t*>(buf);
    uint8_t* payload = reinterpret_cast<uint8_t*>(hdr + 1);
    return payload + idx * hdr->elem_size;
}

// bz_buffer_free — null-safe free of the whole header+payload block.
void bz_buffer_free(void* buf) {
    if (!buf) return;
    free(buf);
}

} // extern "C" (Buffer<T>)
