// fix #3684 (§30.5 Pattern A / §14.4 / §4.14 OwningBuffer<T>, v0.287): a
// `Task<OwningBuffer<T>>` Pattern A result leaked its backing storage at
// task disposal. `Emitter::patternAResultCollectionDisposer`
// (src/codegen/LlvmBackend_Disposal.cpp) was routing OwningBuffer through
// `bz_buffer_free(ptr)` — a free fn that targets the `bz_buffer_hdr_t*`
// layout of `Buffer<T>` (single calloc'd block, header + inline payload,
// freed by plain `free(buf)`).
//
// OwningBuffer<T>'s real runtime storage is a `bz_array_t*` allocated via
// `bz_array_alloc_count_n` (see the new-OwningBuffer arm in
// LlvmBackend_Exprs2.cpp:5070-5096) — a separate HEADER calloc and a separate
// DATA calloc that `bz_buffer_free` freed exactly half of (the header, by
// accident — the same `free(ptr)` that fits `bz_buffer_hdr_t*` happens to
// also free the `bz_array_t` header since they overlap at offset 0, but
// `arr->data` is a separate pointer the runtime never touched). The
// using-var scope-end path (`registerUsingDisposal`'s `d.collection = true`
// arm) and the nested-collection disposer thunk
// (`nestedCollectionDisposerThunk` directly above) BOTH already routed
// OwningBuffer through `bz_array_free` — Pattern A result disposal was
// the only orphan.
//
// §30.5 "disposing the task disposes the result" is the producer-side
// contract this lane makes precise. §4.14 owns the storage primitive
// contract: a value-typed T gets plain value slots (no per-slot disposal —
// the destructor just releases the backing) and a class-typed T gets
// `T?`-shaped nullable slots with the destructor cascading through each
// non-null slot. The fix routes OwningBuffer through the standard
// `bz_array_free(ptr, elemDisposer)` arm with the per-element disposer
// derived from `elementPolicyFor(genericArgs[0], owningFamily=true)` — a
// null disposer for value T (per-element loop skipped), the class Dispose
// thunk for class T (the same wiring the `using var` collection disposal
// arm uses, so the two paths agree on every payload type by construction).
//
// The layout below runs under `runAotChecked` (LSan ON — `runAot`
// deliberately disables LSan via ASAN_OPTIONS=detect_leaks=0 for the
// negative-test suite), so each row reports the LSan finding the fix
// closes when absent. Pre-fix every row in this file produces a
// `runAotChecked: sanitizer fired (LSan at exit)` diagnostic; the
// OwningBuffer<int> row points the 16-byte data allocation at
// `bz_array_alloc_count_n`, the OwningBuffer<Widget> row points at the
// Widget allocations inside the unused init lambda (or the per-cell
// leak in the local slot when invoked through a non-null init), and
// the nested OwningBuffer<OwningArray<Widget>> row cascades. Post-fix
// every row PASSES, with the expected value returned.

#include "support/aot_support.hpp"   // runAotChecked — AOT harness with LSan
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
int run(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §30.5 / §4.14 / #3684: the issue's exact repro shape — `Task<OwningBuffer<int>>`
// produced by an async method, awaited, and discharged via a `using var` in the
// caller. Pre-fix the produced OwningBuffer<int>'s `bz_array_t*` backing
// (header + 16-byte data buffer for 4 ints) leaked at task teardown: LSan
// fires "Direct leak of 16 byte(s) in 1 object(s)" pointing at the
// `bz_array_alloc_count_n` call from `MakeBuffer`. Post-fix
// `patternAResultCollectionDisposer` returns a thunk that calls
// `bz_array_free(result, null)` for an OwningBuffer<int> (T is value-typed
// per §14.4, so the per-element loop is skipped), and the data allocation
// is freed along with the header in the same call. The `.Length` check
// pins the value, mirroring #1289 / #2279's role for their respective
// collections.
BEZEL_TEST(owning_buffer_task_pattern_a_disposal_3684, issue_repro_int_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method MakeBuffer(): Task<OwningBuffer<int>> "
        "    var buf := new OwningBuffer<int>(4); "
        "    buf[0] := 5; "
        "    return buf; "
        "  end MakeBuffer; "
        "  public static method Main(): int "
        "    using var t := C.MakeBuffer(); "
        "    t.Wait(); "
        "    return t.Result.Length; "  // 4
        "  end Main; "
        "end C;"), 4);
}

// §30.5 / §4.14 / #3684: class-typed T control. Per §4.14, OwningBuffer<classT>
// holds `T?` (nullable) slots and its destructor disposes every non-null
// slot. The Pattern A result path's per-element disposer must therefore
// match the `using var` scope-end path's — the class Dispose thunk
// (elementPolicyFor(classT, owningFamily=true).disposerThunk). Pre-fix
// this row leaks every non-null slot's referent (32 B / 2 Widgets here),
// because `bz_buffer_free` only `free()`s the header pointer and never
// visits `arr->data`; post-fix the per-element disposer is properly
// derived and the slots are freed before the backing storage. The first
// slot's `.V` is read AFTER the task teardown boundary would normally
// fire (we keep reading before discharging the using-var's scope end),
// which pins the value surviving teardown — proves both that the slot
// was reachable AND that no UAF precedes the indexed read.
BEZEL_TEST(owning_buffer_task_pattern_a_disposal_3684, owning_buffer_of_class_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Widget public V: int; public constructor(v: int) V := v; end constructor; end Widget; "
        "class C "
        "  public static async method MakeBuffer(): Task<OwningBuffer<Widget>> "
        "    var buf := new OwningBuffer<Widget>(2, lambda(i) => new Widget((i + 1) * 100)); "
        "    return buf; "
        "  end MakeBuffer; "
        "  public static method Main(): int "
        "    using var t := C.MakeBuffer(); "
        "    t.Wait(); "
        "    return t.Result[0]!.V; "  // 100
        "  end Main; "
        "end C;"), 100);
}

// §30.5 / §4.14 / #3684: nested control. The Pattern A disposer for an
// OwningBuffer<OwningArray<Widget>> must reach into each slot's
// bz_array_t* (allocated via `bz_array_alloc_count_n` for class-T arrays
// here per the OwningArray<Widget> path) AND through its per-element
// disposer for the Widgets held inside each inner array. Pre-fix the
// outer OwningBuffer leaks its own backing + both inner OwningArray
// backings + every Widget — three nested layers, all left dangling.
// Post-fix the recursive `elementPolicyFor(elem, owningFamily=true)` path
// reaches the nested-collection disposer thunk exactly as the
// `nestedCollectionDisposerThunk` (#888) does for an OwningArray<Widget>
// element of an outer OwningArray — the two paths agree.
//
// Note: OwningArray<int> is rejected by §14.4 / #1327 (no disposal
// obligation on value-typed elements), so this row uses
// OwningArray<Widget> (class-typed) instead — the same nested-row pattern
// the nested-collection thunk uses for its own coverage.
BEZEL_TEST(owning_buffer_task_pattern_a_disposal_3684, nested_owning_array_elements) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Widget public V: int; public constructor(v: int) V := v; end constructor; end Widget; "
        "class C "
        "  public static async method MakeBuffer(): Task<OwningBuffer<OwningArray<Widget>>> "
        "    var buf := new OwningBuffer<OwningArray<Widget>>(2, "
        "      lambda(i) => new OwningArray<Widget>()); "
        "    return buf; "
        "  end MakeBuffer; "
        "  public static method Main(): int "
        "    using var t := C.MakeBuffer(); "
        "    t.Wait(); "
        "    return t.Result.Length; "  // 2
        "  end Main; "
        "end C;"), 2);
}

// §30.5 / §4.14 / #3684: the row-1 amplification. Mirrors #2279's
// 200-iteration amplified-leak test shape — repeated produce/await cycles
// of a Task<OwningBuffer<int>>. Pre-fix LSan sees 200 × 16 = 3200 B
// leaked; post-fix LSan is silent. The loop body uses a t.Result.Length
// read each iteration to pin both that the buffer was reachable AND that
// the indexer-read's data buffer survives until the borrow ends
// (no premature free — i.e., the §28.3 lifetime rule still holds under
// the corrected bz_array_free disposal). The Main return is `total mod 256`
// to fit the byte-process exit-status slot harness conventions enforce:
// 800 = 4 × 200 = 200 × 4 mod 256 = 32, still a non-trivial pin (a
// regression that elided the per-iteration Length reads would return 0).
BEZEL_TEST(owning_buffer_task_pattern_a_disposal_3684, amplified_200x_issue_repro) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method MakeBuffer(): Task<OwningBuffer<int>> "
        "    var buf := new OwningBuffer<int>(4); "
        "    buf[0] := 5; "
        "    return buf; "
        "  end MakeBuffer; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      using var t := C.MakeBuffer(); "
        "      t.Wait(); "
        "      total := total + t.Result.Length; "
        "      i := i + 1; "
        "    end while; "
        "    return total mod 256; "  // 800 mod 256 = 32
        "  end Main; "
        "end C;"), 32);
}
