// Spec-derived regression tests for fix #2721 —
// `Array<T>.AddRange(items: IEnumerable<T>)` over a NON-array-backed source.
//
// AUTHORITY. stdlib §4.4 (v0.240) declares on `Array<T>`, verbatim:
//
//     public method AddRange(items: IEnumerable<T>);
//
// The parameter is `IEnumerable<T>`, NOT `Array<T>`. stdlib §4.5 declares the
// sibling owning overload on `OwningArray<T>` — `public method AddRange(owned
// items: OwningArray<T>);   // drains items into self` — whose argument IS a
// concrete array-backed collection by declaration; that path is untouched by
// this fix and is covered here only as a "still compiles" control.
//
// THE DEFECT. Codegen lowered the argument with a bare `lowerExpr` and then read
// it with `bz_array_len` / `bz_array_ptr_at`, which is correct ONLY when the
// source happens to be array-backed. For a lazy LINQ pipeline the value is a
// `bz_lazy_t*`, and for a `LinkedList<T>` it is a `bz_llist_t*` — so codegen
// read a length and element pointers out of a struct of a different shape.
// Measured on 07cc2e0 (counts pinned through the exit code, never a printing
// path, so what is observed is the collection and not a formatting path):
//
//   * `b.AddRange(a.Where(x > 1))` over [1,2,3]  -> Count 1, §4.4 requires 2
//   * `b.AddRange(a.Select(x * 10))` over [1,2,3] -> Count 2, §4.4 requires 3
//   * `b.AddRange(linkedList)`                    -> ASan heap-buffer-overflow
//         READ of size 4, `memcpy` <- `bz_array_add` over memory allocated by
//         `llist_node_alloc` <- `bz_llist_add_last`. So this was not merely a
//         wrong value: it was a genuine out-of-bounds read, which the filed
//         issue did not know (it tested only the lazy shape).
//   * `b.AddRange(a)` (plain Array source)        -> Count 3, already correct
//   * `b.AddRange(set)`                           -> Count 2, already correct
//
// The last two are the reason this fix is scoped the way it is: every shape that
// already worked keeps the byte-identical raw path, so the change can only add
// correct behavior where there was none.
//
// THE FIX. Route a non-array-backed source through `lowerToLazyEnumerator` — the
// one backing-agnostic normalizer, already used by `ToArray`/`ToSet`/
// `ExceptWith` — into a `bz_lazy_to_array` snapshot, run the existing
// per-element append loop over the snapshot, and free the snapshot SHELL with a
// NULL disposer. This is the same remedy #2693 used for `new
// LinkedList<T>(source)`.
//
// OWNERSHIP (spec §28.9/§28.11). `bz_lazy_to_array` does NOT retain — the
// `ToArray` terminal calls `bz_array_retain_strings` separately — so a snapshot
// slot holds exactly the same BORROWED reference the raw source slot did. The
// pre-existing per-element `emitStringRetain` therefore still contributes
// exactly one +1 per appended element, consumed by the receiver's own
// release-on-removal thunk, and the snapshot shell is freed without touching the
// elements. Case (G) pins that balance under `runAotChecked`, whose ASan+LSan
// oracle flips `r.ran` for a LEAK and for a DOUBLE-FREE alike; its strings are
// built at RUNTIME (`$"v-{i}"`) because `"ab" + "cd"` constant-folds to an
// rc-sentinel and would make the check vacuous. Values are routed through a
// borrowed PARAMETER so the pre-existing #422 local-escape leak is not what is
// being measured.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent:
// runAotChecked returns ran=false with a "SKIP" diagnostic.
// Builds [1,2,3] at runtime, then AddRanges `expr` into a fresh Array<int> and
// returns a caller-chosen encoding of the result.
std::string prog(const std::string& body) {
    return "module M; import System.Collections.Generic; "
           "class C public static method Main(): int " + body + " end Main; end C;";
}

}  // namespace

// ── The filed repro: a lazy `Where` pipeline (§4.4) ──────────────────────────

// per stdlib §4.4 (v0.240), #2721. Pre-fix: 1. Where(x > 1) over [1,2,3] is
// [2,3], so §4.4's "items" contributes TWO elements.
BEZEL_TEST(array_addrange_ienumerable_2721, lazy_where_source_count) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var a: Array<int> := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  using var b: Array<int> := new Array<int>(); "
        "  b.AddRange(a.Where(lambda(x: int) => x > 1)); "
        "  return b.Count; ")), 2);
}

// The COUNT alone can be right for the wrong reason, so pin the VALUES too.
BEZEL_TEST(array_addrange_ienumerable_2721, lazy_where_source_values) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var a: Array<int> := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  using var b: Array<int> := new Array<int>(); "
        "  b.AddRange(a.Where(lambda(x: int) => x > 1)); "
        "  if b.Count <> 2 then return 90; end; "
        "  return b[0] * 10 + b[1]; ")), 23);   // [2,3]
}

// ── A lazy `Select` pipeline: source ORDER is preserved (§4.4) ───────────────

// per stdlib §4.4 (v0.240), #2721. Pre-fix: 2 (an under-count), so this pins
// both the length and that element k of the source becomes element k of self.
BEZEL_TEST(array_addrange_ienumerable_2721, lazy_select_source_preserves_order) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var a: Array<int> := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  using var b: Array<int> := new Array<int>(); "
        "  b.AddRange(a.Select(lambda(x: int) => x * 10)); "
        "  if b.Count <> 3 then return 90; end; "
        "  if b[0] <> 10 then return 91; end; "
        "  if b[1] <> 20 then return 92; end; "
        "  if b[2] <> 30 then return 93; end; "
        "  return 42; ")), 42);
}

// ── A `LinkedList<T>` source: pre-fix an OUT-OF-BOUNDS READ, not a wrong count ─

// per stdlib §4.4 (v0.240) + §4.6, #2721. `LinkedList<T>` is `bz_llist_t*`-
// backed, so the raw path read llist node memory at bz_array_t stride. Under the
// plain (non-ASan) oracle this produced a wrong value; under ASan it ABORTED
// with a heap-buffer-overflow. Both directions are fixed by the normalizer,
// which routes a linked list through `bz_lazy_from_llist`.
BEZEL_TEST(array_addrange_ienumerable_2721, linkedlist_source_count_and_order) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var ll: LinkedList<int> := new LinkedList<int>(); "
        "  ll.AddLast(4); ll.AddLast(5); "
        "  using var b: Array<int> := new Array<int>(); "
        "  b.AddRange(ll); "
        "  if b.Count <> 2 then return 90; end; "
        "  return b[0] * 10 + b[1]; ")), 45);
}

// The same shape under ASan/LSan: pre-fix this is the heap-buffer-overflow
// READ quoted in the header comment, so `ran` is false without the fix.
BEZEL_TEST(array_addrange_ienumerable_2721, linkedlist_source_is_memory_safe) {
    auto r = runAotChecked(prog(
        "  using var ll: LinkedList<int> := new LinkedList<int>(); "
        "  ll.AddLast(4); ll.AddLast(5); "
        "  using var b: Array<int> := new Array<int>(); "
        "  b.AddRange(ll); "
        "  return b.Count; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// ── Controls: the shapes that ALREADY worked must be unchanged ───────────────

// per stdlib §4.4 (v0.240), #2721. An array-backed source keeps the raw path
// verbatim — this is the filer's own control, and it must not regress.
BEZEL_TEST(array_addrange_ienumerable_2721, plain_array_source_unchanged) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var a: Array<int> := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  using var b: Array<int> := new Array<int>(); "
        "  b.AddRange(a); "
        "  if b.Count <> 3 then return 90; end; "
        "  return b[0] + b[1] + b[2]; ")), 6);
}

// per stdlib §4.4/§4.10 (v0.240), #2721 + #2726. A `Set<T>` source is the case
// that proves the "COUNT alone can be right for the wrong reason" warning above
// is not theoretical, so it gets its own value assertion.
//
// `Set`/`OwningSet`/`SortedSet` are listed in `kArrayBackedCollections`
// (BuiltinRegistry.cpp) but lowerNew builds them with `bz_dict_alloc_generic`
// for every record / non-nullable primitive / enum / string element (#2726).
// A dict pointer read as an array header yields the RIGHT Count purely by
// accidental struct-prefix aliasing — `bz_array_t{void* data; int64_t count;}`
// and `bz_dict_t{bz_entry_t* buckets; int64_t count;}` agree on `count`'s
// offset — while the ELEMENTS are `bz_entry_t` header bytes. Measured: Count 2
// (correct) but `b[0] <> 7`. An earlier draft of this fix asserted only the
// Count here and therefore certified the bug as correct behavior.
BEZEL_TEST(array_addrange_ienumerable_2721, set_source_values_not_just_count) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s: Set<int> := new Set<int>(); "
        "  s.Add(7); s.Add(8); "
        "  using var b: Array<int> := new Array<int>(); "
        "  b.AddRange(s); "
        "  if b.Count <> 2 then return 90; end; "     // aliases even when broken
        "  if b[0] <> 7 then return 91; end; "        // the real assertion
        "  if b[1] <> 8 then return 92; end; "
        "  return 42; ")), 42);
}

// per stdlib §4.4/§4.10 (v0.240), #2721 + #2726. The same source with STRING
// elements: pre-fix the loop read `buckets[0].key_len` as an element pointer and
// handed it to `bz_string_retain`. Contents are asserted (not lengths), and LSan
// is live, so a lost or spurious reference shows up either way.
BEZEL_TEST(array_addrange_ienumerable_2721, set_of_string_source_contents) {
    auto r = runAotChecked(
        "module M; import System.Collections.Generic; "
        "class C "
        "  private static method Fill(dst: Array<string>, src: Set<string>) "
        "    dst.AddRange(src); "
        "  end Fill; "
        "  public static method Main(): int "
        "    using var s: Set<string> := new Set<string>(); "
        "    var i: int := 0; "
        "    while i < 2 do s.Add($\"v-{i}\"); i := i + 1; end; "
        "    using var b: Array<string> := new Array<string>(); "
        "    Fill(b, s); "
        "    if b.Count <> 2 then return 90; end; "
        "    if not b.Contains(\"v-0\") then return 91; end; "
        "    if not b.Contains(\"v-1\") then return 92; end; "
        "    return 42; "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ── Edge shapes ─────────────────────────────────────────────────────────────

// per stdlib §4.4 (v0.240), #2721: an empty pipeline appends nothing (and the
// snapshot shell is still freed).
BEZEL_TEST(array_addrange_ienumerable_2721, empty_lazy_source_appends_nothing) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var a: Array<int> := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  using var b: Array<int> := new Array<int>(); "
        "  b.AddRange(a.Where(lambda(x: int) => x > 99)); "
        "  return 7 + b.Count; ")), 7);
}

// per stdlib §4.4 (v0.240), #2721: AddRange APPENDS — a second call accumulates
// onto the first rather than replacing it. This also exercises two snapshots
// inside one function (each freed independently).
BEZEL_TEST(array_addrange_ienumerable_2721, two_lazy_addranges_accumulate) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var a: Array<int> := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  using var b: Array<int> := new Array<int>(); "
        "  b.AddRange(a.Where(lambda(x: int) => x > 1)); "
        "  b.AddRange(a.Where(lambda(x: int) => x > 2)); "
        "  if b.Count <> 3 then return 90; end; "
        "  return b[0] * 100 + b[1] * 10 + b[2]; ")), 233);
}

// per stdlib §4.4 (v0.240) + §29.11, #2721: a VTABLE-backed `IEnumerable<T>`
// source (a yield method's result), which takes lowerToLazyEnumerator's
// `materializeVtableIEnumerableToArray` branch rather than the array or lazy
// one. This is the third distinct normalizer branch the fix relies on and the
// one that reads nothing array-shaped at all.
//
// Deliberately `runAot` and not `runAotChecked`: a minting yield iterator leaks
// its elements, and that leak is PRE-EXISTING rather than introduced here. The
// evidence is a program that does not involve this fix at all — a plain
// `const t: Array<string> := Gen().ToArray();` contains no `AddRange` anywhere
// and still leaks on 07cc2e0 — which settles consumer-independence on its own.
// (An earlier draft argued this from a byte-count comparison between the two
// consumers instead. That reasoning was unsound and is withdrawn: they are
// different programs with different live sets, so neither total bounds the
// other, and only a same-program before/after could have shown anything.)
//
// That the fix adds no reference of its own is pinned separately and positively:
// `bz_lazy_to_array` disposes the lazy internally and the snapshot shell is
// freed with a NULL disposer, and `string_elements_balance_under_asan` below
// exercises the new lazy path under a live LSan oracle. Asserting leak-freedom
// HERE would enshrine someone else's bug as this fix's contract; the leak is
// filed separately, and what is pinned here is that the VALUES arrive correctly.
BEZEL_TEST(array_addrange_ienumerable_2721, vtable_ienumerable_source_values) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C "
        "  private static method Gen(): IEnumerable<string> "
        "    var i: int := 0; "
        "    while i < 3 do yield return $\"g-{i}\"; i := i + 1; end; "
        "  end Gen; "
        "  private static method Fill(dst: Array<string>) "
        "    dst.AddRange(Gen()); "
        "  end Fill; "
        "  public static method Main(): int "
        "    using var dst: Array<string> := new Array<string>(); "
        "    Fill(dst); "
        "    if dst.Count <> 3 then return 90; end; "
        "    if not dst.Contains(\"g-0\") then return 91; end; "
        "    if not dst.Contains(\"g-2\") then return 92; end; "
        "    return 42; "
        "  end Main; end C;"), 42);
}

// per stdlib §4.4/§4.10 (v0.240) + §28.11, #2721 + #2726: a CLASS-ELEMENT
// `OwningSet<T>` source. This is the load-bearing case for the fix's scoping
// argument, and it is the one shape the exclusion NEWLY REROUTES.
//
// The fix drops the WHOLE set family from the array-backed fast path, including
// the class-element sets that genuinely ARE `bz_array_t*`-backed — so this
// source now takes `lowerToLazyEnumerator`, a path it did not take before. It
// stays correct only because the normalizer re-decides with the SAME
// `genericBackingType(ty, 0, /*isSet=*/true, …)` discriminator `lowerNew`
// constructs with: null for a class element, so `lowerNew` builds an array and
// the normalizer's set branch declines `bz_lazy_from_set` and falls through to
// its own array path. Agreement is an identity, not a coincidence — but nothing
// pinned it, so an edit to the normalizer's set branch could break class-element
// sets silently. `OwningSet<T> where T: class` means EVERY `OwningSet` is
// array-backed, so this is the common case for that type, not a corner.
BEZEL_TEST(array_addrange_ienumerable_2721, class_element_owning_set_source) {
    auto r = runAotChecked(
        "module M; import System.Collections.Generic; "
        "class Box public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  destructor end destructor; end Box; "   // §A.4: no visibility on a destructor
        "class C "
        "  private static method Fill(dst: Array<Box>, src: OwningSet<Box>) "
        "    dst.AddRange(src); "
        "  end Fill; "
        "  public static method Main(): int "
        "    using var os: OwningSet<Box> := new OwningSet<Box>(); "
        "    os.Add(new Box(7)); "
        "    os.Add(new Box(8)); "
        // The borrowing Array takes element BORROWS; `os` still owns and
        // disposes them, so LSan/ASan is a live oracle in both directions here.
        "    using var b: Array<Box> := new Array<Box>(); "
        "    Fill(b, os); "
        "    if b.Count <> 2 then return 90; end; "
        "    if b[0].V <> 7 then return 91; end; "
        "    if b[1].V <> 8 then return 92; end; "
        "    return 42; "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ── Ownership: the string element balance (spec §28.9/§28.11) ───────────────

// per spec §28.9/§28.11 + stdlib §4.4 (v0.240), #2721. The receiver takes an
// INDEPENDENT +1 per appended element and the borrowed source keeps its own, so
// both arrays stay readable afterwards and NOTHING is leaked or double-freed.
// runAotChecked's LSan flips `ran` on a leak and ASan on a double-free, so this
// single assertion is a live oracle in BOTH directions.
BEZEL_TEST(array_addrange_ienumerable_2721, string_elements_balance_under_asan) {
    auto r = runAotChecked(
        "module M; import System.Collections.Generic; "
        "class C "
        // Borrowed PARAMETERS keep the #422 local-escape leak out of the
        // measurement.
        "  private static method Fill(dst: Array<string>, src: Array<string>) "
        "    dst.AddRange(src.Where(lambda(s: string) => s.Length > 0)); "
        "  end Fill; "
        "  public static method Main(): int "
        "    using var src: Array<string> := new Array<string>(); "
        "    var i: int := 0; "
        // Runtime-built strings: `"ab" + "cd"` would constant-fold to an rc
        // sentinel and make the refcount check vacuous.
        "    while i < 3 do src.Add($\"v-{i}\"); i := i + 1; end; "
        "    using var dst: Array<string> := new Array<string>(); "
        "    Fill(dst, src); "
        "    var score: int := 0; "
        "    if dst.Count = 3 then score := score + 1; end; "
        // CONTENTS, not just lengths — a wrong pointer would still have a length.
        "    if dst.Contains(\"v-0\") and dst.Contains(\"v-2\") then score := score + 2; end; "
        // The borrowed source is untouched and still readable.
        "    if src.Count = 3 and src.Contains(\"v-1\") then score := score + 4; end; "
        "    return score; "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
