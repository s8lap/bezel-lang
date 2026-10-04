// Codegen/disposal — a borrowing `Array<T>` viewed as `IEnumerable<T>` reclaims
// its own backing at scope end; the view is a BORROW that frees only the
// enumerator wrapper (issue #1871, spec §29.11 / §28.9 / §28.11, v0.196).
//
// ROOT CAUSE (src/codegen/LlvmBackend_Setup.cpp). A `var xs: Array<T> := [ … ]`
// collection-literal local owns a fresh heap backing (bz_array_alloc). Per the
// spec, `Array<T>` is trivial-dispose — its `Dispose()` is a no-op (§28.9) — but
// its backing storage is nonetheless "reclaimed with the instance" (§28.9 /
// §28.11) by an implementation-defined mechanism: a compiler-emitted scope-end
// `bz_array_free(backing, /*elemDisposer=*/null)`. That reclaim only fired for
// arrays bound to a fresh *materializer* (ToArray/Sorted/Reversed/Split, #1003) —
// NOT for a plain collection literal, which leaked its backing (spine + data) at
// scope end. Taking an `IEnumerable<T>` VIEW of such an array
// (`var v: IEnumerable<T> := xs`) did not change this: the view wrapper
// (wrapArrayAsIEnumerable) BORROWS the backing and its scope-end slot-1 Dispose
// (registerIterableSourceDisposal → iterEnumDispose) frees only the wrapper
// struct — so the array's backing still leaked (issue #1871: "IEnumerable<T> view
// of an Array leaks … the backing"). The enumerator wrapper itself was freed; the
// mis-attributed 40+64-byte leak in the issue is the ARRAY backing.
//
// FIX. collectBorrowingArrayFreeEligible now also collects a plain
// `var xs: Array<T> := [ … ]` LITERAL local whose element policy is backing-only
// (a null per-element disposer: value elements, and class/interface borrows) and,
// if it never escapes, registers the scope-end backing reclaim. The escape walk
// gains two borrowing-array-only relaxations (borrowingArrayMode): a `for var x
// in xs` iterates via a per-loop enumerator that BORROWS the backing (never frees
// it), and binding `xs` into a proven-non-escaping pure-`IEnumerable<T>` VIEW
// local is a BORROW (the wrapper frees only its own struct). Both are borrows, so
// the array stays the sole owner of its backing and reclaims it at ITS scope end
// — which the ownership pass proves outlives every such view (the array is usable
// after the view's scope). String/function-element literals are out of scope
// (their +1/element release differs); `OwningArray<T>` literals are untouched
// (owned-element disposal is a separate path).
//
// Fails-without / passes-with: before the fix each program below LEAKS the array
// backing, which runAotChecked's LeakSanitizer net turns into r.ran==false ->
// run()==-999 (never a legal value). After the fix every allocation is freed
// exactly once (LSan-clean) and the loop's value is returned. No double-free: the
// per-loop enumerator clones and the view wrapper each free only their own
// struct; borrowed class elements are never touched by the null-disposer reclaim.
//
// Spec: §29.11 (vtable-backed IEnumerable<T> view: slot-1 Dispose borrows the
// array; per-loop GetEnumerator clones), §28.9 / §28.11 (Array<T> trivial-dispose;
// backing reclaimed with the instance). Spec revision v0.196.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// A leak / double-free / use-after-free flips r.ran to false -> -999, distinct
// from any legal exit value.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// §29.11 / §28.9 (#1871): the issue's canonical repro. An `Array<int>` literal
// bound to an explicit `IEnumerable<int>` local, then enumerated. The view
// borrows the backing (its Dispose frees only the wrapper); the array reclaims
// its backing at scope end. 1+2+3 = 6. Before the fix: the backing leaks (-999).
BEZEL_TEST(codegen_array_ienumerable_view_backing_reclaim_1871,
           array_literal_bound_to_ienumerable_local_forin_spec_29_11) {
    std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3]; "
        "  var v: IEnumerable<int> := xs; "
        "  var total: int := 0; "
        "  for var x in v do total := total + x; end; "
        "  return total; "                    // 6
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 6);
}

// §28.9 / §28.11 (#1871): the ARRAY OUTLIVES the view's scope — the ownership
// pass admits using `xs` after the inner `do` block that held the view, proving
// the view is a borrow. So the array (not the view) must own and reclaim its
// backing, at the OUTER scope end AFTER the trailing `xs.Length` read (a view-
// owned backing would be a use-after-free here). (1+2+3) + Length 3 = 9.
// Before the fix: the backing leaks (-999).
BEZEL_TEST(codegen_array_ienumerable_view_backing_reclaim_1871,
           array_outlives_view_scope_reclaimed_at_own_scope_spec_28_11) {
    std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3]; "
        "  var total: int := 0; "
        "  do "
        "    var v: IEnumerable<int> := xs; "
        "    for var x in v do total := total + x; end; "
        "  end; "
        "  return total + xs.Count; "         // 6 + 3 = 9
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 9);
}

// §29.11 (#1871): TWO views of the same array, plus a trailing use. Each view
// borrows the shared backing; the array reclaims it EXACTLY ONCE at scope end (a
// double-free through either wrapper would corrupt the heap -> -999).
// (10+20) + (10+20) + Count 2 = 62. Before the fix: the backing leaks (-999).
BEZEL_TEST(codegen_array_ienumerable_view_backing_reclaim_1871,
           two_views_share_backing_reclaimed_once_spec_29_11) {
    std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var xs: Array<int> := [10, 20]; "
        "  var v1: IEnumerable<int> := xs; "
        "  var v2: IEnumerable<int> := xs; "
        "  var total: int := 0; "
        "  for var a in v1 do total := total + a; end; "
        "  for var b in v2 do total := total + b; end; "
        "  return total + xs.Count; "          // 30 + 30 + 2 = 62
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 62);
}

// §28.9 / §28.11 (#1871): a DIRECT `for var x in xs` over the literal array (no
// intermediate view) — the for-in clones a per-loop enumerator that BORROWS the
// backing and frees only its own struct, so the array stays reclaim-eligible and
// its backing is freed once at scope end. 1+2+3+4 = 10. Before the fix: leaks
// (-999).
BEZEL_TEST(codegen_array_ienumerable_view_backing_reclaim_1871,
           direct_forin_over_array_literal_reclaimed_spec_28_11) {
    std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3, 4]; "
        "  var total: int := 0; "
        "  for var x in xs do total := total + x; end; "
        "  return total; "                      // 10
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 10);
}

// §28.11 (#1871): a CLASS-element array `Array<Dog>` viewed as `IEnumerable<Dog>`.
// The array BORROWS its elements (the `using var` dogs own them, disposed exactly
// once each), so the backing reclaim uses a NULL per-element disposer and must not
// touch the dogs — a non-null disposer here would double-free them (ASan error ->
// -999). Counts 2 elements; both dogs freed once by their using vars, backing
// freed once. Before the fix: the array backing leaks (-999).
BEZEL_TEST(codegen_array_ienumerable_view_backing_reclaim_1871,
           class_element_array_view_borrows_elements_spec_28_11) {
    std::string src =
        "module M; "
        "class Dog public Name: string := \"d\"; end Dog; "
        "class C public static method Main(): int "
        "  using var d1: Dog := new Dog(); "
        "  using var d2: Dog := new Dog(); "
        "  var xs: Array<Dog> := [d1, d2]; "
        "  var v: IEnumerable<Dog> := xs; "
        "  var n: int := 0; "
        "  for var d in v do n := n + 1; end; "
        "  return n; "                           // 2
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 2);
}

}  // namespace
