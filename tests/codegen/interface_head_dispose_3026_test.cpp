// Spec-derived regression test for issue #3026.
//
// §28.2 (disposal exactly once) / §28.9 (release discipline) / §8.1 (every
// class implicitly implements `IDisposable`), v0.240. stdlib §4.1 / §4.6-§4.9
// (the POLYMORPHIC collection interfaces IReadOnlyCollection<T> / ICollection<T> /
// IOwningCollection<T> — implemented by collections with INCOMPATIBLE runtime
// representations: bz_array_t* / bz_llist_t* / bz_dict_t*).
//
// THE RULE. §28.2 makes every Dispose() legal; §28.9 makes the runtime
// signature of a Dispose() on a built-in collection `free(its-backing)`. For a
// built-in collection VIEWED through a POLYMORPHIC collection interface, the
// static type erases which concrete backing is held — and a single emit call
// (e.g. `bz_array_free`) is unsound because the actual struct may be
// `bz_llist_t` or `bz_dict_t` instead of `bz_array_t`.
//
// THE DEFECT. Before this fix the explicit-Dispose() chain
// (LlvmBackend_Calls2.cpp) keyed on `isCollectionTypeName` and
// `isListInterfaceName` (the ARRAY-BACKED list interfaces #2970 fixed) — and
// the polymorphic collection interfaces had no arm. The chain fell through to
// the generic vtable dispatch (emitDisposeAndReclaim → load word 0 as vtable →
// call slot 2). A `bz_array_t*` has no vtable at word 0 — word 0 is the data
// pointer — so the load+indirect call jumped into the array's PAYLOAD and the
// program SEGFAULTED.
//
// MEASURED on 671726f (exit 139): `var e: ICollection<int> := new Array<int>(2);
// e.Dispose();` emitted:
//
//   mov (%rax),%rcx        ; load word 0 (data pointer — should be vtable)
//   call *0x10(%rcx)       ; jump into rcx[2] — into the data buffer
//
// — a wild jump into the heap. `bezelc` emitted no diagnostic. Exit 139 (SIGSEGV).
//
// THE FIX adds a runtime kind tag to bz_array_t, bz_llist_t, and bz_dict_t
// (each struct gains a trailing `int64_t kind` field; existing offsets
// preserved), a runtime helper `bz_runtime_dispose(p, dispose_elem)` that
// reads the kind and dispatches to the matching `bz_array_free` /
// `bz_llist_free` / `bz_dict_free`, and a new arm in the dispose chain that
// uses it for the three polymorphic collection interfaces. The disposer
// argument comes from the static element type T (matching the static-only
// handling `isCollectionTypeName` already uses for the concrete Array / LinkedList
// / Dictionary arms); for the borrowing interfaces the disposer is null, for
// IOwningCollection<T> it is the class-element disposer the static type
// yields — the same pattern the OWNING list-interface / OWNING-array arms use
// (it is sound because every concrete implementing collection — Array<T>,
// OwningArray<T>, LinkedList<T>, OwningLinkedList<T>, Set<T>, OwningSet<T>,
// Dictionary<K,V>, SortedDictionary<K,V> — keys its free on `dispose_elem` the
// same way bz_*_free already accepts it).
//
// WHY THESE ORACLES. A zero exit code proves nothing about disposal — every
// successful leg is run under generated-code AddressSanitizer (runAotChecked)
// so a wild jump / double-free / leak is a located report rather than a silent
// pass. Each test exercises a different backing-×-interface cell so a fix that
// special-cases one cell cannot pass the whole suite. The IOwningCollection<T>
// leg additionally tracks a `Dtors` counter so a cascade that ran zero or two
// disposals is unambiguous.

#include "support/aot_support.hpp"   // runAot — AOT test harness
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

std::string prog(const std::string& mainBody,
                 const std::string& prelude = "") {
    return "module M; " + prelude +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// §28.2 / §8.1 / stdlib §4.1: explicit `e.Dispose()` on a built-in collection
// VIEWED THROUGH EACH POLYMORPHIC COLLECTION INTERFACE × EACH BACKING. Each cell
// was a wild jump on 671726f; each is now free + null through bz_runtime_dispose.
// ---------------------------------------------------------------------------

// ARRAY × ICollection<T>
BEZEL_TEST(codegen_iface_head_dispose_3026,
           array_icollection_explicit_dispose_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var e: ICollection<int> := new Array<int>(2); "
                 "e.Dispose(); return 0;")),
        0);
}

// ARRAY × IReadOnlyCollection<T>
BEZEL_TEST(codegen_iface_head_dispose_3026,
           array_ireadonlycollection_explicit_dispose_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var e: IReadOnlyCollection<int> := new Array<int>(2); "
                 "e.Dispose(); return 0;")),
        0);
}

// OWNING ARRAY × IOwningCollection<T> (where T: class) — must cascade
BEZEL_TEST(codegen_iface_head_dispose_3026,
           owningarray_iowningcollection_cascades_disposal_28_9) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var arr := new OwningArray<Widget>(1); "
            "arr.Add(new Widget()); "
            "var e: IOwningCollection<Widget> := move arr; "
            "e.Dispose(); "
            "if Widget.Dtors <> 1 then return 92; end; "
            "return 0;",
            "class Widget "
            "  public static Dtors: int := 0; "
            "  destructor Widget.Dtors := Widget.Dtors + 1; end destructor; "
            "end Widget; ")),
        0);
}

// LINKED LIST × ICollection<T> — bz_llist_t* path (different kind tag)
BEZEL_TEST(codegen_iface_head_dispose_3026,
           linkedlist_icollection_explicit_dispose_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var e: ICollection<int> := new LinkedList<int>(); "
                 "e.Dispose(); return 0;")),
        0);
}

// OWNING LINKED LIST × IOwningCollection<T> — must cascade through bz_llist_free
BEZEL_TEST(codegen_iface_head_dispose_3026,
           owninglinkedlist_iowningcollection_cascades_disposal_28_9) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var lst := new OwningLinkedList<Widget>(); "
            "lst.AddLast(new Widget()); "
            "var e: IOwningCollection<Widget> := move lst; "
            "e.Dispose(); "
            "if Widget.Dtors <> 1 then return 93; end; "
            "return 0;",
            "class Widget "
            "  public static Dtors: int := 0; "
            "  destructor Widget.Dtors := Widget.Dtors + 1; end destructor; "
            "end Widget; ")),
        0);
}

// SET × ICollection<T> — class element → bz_array_t-backed; primitive
// element → bz_dict_t-backed. Both must dispose correctly via the kind tag.
// Set<int> (primitive element) → bz_dict_t* path.
BEZEL_TEST(codegen_iface_head_dispose_3026,
           setint_icollection_explicit_dispose_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var e: ICollection<int> := new Set<int>(); "
                 "e.Dispose(); return 0;")),
        0);
}

// Set<Widget> (class element) → bz_array_t-backed. Still ICollection<T>.
BEZEL_TEST(codegen_iface_head_dispose_3026,
           setclass_icollection_explicit_dispose_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var e: ICollection<Widget> := new Set<Widget>(); "
                 "e.Dispose(); return 0;",
                 "class Widget end Widget; ")),
        0);
}

// OWNING SET × IOwningCollection<T> — must cascade through bz_dict_free
BEZEL_TEST(codegen_iface_head_dispose_3026,
           owningset_iowningcollection_cascades_disposal_28_9) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var s := new OwningSet<Widget>(); "
            "s.Add(new Widget()); "
            "var e: IOwningCollection<Widget> := move s; "
            "e.Dispose(); "
            "if Widget.Dtors <> 1 then return 94; end; "
            "return 0;",
            "class Widget "
            "  public static Dtors: int := 0; "
            "  destructor Widget.Dtors := Widget.Dtors + 1; end destructor; "
            "end Widget; ")),
        0);
}

// STACK × IReadOnlyCollection<T> — Stack<T> only implements IReadOnlyCollection
BEZEL_TEST(codegen_iface_head_dispose_3026,
           stack_ireadonlycollection_explicit_dispose_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var e: IReadOnlyCollection<int> := new Stack<int>(); "
                 "e.Dispose(); return 0;")),
        0);
}

// QUEUE × IReadOnlyCollection<T>
BEZEL_TEST(codegen_iface_head_dispose_3026,
           queue_ireadonlycollection_explicit_dispose_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var e: IReadOnlyCollection<int> := new Queue<int>(); "
                 "e.Dispose(); return 0;")),
        0);
}

// ---------------------------------------------------------------------------
// §28.2 / §28.9 GUARDS: the concrete built-in paths (Array/OwningArray/
// LinkedList/OwningLinkedList/Dictionary/Set/OwningSet/Stack/Queue) already
// dispose correctly on 671726f — they must remain clean. Each was measured
// clean on 671726f and is what localised the prior defect (#2970 + #2921) to
// the polymorphic-interface path; they must stay clean so the new fix's arm
// does not silently regress working code.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_iface_head_dispose_3026,
           concrete_array_explicit_dispose_unchanged_4_4) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; while i < 100 do "
            "var e := new Array<int>(1); e.Dispose(); i := i + 1; "
            "end; return 0;")),
        0);
}

BEZEL_TEST(codegen_iface_head_dispose_3026,
           concrete_owningarray_explicit_dispose_unchanged_4_5) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; while i < 50 do "
            "var e := new OwningArray<Widget>(1); "
            "e.Add(new Widget()); "
            "e.Dispose(); "
            "i := i + 1; "
            "end; "
            "if Widget.Dtors <> 50 then return 95; end; "
            "return 0;",
            "class Widget "
            "  public static Dtors: int := 0; "
            "  destructor Widget.Dtors := Widget.Dtors + 1; end destructor; "
            "end Widget; ")),
        0);
}

BEZEL_TEST(codegen_iface_head_dispose_3026,
           concrete_linkedlist_explicit_dispose_unchanged_4_6) {
    BEZEL_EXPECT_EQ(
        run(prog("var e := new LinkedList<int>(); e.Dispose(); return 0;")),
        0);
}

BEZEL_TEST(codegen_iface_head_dispose_3026,
           concrete_owninglinkedlist_explicit_dispose_unchanged_4_7) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var lst := new OwningLinkedList<Widget>(); "
            "lst.AddLast(new Widget()); "
            "lst.Dispose(); "
            "if Widget.Dtors <> 1 then return 96; end; "
            "return 0;",
            "class Widget "
            "  public static Dtors: int := 0; "
            "  destructor Widget.Dtors := Widget.Dtors + 1; end destructor; "
            "end Widget; ")),
        0);
}

BEZEL_TEST(codegen_iface_head_dispose_3026,
           concrete_set_explicit_dispose_unchanged_4_10) {
    BEZEL_EXPECT_EQ(
        run(prog("var e := new Set<int>(); e.Dispose(); return 0;")),
        0);
}

BEZEL_TEST(codegen_iface_head_dispose_3026,
           concrete_owningset_explicit_dispose_unchanged_4_11) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var s := new OwningSet<Widget>(); "
            "s.Add(new Widget()); "
            "s.Dispose(); "
            "if Widget.Dtors <> 1 then return 97; end; "
            "return 0;",
            "class Widget "
            "  public static Dtors: int := 0; "
            "  destructor Widget.Dtors := Widget.Dtors + 1; end destructor; "
            "end Widget; ")),
        0);
}

BEZEL_TEST(codegen_iface_head_dispose_3026,
           concrete_ilist_explicit_dispose_unchanged_4_1) {
    BEZEL_EXPECT_EQ(
        run(prog("var e: IList<int> := new Array<int>(2); "
                 "e.Dispose(); return 0;")),
        0);
}

BEZEL_TEST(codegen_iface_head_dispose_3026,
           ilist_using_var_unchanged_4_1) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; while i < 100 do "
            "using var e: IList<int> := new Array<int>(1); "
            "i := i + 1; "
            "end; return 0;")),
        0);
}

// GUARD for `?.Dispose()` chaining through the polymorphic interface — the
// short-circuit `null?` path. Pre-existing #2967 nulled the slot after dispose;
// must stay null.
BEZEL_TEST(codegen_iface_head_dispose_3026,
           icollection_using_var_unchanged_4_1) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; while i < 100 do "
            "using var e: ICollection<int> := new Array<int>(1); "
            "i := i + 1; "
            "end; return 0;")),
        0);
}