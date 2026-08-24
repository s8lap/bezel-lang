// Spec-derived regression tests for issue #2773 (row B1, v0.240): `SortedSet<T>`
// iteration over a class or interface element must order by the user's
// `CompareTo`, per stdlib §4.18's ascending-order promise.
//
// WHAT THE DOCUMENTS REQUIRE.  stdlib §4.18 declares
//     public class SortedSet<T>: IReadOnlyCollection<T> where T: IComparable<T>
//       public method Add(item: T); ...
// and §4.18's contract: "iteration in ascending order" — the elements yield in
// the order the user's `CompareTo` induces, NOT in `Add`-insertion order.
//
// THE DEFECT — a SILENT ORDERING VIOLATION, in codegen only.  The
// `wrapSortedSetAsIEnumerable` site at LlvmBackend_Calls2.cpp:13181 routes the
// snapshot through `bz_sort_kv_gen` with a per-element comparator produced by
// `buildSortedKeyComparator` (LlvmBackend_Calls3.cpp:521).  That comparator
// builder handled only primitive scalars (i32/i64/f32/f64/i1/i8/i16) and the
// `string` branch is handled INSIDE `wrapSortedSetAsIEnumerable` itself with
// its own internal-linkage `bz$sscmp$string` thunk.  For any CLASS or
// INTERFACE element it returned NULL — a silent fall-through to the
// insertion-order `bz_dict_keys_gen` output that violates §4.18.
//
// The defect is silently wrong, not memory-unsafe: the comparator args aPtr
// and bPtr each point to malloc(8) buffers holding the instance pointer
// (mirroring the `string` element's shape, but the buildSortedKeyComparator
// branch never reached them because no key was present). The pre-fix output
// is the hash-order / insertion-order output of `bz_dict_keys_gen`, not the
// user-declared total order — so `[3, 1, 2]` iterates back as `[3, 1, 2]` and a
// counter (or any pack into the exit code) discriminates the buggy value
// from the sorted one.
//
// THE FIX extends `buildSortedKeyComparator` to handle a `T` whose sema
// `TypeRef` is `Class` or `Interface` declaring `CompareTo`, dispatching
// `a.CompareTo(b)` via the existing class vtable or §13 itable thunk shape
// (mirroring the `bz_array_sort` comparator at LlvmBackend_Calls.cpp:4301 for
// the class case and `bz$ifacecmp$<Iface>` (#2768 / `ensureIfaceCmpThunk`) for
// the interface case). The wrapper at Calls2.cpp then re-uses the SAME thunk
// ABI the `bz_array_sort` codepath uses (`i32` returning, two `ptr` args,
// each pointing to the key buffer) — the values reaching it for a class
// element are the same `void*`-pointing-to-instance-pointer buffers the
// hash-table's `bz_dict_set_gen` already stores at insert time.
//
// CONTROLS. The `int` and `string` cases are the §4.18 surface that already
// ordered correctly; if a future change widens the new arm and steals either,
// the §4.18 contract breaks for a UNIQUE element category. The interface case
// is a sibling of the class case (a `T` that lowers to `ptrTy()` but is not a
// `string` and not a `Class`), and the #2768 anti-mis-route machinery (it
// rejects elements whose interface names no `IComparable<Self>`) excludes it
// here, so a successful test must reach the user's `CompareTo` once more.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #2773 (B1) / stdlib §4.18 (v0.240): for-in over `SortedSet<T>` where T is a
// CLASS named `IComparable<Self>` and the inserted order is [3, 1, 2] must
// iterate in ASCENDING order.  Pack the K values from each `for` step into
// base 10: 1, 2, 3 → 1*100 + 2*10 + 3 = 123.  The pre-fix value is the
// insertion-order output of `bz_dict_keys_gen`: 3, 1, 2 → 312.
//
// The element is a class with `public K: int` and a `CompareTo(other: Item): int`
// method that returns `K - other.K` (strictly ascending).  This is the
// exact `where T: IComparable<T>` shape stdlib §4.18 demands.  Item is
// declared at module scope — Bezel has no nested-class-inside-method form.
BEZEL_TEST(sorted_set_class_element_iteration_2773,
           forin_class_element_orders_ascending_spec_4_18) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Item: IComparable<Item> "
        "  public K: int; "
        "  public constructor(k: int) K := k; end constructor; "
        "  public method CompareTo(other: Item): int => K - other.K; "
        "end Item; "
        "class C public static method Main(): int "
        "  using var ss := new SortedSet<Item>(); "
        "  ss.Add(new Item(3)); ss.Add(new Item(1)); ss.Add(new Item(2)); "
        "  var order := 0; "
        "  for var x in ss do order := order * 10 + x.K; end; "
        "  return order; "
        "end Main; end C;"
    ), 123);
}

// #2773 (B1) / stdlib §4.18: the same CLASS element over the EXPLICIT
// .GetEnumerator() surface — the `for-in` and the explicit-call surfaces
// must agree on ascending order so the two §4.18 paths do not drift (the
// SortedDictionary twin in tests/codegen/sorted_1426_test.cpp proves the
// pairing on string/int; this proves it on a CLASS element). 1, 2, 3 → 123.
BEZEL_TEST(sorted_set_class_element_iteration_2773,
           getenumerator_class_element_orders_ascending_spec_4_18) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Item: IComparable<Item> "
        "  public K: int; "
        "  public constructor(k: int) K := k; end constructor; "
        "  public method CompareTo(other: Item): int => K - other.K; "
        "end Item; "
        "class C public static method Main(): int "
        "  using var ss := new SortedSet<Item>(); "
        "  ss.Add(new Item(3)); ss.Add(new Item(1)); ss.Add(new Item(2)); "
        "  var order := 0; "
        "  using var e := ss.GetEnumerator(); "
        "  while e.MoveNext() do order := order * 10 + e.Current.K; end; "
        "  return order; "
        "end Main; end C;"
    ), 123);
}

// #2773 (B1) / stdlib §4.18: SortedSet's `.First` (least) on a CLASS
// element must order by `CompareTo`.  Same `Item: IComparable<Item>` shape,
// insertion order [3, 1, 2], `.First` returns the smallest K = 1.  Pre-fix
// the `buildSortedKeyComparator` path that `bz_dict_min_key_gen` uses is the
// same one `wrapSortedSetAsIEnumerable` uses, so the same defect reaches this
// site — `.First` would return whatever insertion-order position zero held,
// which is K = 3.
BEZEL_TEST(sorted_set_class_element_iteration_2773,
           first_class_element_returns_least_spec_4_18) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Item: IComparable<Item> "
        "  public K: int; "
        "  public constructor(k: int) K := k; end constructor; "
        "  public method CompareTo(other: Item): int => K - other.K; "
        "end Item; "
        "class C public static method Main(): int "
        "  using var ss := new SortedSet<Item>(); "
        "  ss.Add(new Item(3)); ss.Add(new Item(1)); ss.Add(new Item(2)); "
        "  return ss.First.K; "
        "end Main; end C;"
    ), 1);
}

// #2773 (B1) / stdlib §4.18 + spec §14.2 rule 3: SortedSet<T> for an
// INTERFACE element `IFoo: IComparable<IFoo>` (the §14.2 self-recurrence —
// `where T: IComparable<T>` admits an interface substitution).  The fix must
// reach `bz$ifacecmp$<IFoo>` (#2768) so the itable dispatch over the IFOO
// vtable orders the iteration, not the class arm (interfaces are not Classes,
// so `classCompareMs` stays null) and not the silent fall-through.  Insertion
// order [3, 1, 2] → expected ascending packed = 1*100 + 2*10 + 3 = 123.
// Pre-fix the for-in iterates in insertion order: 3*100 + 1*10 + 2 = 312.
BEZEL_TEST(sorted_set_class_element_iteration_2773,
           forin_iface_self_comparable_orders_ascending_spec_4_18) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public interface IFoo: IComparable<IFoo> "
        "  method Rank(): int; "
        "end IFoo; "
        "class Node: IFoo "
        "  private _r: int; "
        "  public constructor(r: int) _r := r; end constructor; "
        "  public method Rank(): int => _r; "
        "  public method CompareTo(other: IFoo): int => _r - other.Rank(); "
        "end Node; "
        "class C public static method Main(): int "
        "  using var ss := new SortedSet<IFoo>(); "
        "  using var a := new Node(3); "
        "  using var b := new Node(1); "
        "  using var c := new Node(2); "
        "  ss.Add(a); ss.Add(b); ss.Add(c); "
        "  var order := 0; "
        "  for var x in ss do order := order * 10 + x.Rank(); end; "
        "  return order; "
        "end Main; end C;"
    ), 123);
}

// CONTROL — SortedSet<int> insertion [3, 1, 2] iterates 1, 2, 3 ascending.
// This case already works because `buildSortedKeyComparator` handles i32
// scalars.  Pinning here so the fix never steals the integer path.  123.
BEZEL_TEST(sorted_set_class_element_iteration_2773,
           forin_int_element_still_ascending_spec_4_18) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var ss := new SortedSet<int>(); "
        "  ss.Add(3); ss.Add(1); ss.Add(2); "
        "  var order := 0; "
        "  for var x in ss do order := order * 10 + x; end; "
        "  return order; "
        "end Main; end C;"
    ), 123);
}

// CONTROL — SortedSet<string> insertion ["c", "a", "b"] iterates
// ascending ("a", "b", "c").  The string branch at
// wrapSortedSetAsIEnumerable:13194–13243 owns this case, NOT
// `buildSortedKeyComparator`, and the §4.18 ordinal-contract must not
// regress.  Asserted via Console.WriteLine because string-pack into the exit
// code is not coherent (8-bit truncation hides the order).
BEZEL_TEST(sorted_set_class_element_iteration_2773,
           forin_string_element_still_ascending_spec_4_18) {
    auto r = bezel::test::runAotOut(
        "module M; "
        "class C public static method Main(): int "
        "  using var ss := new SortedSet<string>(); "
        "  ss.Add(\"c\"); ss.Add(\"a\"); ss.Add(\"b\"); "
        "  for var v in ss do Console.WriteLine(v); end; "
        "  return 0; "
        "end Main; end C;"
    );
    BEZEL_EXPECT_EQ(r, "a\nb\nc");
}