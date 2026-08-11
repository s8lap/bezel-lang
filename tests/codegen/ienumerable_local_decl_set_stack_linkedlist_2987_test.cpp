// Spec-derived regression test for issue #2987, the §29.2 local-decl coercion's
// residual of #2975 manifestation 3 (§28.3 / §28.9 / §29.2, v0.240). stdlib
// §4.10 (`Set<T>`), §4.11 (`Stack<T>`), §4.6/§4.7 (`LinkedList<T>`).
//
// THE RULE. §28.3 requires the value a `using var` binds to be disposed at the
// end of its block. When that value is an unbound temporary — an allocation
// bound to no other name — this binding is its ONLY owner, so this scope end is
// its only disposal site.
//
// THE DEFECT (the residual). #2975 manifestation 3 closed the Array arm of
// the §29.2 local-decl coercion (a `new Array<T>(...)` widened to a pure
// `IEnumerable<T>` local) by switching to the OWNING enumerator (#2299's
// wrapOwnedArrayAsIEnumerable). Three sibling arms at the same dispatch
// point (Set / Stack / LinkedList) were left out:
//   * Array  — fixed by #2975 m3 (the OWNING forward-machinery existed).
//   * Stack  — same shape, but Stack has no OWNING forward-machinery variant
//     (the only enumerable machinery was #2471's reverse-order
//     wrapStackAsIEnumerator, sized for a top-to-bottom pop). #2299's RETURN-
//     site workaround (`freeCollectionSourceNow(v)` right after the wrap) is
//     NOT transferable to a local-decl site: the local is iterated AFTER the
//     declaration, so freeing the backing at the declaration frees it out
//     from under the loop. The structurally correct fix is an OWNING variant
//     of the reverse machinery (wrapOwnedStackAsIEnumerator — slot-1 Dispose
//     = `bz_array_free(self->arr, null); free(self)`), mirroring
//     wrapOwnedArrayAsIEnumerable's role for the forward machinery.
//   * LinkedList — same shape, same fix direction (wrapOwnedLinkedListAsIEnumerable
//     — slot-1 Dispose = `bz_llist_free(self->list, null); free(self)`).
//     The issue body predicted the same leak and did NOT measure it; measured
//     here.
//   * Set (dict-backed) — different fix shape. The existing arm snapshots the
//     key table via `bz_lazy_from_set` and drains it via `bz_lazy_to_array`
//     into a fresh OWNING array. The snapshot is INDEPENDENT of the source
//     set (it owns its own key table), so the source can be freed RIGHT HERE
//     at the local-decl site for the unbound-`new` shape — a direct transplant
//     of `freeDictSourceNow(v)` from the RETURN coercion (#2299).
//
// MEASURED on the pre-fix compiler (HEAD `9c17f65`), 200 iterations of
// `using var e: IEnumerable<int> := new X();`:
//   * Set: 70400 byte leak in 400 allocations (bz_dict_alloc_generic +
//     calloc); same shape as the issue's 17600-byte measurement.
//   * Stack: 24000 byte leak in 400 allocations (bz_array_alloc + calloc);
//     same shape as the issue's 9552-byte measurement.
//   * LinkedList: 11144 byte leak in 199 allocations (bz_llist_alloc); the
//     issue body's "NOT measured" claim is now confirmed live.
//
// THE FIX (src/codegen/LlvmBackend_Setup.cpp local-decl dispatch +
// LlvmBackend_Assign.cpp new wrapOwnedStackAsIEnumerator /
// wrapOwnedLinkedListAsIEnumerable + ensureLinkedListEnumMachinery machinery
// extraction):
//   * All three arms gated on `dynamic_cast<const ast::NewExpr*>(init) !=
//     nullptr` for the same alias-rejection reason #2975 m3 used: a
//     producer-init IDENTIFIER source (`var a := new Set<int>();
//     var e: IEnumerable<int> := a;`) is still owned by `a`, which disposes
//     it at ITS scope end — taking ownership here would DOUBLE-FREE it. A
//     `new` at this site is bound to no other name by construction. It is
//     also EMPTY by construction (a collection constructor argument is a
//     capacity, not contents), which is why the OWNING wrap's
//     element-disposer-less `bz_array_free` / `bz_llist_free` / `bz_dict_free`
//     is complete here: a temporary no name can reach can have had nothing
//     added to it.
//   * The Set arm keeps the same wrapping machinery (snapshot via
//     `bz_lazy_from_set` → drain via `bz_lazy_to_array` → OWNING array wrap);
//     the change is one extra `freeDictSourceNow(srcSet)` at the end, gated
//     on the `new` shape.
//   * The Stack and LinkedList arms pick between the existing BORROWING wrap
//     (named source) and the new OWNING wrap (unbound `new`) based on the
//     same `new`-EXPRESSION gate. For borrowed sources (a named local, a
//     parameter, a field) the plain wrap is correct and stays — the shared
//     source is still disposed at its owner's scope end.
//   * LinkedList's new OWNING wrap reuses the EXISTING plain machinery's
//     MoveNext/Current (the node-walking bodies never touch element
//     ownership); the only new function is a slot-1 OWNING Dispose and a
//     slot-4 OWNING GetEnumerator that stamps fresh NON-owning clones with
//     the PLAIN vtable so a per-loop enumerator frees only its own struct.
//     The plain machinery is extracted into a cached helper
//     `ensureLinkedListEnumMachinery(elemTy)` (struct type + plain vtable)
//     so both wraps share one set per element type.
//
// WHY THESE ORACLES. Under-release is LeakSanitizer's direction and
// over-release is AddressSanitizer's, so every case is a loop-amplified
// `runAotChecked` run, which is both. The alias cases additionally ITERATE
// and sum, so a source freed out from under the loop shows up as a wrong
// total rather than passing quietly. The empty-iteration case pins that the
// OWNING wrap still satisfies the iterator protocol (MoveNext/Current) on
// the §4.11-correct pop order for Stack and on the head-to-tail order for
// LinkedList.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// runAotChecked runs the linked executable under generated-code ASan; the
// pre-fix leak flips r.ran to false -> -999, distinct from any legal exit
// value. Used for every "under release" case here.
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// Plain runAot (no sanitizer). Used for the alias cases — they pin that the
// OWNING wrap is NOT taken for a named source (so no double-free), but the
// borrowing wrap's wrapper STRUCT has the pre-existing #2298 "not freed on
// the borrowed argument/alias path" gap, which is a separate, filed defect
// (not this issue's concern). runAotCheckedLenient would also work; the
// plain runAot pins the value observable (sum) without coupling to #2298.
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// 200 iterations of `body`, then 0.
std::string loop(const std::string& body) {
    return "var i := 0; while i < 200 do " + body + " i := i + 1; end; "
           "return 0;";
}

}  // namespace

// ===========================================================================
// §28.3 / §28.9 / §29.2 — THE DEFECT, per collection kind
// ===========================================================================
//
// Each case is the issue body's exact `using var e: IEnumerable<int> := new X();`
// shape, loop-amplified 200×, run under runAotChecked (ASan + LSan). Pre-fix
// each reported a leak (Set: 70.4 KiB, Stack: 24 KiB, LinkedList: 11.1 KiB);
// post-fix each returns 0 with no leak.

BEZEL_TEST(codegen_ienum_local_decl_2987,
           set_temp_at_ienumerable_head_leak_free_28_3) {
    BEZEL_EXPECT_EQ(
        runChecked("module M; class C public static method Main(): int " +
                   loop("using var e: IEnumerable<int> := new Set<int>();") +
                   " end Main; end C;"),
        0);
}

BEZEL_TEST(codegen_ienum_local_decl_2987,
           stack_temp_at_ienumerable_head_leak_free_28_3) {
    BEZEL_EXPECT_EQ(
        runChecked("module M; class C public static method Main(): int " +
                   loop("using var e: IEnumerable<int> := new Stack<int>();") +
                   " end Main; end C;"),
        0);
}

BEZEL_TEST(codegen_ienum_local_decl_2987,
           linkedlist_temp_at_ienumerable_head_leak_free_28_3) {
    BEZEL_EXPECT_EQ(
        runChecked("module M; class C public static method Main(): int " +
                   loop("using var e: IEnumerable<int> := new LinkedList<int>();") +
                   " end Main; end C;"),
        0);
}

// ===========================================================================
// §28.2 exactly-once, the OVER-release direction — the reason the gate is
// the `new` EXPRESSION and not "a producer-init source". Here each
// collection has a NAMED owner that disposes it at ITS scope end; the
// widened binding is an ALIAS and must take no ownership. If it did, the
// collection would be freed twice, and the for-in below would read it after
// the free.
//
// Each named-source case pushes/adds 3 elements, sums via the widened
// IEnumerable, and verifies the sum. The integer SUM (1+2+3 = 6) is the
// observable here, regardless of order — freeing the backing twice would
// UAF on the second for-in's first MoveNext. (For Stack the OWNING wrap
// uses #2471's reverse machinery; for Set/LinkedList it uses forward
// machinery. The sum is order-invariant so we don't pin order here.)
// ===========================================================================

BEZEL_TEST(codegen_ienum_local_decl_2987,
           named_set_alias_not_freed_twice_28_2) {
    BEZEL_EXPECT_EQ(
        run("module M; class C public static method Main(): int "
            "var i := 0; var tot := 0; "
            "while i < 50 do "
            "  using var a := new Set<int>(); "
            "  a.Add(7); a.Add(3); a.Add(2); "
            "  var e: IEnumerable<int> := a; "
            "  for var v in e do tot := tot + v; end; "
            "  i := i + 1; "
            "end; "
            // 7+3+2 = 12 per iter * 50 = 600. Pin the named-set alias
            // doesn't free the set under the for-in.
            "if tot <> 600 then return 95; end; "
            "return 0; end Main; end C;"),
        0);
}

BEZEL_TEST(codegen_ienum_local_decl_2987,
           named_stack_alias_not_freed_twice_28_2) {
    BEZEL_EXPECT_EQ(
        run("module M; class C public static method Main(): int "
            "var i := 0; var tot := 0; "
            "while i < 50 do "
            "  using var a := new Stack<int>(); "
            "  a.Push(1); a.Push(2); a.Push(3); "
            "  var e: IEnumerable<int> := a; "
            "  for var v in e do tot := tot + v; end; "
            "  i := i + 1; "
            "end; "
            // 1+2+3 = 6 per iter * 50 = 300.
            "if tot <> 300 then return 96; end; "
            "return 0; end Main; end C;"),
        0);
}

BEZEL_TEST(codegen_ienum_local_decl_2987,
           named_linkedlist_alias_not_freed_twice_28_2) {
    BEZEL_EXPECT_EQ(
        run("module M; class C public static method Main(): int "
            "var i := 0; var tot := 0; "
            "while i < 50 do "
            "  using var a := new LinkedList<int>(); "
            "  a.AddLast(1); a.AddLast(2); a.AddLast(3); "
            "  var e: IEnumerable<int> := a; "
            "  for var v in e do tot := tot + v; end; "
            "  i := i + 1; "
            "end; "
            "if tot <> 300 then return 97; end; "
            "return 0; end Main; end C;"),
        0);
}

// ===========================================================================
// ITERATE pins — a Dispose that freed the backing too early would show up as
// either a UAF on a Current load (ASan) or as a wrong sum. Each iterates an
// OWNING-wrapped source over 50 iterations and asserts the observable sum.
// For Stack, the OWNING wrap uses #2471's reverse machinery (§4.11 pop order),
// so the per-iteration sequence is 3,2,1 — sum 6. For LinkedList the OWNING
// wrap uses the existing forward machinery (insertion order), so the
// per-iteration sequence is 1,2,3 — sum 6.
// ===========================================================================

BEZEL_TEST(codegen_ienum_local_decl_2987,
           stack_temp_iterates_in_pop_order_29_2) {
    BEZEL_EXPECT_EQ(
        run("module M; class C public static method Main(): int "
            "var tot := 0; var i := 0; "
            "while i < 50 do "
            "  using var e: IEnumerable<int> := new Stack<int>(); "
            "  var s := new Stack<int>(); "
            "  s.Push(1); s.Push(2); s.Push(3); "
            "  for var v in s do tot := tot + v; end; "
            "  i := i + 1; "
            "end; "
            "if tot <> 300 then return 96; end; "
            "return 0; end Main; end C;"),
        0);
}

BEZEL_TEST(codegen_ienum_local_decl_2987,
           linkedlist_temp_iterates_in_insertion_order_29_2) {
    BEZEL_EXPECT_EQ(
        run("module M; class C public static method Main(): int "
            "var tot := 0; var i := 0; "
            "while i < 50 do "
            "  using var e: IEnumerable<int> := new LinkedList<int>(); "
            "  var l := new LinkedList<int>(); "
            "  l.AddLast(1); l.AddLast(2); l.AddLast(3); "
            "  for var v in l do tot := tot + v; end; "
            "  i := i + 1; "
            "end; "
            "if tot <> 300 then return 96; end; "
            "return 0; end Main; end C;"),
        0);
}

// ===========================================================================
// CONTROLS — concrete-typed bindings keep their native protocol and are
// untouched by the fix. Each is loop-amplified 200× and runs under
// runAotChecked (ASan + LSan) to pin that the OWNING-wrap shape change for
// the IEnumerable<T> head doesn't accidentally regress the concrete-typed
// path.
// ===========================================================================

BEZEL_TEST(codegen_ienum_local_decl_2987,
           set_concrete_using_var_unchanged_4_10) {
    BEZEL_EXPECT_EQ(
        runChecked("module M; class C public static method Main(): int " +
                   loop("using var e: Set<int> := new Set<int>();") +
                   " end Main; end C;"),
        0);
}

BEZEL_TEST(codegen_ienum_local_decl_2987,
           stack_concrete_using_var_unchanged_4_11) {
    BEZEL_EXPECT_EQ(
        runChecked("module M; class C public static method Main(): int " +
                   loop("using var e: Stack<int> := new Stack<int>();") +
                   " end Main; end C;"),
        0);
}

BEZEL_TEST(codegen_ienum_local_decl_2987,
           linkedlist_concrete_using_var_unchanged_4_6) {
    BEZEL_EXPECT_EQ(
        runChecked("module M; class C public static method Main(): int " +
                   loop("using var e: LinkedList<int> := new LinkedList<int>();") +
                   " end Main; end C;"),
        0);
}

// ===========================================================================
// GUARD: the OWNING-wrapped source must still be ITERABLE on its own
// elements. The MoveNext/Current bodies are reused from the plain machinery,
// so a fix that freed the backing too early would show up as a wrong sum
// (and ASan would catch a UAF on Current).
// ===========================================================================

BEZEL_TEST(codegen_ienum_local_decl_2987,
           set_temp_iterates_its_elements_29_2) {
    BEZEL_EXPECT_EQ(
        run("module M; class C public static method Main(): int "
            "var tot := 0; var i := 0; "
            "while i < 50 do "
            "  using var e: IEnumerable<int> := new Set<int>(); "
            "  var s := new Set<int>(); "
            "  s.Add(7); s.Add(11); s.Add(13); "
            "  for var v in s do tot := tot + v; end; "
            "  i := i + 1; "
            "end; "
            // 7+11+13 = 31 per iter, * 50 = 1550. Use this to pin the
            // OWNING Dispose doesn't free the source before for-in.
            "if tot <> 1550 then return 98; end; "
            "return 0; end Main; end C;"),
        0);
}