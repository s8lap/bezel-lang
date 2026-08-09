// Spec-derived regression test for issue #2970.
//
// §28.2 (disposal exactly once) / §28.9 (release discipline) / §8.1 (every
// class implicitly implements `IDisposable`), v0.240. stdlib §4.1 / §4.4 / §4.5
// (the borrowing + owning list interfaces and the concrete Array<T> /
// OwningArray<T>).
//
// THE RULE. §28.2 makes every Dispose() legal; §28.9 makes the runtime
// signature of a Dispose() on a built-in collection `free(its-backing)`. For a
// built-in collection VIEWED through a borrowing or owning list interface
// (IList<T> / IReadOnlyList<T> / IOwningList<T>), the runtime value IS the same
// bz_array_t* as the concrete collection — the interface head is array-backed
// per stdlib §4.3, and the indexer bridge (#401) already keys on the same fact.
//
// THE DEFECT. Before this fix the explicit-Dispose() chain
// (LlvmBackend_Calls2.cpp) keyed by `isCollectionTypeName` — the CONCRETE
// built-in heads — and the interface heads had no arm. The chain fell through
// to the generic vtable dispatch (emitDisposeAndReclaim → load word 0 as
// vtable → call slot 2). A `bz_array_t*` has no vtable at word 0 — word 0 is
// the data pointer — so the load+indirect call jumped into the array's
// PAYLOAD and the program SEGFAULTED.
//
// MEASURED on 001a661 (exit 139): `var e: IList<int> := new Array<int>(2);
// e.Dispose();` emitted:
//
//   mov (%rax),%rcx        ; load word 0 (data pointer — should be vtable)
//   call *0x10(%rcx)       ; jump into rcx[2] — into the data buffer
//
// — a wild jump into the heap. `bezelc` emitted no diagnostic. `timeout: the
// monitored command dumped core; exit=124`.
//
// THE FIX extends the disposal chain to the array-backed list interfaces
// (IList<T> / IReadOnlyList<T> / IOwningList<T>) — the same arms as
// #2967's collection chain, plus the disposer derived for the OWNING list
// interface per stdlib §4.2 (its elements are owned; disposal cascades). The
// polymorphic collection interfaces (IReadOnlyCollection<T> / ICollection<T> /
// IOwningCollection<T>) span incompatible runtime representations and need
// runtime discrimination — deferred; they remain a wild jump on this fix.
//
// WHY THESE ORACLES. A zero exit code proves nothing about disposal — every
// successful leg is run under generated-code AddressSanitizer (runAotChecked)
// so a wild jump / double-free is a located report rather than a silent
// pass. The IOwningList<T> leg additionally tracks a `Dtors` counter so a
// cascade that ran zero or two disposals is unambiguous: zero = no cascade
// (the disposer arg was missing or wrong), two = double-dispose (the
// disposer ran twice).

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
// §28.2 / §8.1: explicit `e.Dispose()` on a built-in collection viewed through
// its array-backed list interface. Each was a wild jump on 001a661; each is
// now free + null through `bz_array_free`.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_iface_head_dispose_2970,
           ilist_over_array_explicit_dispose_does_not_wild_jump_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var e: IList<int> := new Array<int>(2); e.Dispose(); "
                 "return 0;")),
        0);
}

BEZEL_TEST(codegen_iface_head_dispose_2970,
           ireadonlylist_over_array_explicit_dispose_does_not_wild_jump_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var e: IReadOnlyList<int> := new Array<int>(2); "
                 "e.Dispose(); return 0;")),
        0);
}

// The same two programs under generated-code ASan/LSan: the wild jump became
// an ASan SEGV on the bug commit; a fix that stopped disposing instead of
// disposing correctly would leak here. The pre-bug exit code on 001a661 was
// 124 (timeout); ASan would itself crash and produce a non-zero exit.
BEZEL_TEST(codegen_iface_head_dispose_2970,
           ilist_over_array_explicit_dispose_leak_free_under_asan_28_9) {
    BEZEL_EXPECT_EQ(
        runChecked(prog("var e: IList<int> := new Array<int>(2); "
                        "e.Dispose(); return 0;")),
        0);
}

BEZEL_TEST(codegen_iface_head_dispose_2970,
           ireadonlylist_over_array_explicit_dispose_leak_free_under_asan_28_9) {
    BEZEL_EXPECT_EQ(
        runChecked(prog("var e: IReadOnlyList<int> := new Array<int>(2); "
                        "e.Dispose(); return 0;")),
        0);
}

// ---------------------------------------------------------------------------
// §4.2 / §28.9 / §28.11: IOwningList<T> is the OWNING list interface (extends
// IDisposable). Its Dispose() cascades through its class elements (stdlib §4.2:
// "its Dispose() cascades through its elements"). The disposer arg to
// bz_array_free must be the class-element disposer — a missing disposer would
// leak every element; a wrong one would free the wrong object.
//
// The pattern below uses `move` so the IOwningList<T> binding OWNS the
// underlying OwningArray<T> (§28.8.1). A plain borrow (`var g := arr`) would
// also reach this codegen path but with `arr` still owning, so `g.Dispose()`
// would double-free against `arr`'s scope-exit auto-drop — a pre-existing
// alias double-free shape recorded on #2970 (NOT introduced by this fix; the
// concrete `OwningArray<T>` borrow path has the same property, per its
// `cannot call Dispose on a borrowed owning collection` sema rejection).
//
// `Dtors == 1` for ONE inserted Widget is the cascade oracle. Pre-fix the
// program SEGFAULTED (wild jump) so no count could be read. A fix that ran
// the ARRAY release but forgot the disposer would show `Dtors == 0`. A fix
// that ran the disposer twice would show `Dtors == 2` (and likely crash).
// // per §28.9
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_iface_head_dispose_2970,
           iowninglist_cascades_element_disposal_28_9) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var arr := new OwningArray<Widget>(1); "
            "arr.Add(new Widget()); "
            "var g: IOwningList<Widget> := move arr; "
            "g.Dispose(); "
            "if Widget.Dtors <> 1 then return 92; end; "
            "return 0;",
            "class Widget "
            "  public static Dtors: int := 0; "
            "  destructor Widget.Dtors := Widget.Dtors + 1; end destructor; "
            "end Widget; ")),
        0);
}

// ---------------------------------------------------------------------------
// GUARDS: the same source shapes the original arms already served — concrete
// Array<T>, OwningArray<T>, IList<T> through `using var` — must remain clean.
// Each was measured clean on 001a661 and is what localised the defect to the
// explicit-Dispose path; they must stay clean.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_iface_head_dispose_2970,
           concrete_array_explicit_dispose_unchanged_4_4) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; while i < 100 do "
            "var e := new Array<int>(1); e.Dispose(); i := i + 1; "
            "end; return 0;")),
        0);
}

BEZEL_TEST(codegen_iface_head_dispose_2970,
           concrete_owningarray_explicit_dispose_unchanged_4_5) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; while i < 50 do "
            "var e := new OwningArray<Widget>(1); "
            "e.Add(new Widget()); "
            "e.Dispose(); "
            "i := i + 1; "
            "end; "
            "if Widget.Dtors <> 50 then return 93; end; "
            "return 0;",
            "class Widget "
            "  public static Dtors: int := 0; "
            "  destructor Widget.Dtors := Widget.Dtors + 1; end destructor; "
            "end Widget; ")),
        0);
}

BEZEL_TEST(codegen_iface_head_dispose_2970,
           ilist_using_var_unchanged_4_1) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; while i < 100 do "
            "using var e: IList<int> := new Array<int>(1); "
            "i := i + 1; "
            "end; return 0;")),
        0);
}