// Spec-derived regression test for issue #4221.
//
// §28.2 (`IDisposable` / disposal exactly once) / §28.9 (Runtime-managed
// types) / §8.1 (`Object` is the universal root), v0.298.
// stdlib §4.4 (`Array<T>`), §4.5 (`OwningArray<T>`), §4.6 (`LinkedList<T>`),
// §4.8 (`Dictionary<K, V>`), §4.10 (`Set<T>`/`OwningSet<T>`).
//
// THE DEFECT. `x?.Dispose()` (the null-conditional spelling) on a COLLECTION
// receiver (`Array<T>`, `Dictionary<K, V>`, `Set<T>`, `LinkedList<T>`, ...)
// is memory-unsafe. `fix #4209` intercepted the VTABLE-LESS BUILTIN receiver
// shape in `lowerNullCondCall`'s `?.Dispose()` arm (StringBuilder, Mutex,
// Stopwatch, ...) by routing it through `emitVtablelessBuiltinRelease`,
// gated on THAT function's own success. `isVtableLessBuiltinClassName` is
// ALSO true of Array/Dictionary/Set/LinkedList/..., but
// `emitVtablelessBuiltinRelease` has no arm for them, so #4209's guard fell
// through for a collection receiver to `explicitDisposeOwnedReceiverSlot` /
// `emitDynamicDispose(recvD)` — reading the receiver's word 0 as a Bezel
// vtable and indirect-calling slot 2. A collection's runtime struct carries
// no such vtable (word 0 is its own header data), so this reads/jumps
// through garbage. MEASURED on 6f46a73 (#4209 landed, this fix's parent):
// `var a := new Array<int>(2); a.Add(1); a?.Dispose();` —
// AddressSanitizer heap-buffer-overflow reading 8 bytes past the array's
// `bz_array_alloc` header (the array's own short header happens to hold a
// value near its allocation rather than exactly zero, so it manifests as an
// overflow rather than the pc=0 SEGV the vtable-less-builtin population
// produces — same root cause, different manifestation, per issue #4221).
//
// THE FIX ports the non-`?.` `.Dispose()` arm's FULL, type-first dispatch
// chain (the Set-family dict-backed check, `isLinkedListTypeName`,
// `isListInterfaceName`, `isCollectionInterfaceName`, `isCollectionTypeName`,
// `isDictTypeName`, THEN the vtable-less-builtin arm, THEN the general
// user-class fallback) into a shared `emitExplicitDisposeChain` function
// that both `lowerCallDispatch`'s `.Dispose()` arm and
// `lowerNullCondCall`'s `?.Dispose()` arm now call — so a collection
// receiver is intercepted by its own element-aware arm (`bz_array_free` /
// `bz_dict_free` / `bz_llist_free`, with the owning forms cascading
// disposal into their class elements per `elementPolicyFor`) and never
// reaches the generic vtable-slot dispatch at all.
//
// WHY THESE ORACLES. A zero/expected exit code is not evidence of correct
// disposal on its own (the crash can just as easily be a heap-buffer-
// overflow SIGABRT under ASan or a silent corruption without it) — so every
// crash-fix row asserts a DISTINCT non-zero exit value a crash cannot
// produce, and every disposal/leak/double-free claim is a loop-amplified
// ASan/LSan run (`runAotChecked`, generated-code sanitizer) rather than a
// one-shot check (a one-shot leak/double-free row can pass on both sides of
// a fix — LANE_RULES §3/§15).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& mainBody, const std::string& prelude = "") {
    return "module M; " + prelude +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// BORROWING COLLECTIONS — plain `var`, the core #4221 repro across the four
// named receivers, each with a distinct return value a crash cannot produce.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_nullcond_dispose_collections_4221,
           array_plain_var_nullcond_dispose_does_not_crash_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var a := new Array<int>(2); a.Add(1); a?.Dispose(); "
                 "return 41;")),
        41);
}

BEZEL_TEST(codegen_nullcond_dispose_collections_4221,
           dictionary_plain_var_nullcond_dispose_does_not_crash_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var d := new Dictionary<string, int>(); d.Add(\"k\", 1); "
                 "d?.Dispose(); return 42;")),
        42);
}

BEZEL_TEST(codegen_nullcond_dispose_collections_4221,
           set_plain_var_nullcond_dispose_does_not_crash_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var s := new Set<int>(); s.Add(1); s?.Dispose(); "
                 "return 43;")),
        43);
}

BEZEL_TEST(codegen_nullcond_dispose_collections_4221,
           linkedlist_plain_var_nullcond_dispose_does_not_crash_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var l := new LinkedList<int>(); l.AddLast(1); "
                 "l?.Dispose(); return 44;")),
        44);
}

BEZEL_TEST(codegen_nullcond_dispose_collections_4221,
           sortedset_plain_var_nullcond_dispose_does_not_crash_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var s := new SortedSet<int>(); s.Add(1); s?.Dispose(); "
                 "return 45;")),
        45);
}

// ---------------------------------------------------------------------------
// LIST-INTERFACE and COLLECTION-INTERFACE receivers — the two interface
// families the non-`?.` chain dispatches with a dedicated arm ahead of the
// vtable-less-builtin fallback (`isListInterfaceName` /
// `isCollectionInterfaceName`); a static type through the interface, backed
// by the same `bz_array_t*`/`bz_dict_t*` at runtime.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_nullcond_dispose_collections_4221,
           ilist_interface_nullcond_dispose_does_not_crash_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var x: IList<int> := new Array<int>(2); x.Add(1); "
                 "x?.Dispose(); return 46;")),
        46);
}

// NOTE: no `.Add` here (unlike the sibling `IList<int>` row above) — `.Add`
// through the POLYMORPHIC collection-interface heads (`ICollection<T>` /
// `IReadOnlyCollection<T>` / `IOwningCollection<T>` / `IOwningList<T>`) is a
// separate, pre-existing, unimplemented-in-codegen gap (stdlib §4.1-§4.3,
// issue #853 — measured while writing this test: `var x: ICollection<int>
// := new Array<int>(2); x.Add(1);` reports "not yet supported in codegen"
// at compile time) — unrelated to this fix. The receiver is declared
// directly with the interface type from a `new` expression (no separate
// concrete-typed local aliasing the same backing) so this row exercises
// only the `isCollectionInterfaceName` arm's `?.Dispose()` path, not #853
// or the aliasing double-free filed as a follow-up (see this lane's report).
BEZEL_TEST(codegen_nullcond_dispose_collections_4221,
           icollection_interface_nullcond_dispose_does_not_crash_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var x: ICollection<int> := new Array<int>(2); "
                 "x?.Dispose(); return 47;")),
        47);
}

// ---------------------------------------------------------------------------
// OWNING COLLECTION, NESTED — stdlib §4.5: `OwningArray<T>` owns its
// elements; disposal cascades. Distinct from every row above: those are all
// BORROWING collections, whose `Dispose()` frees only the backing storage.
// This proves the element-aware `elementPolicyFor` disposer chosen by the
// non-`?.` chain's `isCollectionTypeName` arm is reached through `?.` too —
// `U`'s destructor incrementing `U.Disposed` is the observable proof the
// ELEMENT was disposed, not just the array's own backing freed.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_nullcond_dispose_collections_4221,
           owningarray_of_disposable_nullcond_dispose_cascades_element_28_9) {
    BEZEL_EXPECT_EQ(
        run(prog("var oa := new OwningArray<U>(); oa.Add(new U()); "
                 "oa?.Dispose(); return U.Disposed;",
                 "class U public static Disposed: int := 0; "
                 "  public constructor() end constructor; "
                 "  destructor U.Disposed := U.Disposed + 1; end destructor; "
                 "end U; ")),
        1);
}

// ---------------------------------------------------------------------------
// §28.2 exactly-once, REPEATED explicit `?.Dispose()`: the second call must
// reload the now-null slot and skip (the `?.` null-guard itself does this
// once `nullExplicitlyDisposedBuiltinSlot` — the chain's single §28.2 exit —
// has run). A fix that freed the backing WITHOUT nulling the slot would
// double-free `bz_array_free` here. 200 iterations under ASan so a
// single-shot fluke cannot pass (LANE_RULES §3/§15: loop-amplify).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_nullcond_dispose_collections_4221,
           array_nullcond_dispose_repeated_is_noop_28_2) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; "
            "while i < 200 do "
            "  var a := new Array<int>(2); "
            "  a.Add(1); "
            "  a?.Dispose(); "
            "  a?.Dispose(); "
            "  i := i + 1; "
            "end; "
            "return 0;")),
        0);
}

// §28.2 exactly-once, EXPLICIT `?.Dispose()` THEN `using var` scope-exit
// auto-drop in the same scope: both reach the same receiver. A fix that
// released without nulling the slot turns the crash above into a DOUBLE
// FREE here instead (the auto-drop reloads a still-live pointer).
BEZEL_TEST(codegen_nullcond_dispose_collections_4221,
           array_nullcond_explicit_then_autodrop_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; "
            "while i < 200 do "
            "  using var a := new Array<int>(2); "
            "  a.Add(1); "
            "  a?.Dispose(); "
            "  i := i + 1; "
            "end; "
            "return 0;")),
        0);
}

// ---------------------------------------------------------------------------
// §28.9 under-release direction: LeakSanitizer over 200 iterations of the
// plain-`var` explicit-dispose shape — the array's backing storage must
// actually be freed, not merely made safe to skip on a second call.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_nullcond_dispose_collections_4221,
           array_nullcond_dispose_plain_var_is_leak_free_28_9) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; "
            "while i < 200 do "
            "  var a := new Array<int>(2); "
            "  a.Add(1); "
            "  a?.Dispose(); "
            "  i := i + 1; "
            "end; "
            "return 0;")),
        0);
}

// Owning-collection counterpart: element disposal must not leak either.
BEZEL_TEST(codegen_nullcond_dispose_collections_4221,
           owningarray_of_disposable_nullcond_dispose_is_leak_free_28_9) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; "
            "while i < 200 do "
            "  var oa := new OwningArray<U>(); "
            "  oa.Add(new U()); "
            "  oa?.Dispose(); "
            "  i := i + 1; "
            "end; "
            "return 0;",
            "class U "
            "  public constructor() end constructor; "
            "  destructor end destructor; "
            "end U; ")),
        0);
}

// ---------------------------------------------------------------------------
// §7.5 null-receiver short-circuit control: a genuinely null collection
// receiver remains a clean no-op — the `?.` guard itself, unaffected by
// this fix (it never reaches `emitExplicitDisposeChain` at all).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_nullcond_dispose_collections_4221,
           array_nullcond_dispose_null_receiver_is_noop_7_5) {
    BEZEL_EXPECT_EQ(
        run(prog("var a: Array<int>? := null; a?.Dispose(); return 48;")),
        48);
}
