// Spec-derived regression tests for fix #2773 (B1) — `SortedSet<T>` orders
// and dedups a CLASS/INTERFACE element by `CompareTo`, per stdlib §4.18
// (v0.286).
//
// BACKGROUND. Issue #2773 was filed as a sweep from #2768 ("the element
// lowers to ptrTy() therefore string" predicate shape). Its section B row for
// `SortedSet<T>` claimed a SILENT ordering violation: a class/interface
// element would fall through to an unsorted insertion-order snapshot with no
// diagnostic. That row was REFUTED on the current tree (v0.286): between
// v0.240 and now, issue #3428 (v0.283) turned the shape into a truthful
// compile-time REJECTION instead ("stdlib §4.18: SortedSet<T> is not yet
// implemented for a reference element type") — so by the time this lane
// picked the issue up, there was no longer a silent-wrong-value bug to fix,
// only a rejection to (soundly) lift.
//
// What made lifting it sound: fix #3488 (SortedDictionary's reference-key
// ordering, v0.283) built `buildObjectCompareToThunk` — a shared
// `CompareTo`-through-the-vtable comparator for any class/interface type,
// lifted from the #1195 (`Array.Sort`) / #2768 (`Array.Sort` interface arm)
// thunks — and `buildSortedObjectKeyIdentity`, the (constant-hash,
// `CompareTo == 0`) identity pair that makes a reference-keyed hash table
// obey `CompareTo`-equality instead of §8.3 `Equals`/`GetHashCode` reference
// identity. Fix #2773 (B1) wires `SortedSet<T>`'s construction
// (`LlvmBackend_Exprs2.cpp`'s `lowerNew` Set arm) and its backing-type
// eligibility (`LlvmBackend_Calls3.cpp`'s `genericBackingType`) onto that
// SAME infrastructure. Every SortedSet CONSUMER (`for`-in / `GetEnumerator`
// via `wrapSortedSetAsIEnumerable`, `.First`/`.Last` via
// `buildSortedKeyComparator`) already dispatched a reference key through
// `buildObjectCompareToThunk` as of #3488 — this fix's own diff touches only
// the CONSTRUCTION site, and every row below exercises a path this fix's
// diff does not itself edit, proving the ordering/dedup infrastructure was
// already correct and only unreachable.
//
// This file is the ordering/dedup CORRECTNESS surface. The admission/
// rejection BOUNDARY (which element categories construct vs. still reject)
// is pinned in tests/codegen/sorted_set_class_element_3428_test.cpp, updated
// in the same commit per §7 (a fix reversing a prior rejection must update
// the tests that pinned it).
//
// `runAotOut` rows assert the printed ORDER on stdout directly — never a
// derived checksum — so a row that happened to sum to the right total while
// permuted would still fail.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

std::string out(const std::string& src) { return runAotOut(src); }

// `Ord: IComparable<Ord>` — the CLASS control every row below shares.
const char* PRELUDE =
    "module M; "
    "class Ord: IComparable<Ord> "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  public method CompareTo(other: Ord): int return V - other.V; end CompareTo; "
    "end Ord; "
    "interface IOrd: IComparable<IOrd> "
    "  method Val(): int; "
    "end IOrd; "
    "class OrdI: IOrd "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  public method Val(): int => V; "
    "  public method CompareTo(other: IOrd): int return V - other.Val(); end CompareTo; "
    "end OrdI; ";

std::string prog(const std::string& body) {
    return std::string(PRELUDE) +
           "class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// ── Ascending order (stdlib §4.18's "iteration in ascending order") ──────────

// per stdlib §4.18 (v0.286), fix #2773 (B1): `for`-in over a CLASS element —
// added out of order (3, 1, 2), the desugared iteration must read 1, 2, 3.
BEZEL_TEST(sorted_set_class_element_2773, for_in_class_element_orders_ascending) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var s := new SortedSet<Ord>(); "
        "  using var a := new Ord(3); using var b := new Ord(1); using var c := new Ord(2); "
        "  s.Add(a); s.Add(b); s.Add(c); "
        "  var acc: int := 0; "
        "  for var x in s do acc := acc * 10 + x.V; end; "
        "  Console.WriteLine(acc); "
        "  return 0; ")),
        std::string("123"));
}

// per stdlib §4.18 (v0.286), fix #2773 (B1): the EXPLICIT `.GetEnumerator()`
// surface must agree with `for`-in on ordering — both desugar to
// `wrapSortedSetAsIEnumerable`, but this calls it directly rather than
// through the `for` statement's own lowering.
BEZEL_TEST(sorted_set_class_element_2773, getenumerator_class_element_orders_ascending) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var s := new SortedSet<Ord>(); "
        "  using var a := new Ord(3); using var b := new Ord(1); using var c := new Ord(2); "
        "  s.Add(a); s.Add(b); s.Add(c); "
        "  using var e := s.GetEnumerator(); "
        "  var acc: int := 0; "
        "  while e.MoveNext() do acc := acc * 10 + e.Current.V; end; "
        "  Console.WriteLine(acc); "
        "  return 0; ")),
        std::string("123"));
}

// per stdlib §4.18 (v0.286), fix #2773 (B1): `.First` (least) and `.Last`
// (greatest) on a CLASS element, via `buildSortedKeyComparator`'s
// `buildObjectCompareToThunk` arm and `bz_dict_min_key_gen`/`bz_dict_max_key_gen`.
BEZEL_TEST(sorted_set_class_element_2773, first_and_last_class_element) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var s := new SortedSet<Ord>(); "
        "  using var a := new Ord(3); using var b := new Ord(1); using var c := new Ord(2); "
        "  s.Add(a); s.Add(b); s.Add(c); "
        "  Console.WriteLine(s.First.V); "
        "  Console.WriteLine(s.Last.V); "
        "  return 0; ")),
        std::string("1\n3"));
}

// per stdlib §4.18 (v0.286), fix #2773 (B1): an INTERFACE element
// (`IOrd: IComparable<IOrd>`) orders ascending — `buildObjectCompareToThunk`'s
// interface arm reuses #2768's `ensureIfaceCmpThunk` itable dispatch.
BEZEL_TEST(sorted_set_class_element_2773, for_in_interface_element_orders_ascending) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var s := new SortedSet<IOrd>(); "
        "  using var a := new OrdI(5); using var b := new OrdI(2); using var c := new OrdI(9); "
        "  s.Add(a); s.Add(b); s.Add(c); "
        "  var acc: int := 0; "
        "  for var x in s do acc := acc * 10 + x.Val(); end; "
        "  Console.WriteLine(acc); "
        "  return 0; ")),
        std::string("259"));
}

// ── Controls: int and string elements are BYTE-IDENTICAL to before this fix ──
// (they never reach the new `hasObjectHeaderEquality` arm at all — neither
// lowers to a §8.3 object header — so a regression here would mean the fix
// widened something it should not have.)

BEZEL_TEST(sorted_set_class_element_2773, for_in_int_element_orders_ascending_control) {
    BEZEL_EXPECT_EQ(out(
        "module M; class C public static method Main(): int "
        "  using var s := new SortedSet<int>(); "
        "  s.Add(30); s.Add(10); s.Add(20); "
        "  var acc: int := 0; "
        "  for var x in s do acc := acc * 100 + x; end; "
        "  Console.WriteLine(acc); "
        "  return 0; "
        "end Main; end C;"),
        std::string("102030"));
}

BEZEL_TEST(sorted_set_class_element_2773, for_in_string_element_orders_ascending_control) {
    BEZEL_EXPECT_EQ(out(
        "module M; class C public static method Main(): int "
        "  using var s := new SortedSet<string>(); "
        "  s.Add(\"cherry\"); s.Add(\"apple\"); s.Add(\"banana\"); "
        "  var acc: string := \"\"; "
        "  for var x in s do acc := acc + x + \",\"; end; "
        "  Console.WriteLine(acc); "
        "  return 0; "
        "end Main; end C;"),
        std::string("apple,banana,cherry,"));
}

// ── Membership: CompareTo-equality, not §8.3 reference identity ──────────────
// (stdlib §4.18: "for SortedSet<T> that identity is CompareTo-equality, and
// adding an equal element leaves the set unmodified".)

// per stdlib §4.18 (v0.286), fix #2773 (B1): `Add` of a CompareTo-equal but
// reference-DISTINCT instance leaves the set unmodified (discards silently —
// `Add` is unit-returning, so "unmodified" is observed via `Count`).
BEZEL_TEST(sorted_set_class_element_2773, add_compareto_equal_leaves_set_unmodified) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<Ord>(); "
        "  using var a := new Ord(5); using var b := new Ord(5); "
        "  s.Add(a); "
        "  s.Add(b); "  // CompareTo-equal to a, but NOT the same reference
        "  return s.Count; ")),
        1);
}

// per stdlib §4.18 (v0.286), fix #2773 (B1): `TryAdd` reports the duplicate
// (`false`) exactly when §4.18 says the set is unmodified.
BEZEL_TEST(sorted_set_class_element_2773, tryadd_reports_compareto_duplicate) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<Ord>(); "
        "  using var a := new Ord(5); using var b := new Ord(5); "
        "  s.Add(a); "
        "  var added := s.TryAdd(b); "
        "  return (if added then 1 else 0); ")),
        0);
}

// per stdlib §4.18 (v0.286), fix #2773 (B1): `Contains` and `Remove` both key
// on `CompareTo`-equality — a reference-distinct, CompareTo-equal instance
// both finds and removes the stored element.
BEZEL_TEST(sorted_set_class_element_2773, contains_and_remove_use_compareto_equality) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<Ord>(); "
        "  using var a := new Ord(5); "
        "  s.Add(a); "
        "  using var probe := new Ord(5); "
        "  var found: int := (if s.Contains(probe) then 10 else 0); "
        "  var removed: int := (if s.Remove(probe) then 1 else 0); "
        "  return found + removed * 100 + s.Count; ")),
        /* Contains true (10) + Remove true (100) + Count 0 afterward */ 110);
}

// per stdlib §4.18 (v0.286): DISTINCT (non-CompareTo-equal) instances both
// land, and dedup does not over-fire.
BEZEL_TEST(sorted_set_class_element_2773, distinct_elements_all_land) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<Ord>(); "
        "  using var a := new Ord(1); using var b := new Ord(2); using var c := new Ord(3); "
        "  s.Add(a); s.Add(b); s.Add(c); "
        "  return s.Count; ")),
        3);
}
