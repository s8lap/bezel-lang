// Spec-derived regression tests for fix #3987: `SortedSet<T>`/
// `SortedDictionary<K, V>` used to dedup an element/key that is BOTH
// auto-hashable (stdlib §9.2) AND declares a dispatchable `CompareTo` by
// STRUCTURAL (§9.1/§9.2) equality instead of `CompareTo`-equality — a
// silent-wrong-runtime-value defect (`Count`, `Contains`, indexer lookups),
// not merely a missing diagnostic.
//
// AUTHORITY. stdlib §4.18 (v0.295), "A comparison-ordered set: membership by
// `CompareTo`-equality, iteration in ascending order" — "Two elements are the
// same element exactly when `CompareTo` returns `0`". stdlib §4.17 states the
// identical rule for `SortedDictionary<K, V>`'s key: "Two keys are the same
// key exactly when `CompareTo` returns `0`". Neither text carves out an
// exception for a key/element that ALSO happens to be hashable — §3.6/§9.2
// define hashability as a separate, independent property.
//
// THE DEFECT (measured on 6b0b167, post-#3947/#3865, verified on THIS tree
// before writing this file's fix, per the lane brief's own reproduction
// requirement):
//
//     module M;
//     record P
//       public property X: int;
//       public property Y: int;
//       public constructor(x: int, y: int) X := x; Y := y; end constructor;
//       public method CompareTo(other: P): int return X - other.X; end CompareTo;
//     end P;
//     class C
//       public static method Main(): int
//         var s := new SortedSet<P>();
//         s.Add(new P(1, 2));
//         s.Add(new P(1, 3));   // CompareTo(P(1,2), P(1,3)) == 0 -> "same element"
//         Console.WriteLine(s.Count);
//         return 0;
//       end Main;
//     end C;
//
// Compiled clean, ran, printed `2` — `P(1,2)` and `P(1,3)` are `CompareTo`-equal
// (both `X == 1`) but structurally distinct (`Y` differs), so BOTH were
// inserted where §4.18 requires ONE. `fix #3947`'s own regression file
// (`sortedset_sorteddict_contains_add_no_compareto_3947_test.cpp`) pinned this
// exact shape as a deliberate, documented "NARROW, unaffected" control
// (`sortedset_autohashable_record_with_compareto_unaffected` /
// `sorteddictionary_autohashable_record_key_with_compareto_unaffected`) — but
// those two rows both use a SINGLE-FIELD record (`AutoHashCmp`), where
// structural and `CompareTo` equality coincide by construction and so cannot
// distinguish the two behaviors; they are unaffected by this fix (verified in
// the accept-side sweep below, `existing_3947_single_field_control_unaffected`)
// and are NOT updated here.
//
// THE FIX (this file). `LlvmBackend_Calls3.cpp`'s `buildSortedObjectKeyIdentity`
// (the dedup/hash identity builder consulted once, at `lowerNew`) and
// `buildSortedKeyComparator` (the ordering comparator consulted by `.First`/
// `.Last`/`.FirstKey`/`.LastKey`/sorted iteration) each carried a
// `!recordTypeOfRef(keyTy)` guard that deliberately excluded an
// AUTO-HASHABLE record from `CompareTo`-based identity/ordering, even when it
// declares a dispatchable `CompareTo` — matching fix #3740's restraint for a
// DIFFERENT (non-hashable) record shape. That restraint is itself the bug for
// THIS shape: §4.17/§4.18 name `CompareTo`-equality as the SOLE membership
// rule, unconditionally. `buildRecordCompareToThunk` already gates on
// `recordHasDispatchableCompareTo` (record has a dispatchable single-parameter
// `CompareTo`, and is non-generic) internally, so dropping the
// `!recordTypeOfRef` guard is a no-op for a record with NO `CompareTo` (the
// #3947 rejection at every member still applies there) and changes ONLY the
// auto-hashable-WITH-`CompareTo` shape to `CompareTo`-based identity —
// fixing BOTH the identity builder and the comparator together, per issue
// #3987's own suggested resolution ("so hash/eq/ordering all agree"): fixing
// only the identity half would have left `.First`/`.Last` FALSELY rejecting
// (`"does not implement IComparable<T>"`, even though it does) a value
// `.Add`/`.Contains` now correctly accept — reintroducing the exact
// "accept by one member, reject by another" defect class #3947 closed, in
// the opposite direction.

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

// `P`: the issue's own shape — auto-hashable (two auto-properties, §9.2),
// `CompareTo` compares ONLY `X`, so `Y` distinguishes structural identity
// from `CompareTo` identity. `AutoHashCmp`: a SINGLE-field auto-hashable
// record with `CompareTo` — used for the `.First`/`.Last`/`.FirstKey`/
// `.LastKey` rows, where only ordering (not dedup) is under test, and for
// the existing-#3947-control-unaffected row.
const char* PRELUDE =
    "module M; "
    "record P "
    "  public property X: int; "
    "  public property Y: int; "
    "  public constructor(x: int, y: int) X := x; Y := y; end constructor; "
    "  public method CompareTo(other: P): int return X - other.X; "
    "    end CompareTo; "
    "end P; "
    "record AutoHashCmp "
    "  public property X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "  public method CompareTo(other: AutoHashCmp): int return X - other.X; "
    "    end CompareTo; "
    "end AutoHashCmp; ";

std::string prog(const std::string& body) {
    return std::string(PRELUDE) +
           "class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// ── THE ISSUE'S OWN REPRO, VERBATIM IN SHAPE ────────────────────────────────

// per stdlib §4.18 (v0.295), issue #3987's own repro: two `CompareTo`-equal
// (same `X`) but structurally-distinct (different `Y`) elements must dedup
// to ONE element, not two.
BEZEL_TEST(sortedset_sorteddict_compareto_identity_3987,
           verbatim_issue_repro_sortedset_dedups_by_compareto) {
    BEZEL_EXPECT_EQ(run(prog(
        "  var s := new SortedSet<P>(); "
        "  s.Add(new P(1, 2)); "
        "  s.Add(new P(1, 3)); "
        "  return s.Count; ")),
        1);
}

// ── SortedSet<T>: dedup + membership by CompareTo-equality ────────────────

BEZEL_TEST(sortedset_sorteddict_compareto_identity_3987,
           sortedset_contains_uses_compareto_equality) {
    // per §4.18: a query argument that is CompareTo-equal to a stored element
    // (same X, different Y) must report Contains == true, though it is
    // structurally distinct from every stored element.
    BEZEL_EXPECT_EQ(out(prog(
        "  var s := new SortedSet<P>(); "
        "  s.Add(new P(1, 2)); "
        "  Console.WriteLine((if s.Contains(new P(1, 5)) then 1 else 0)); "
        "  return 0; ")),
        std::string("1"));
}

BEZEL_TEST(sortedset_sorteddict_compareto_identity_3987,
           sortedset_tryadd_of_compareto_equal_element_declines) {
    // per §4.18: "adding an equal element leaves the set unmodified ...
    // TryAdd returns false".
    BEZEL_EXPECT_EQ(out(prog(
        "  var s := new SortedSet<P>(); "
        "  s.Add(new P(1, 2)); "
        "  var ok := s.TryAdd(new P(1, 9)); "
        "  Console.WriteLine((if ok then 1 else 0)); "
        "  Console.WriteLine(s.Count); "
        "  return 0; ")),
        std::string("0\n1"));
}

// ── SortedDictionary<K, V>: key dedup + lookup by CompareTo-equality ──────

BEZEL_TEST(sortedset_sorteddict_compareto_identity_3987,
           sorteddictionary_key_dedups_by_compareto) {
    // per §4.17: "Two keys are the same key exactly when CompareTo returns
    // 0" — TryAdd of a CompareTo-equal (but structurally distinct) key must
    // decline, Count stays 1, and a lookup with a THIRD CompareTo-equal key
    // (distinct Y again) must find the ORIGINALLY stored value (10, not 20 —
    // TryAdd never wrote its value since it declined).
    BEZEL_EXPECT_EQ(out(prog(
        "  var d := new SortedDictionary<P, int>(); "
        "  d.Add(new P(1, 2), 10); "
        "  var ok := d.TryAdd(new P(1, 9), 20); "
        "  Console.WriteLine((if ok then 1 else 0)); "
        "  Console.WriteLine(d.Count); "
        "  Console.WriteLine(d[new P(1, 999)]); "
        "  return 0; ")),
        std::string("0\n1\n10"));
}

BEZEL_TEST(sortedset_sorteddict_compareto_identity_3987,
           sorteddictionary_containskey_uses_compareto_equality) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var d := new SortedDictionary<P, int>(); "
        "  d.Add(new P(1, 2), 10); "
        "  Console.WriteLine((if d.ContainsKey(new P(1, 7)) then 1 else 0)); "
        "  return 0; ")),
        std::string("1"));
}

// ── Ordering: .First/.Last/.FirstKey/.LastKey no longer falsely reject ────
//
// Pre-fix, `buildSortedKeyComparator` carried the SAME `!recordTypeOfRef`
// exclusion, so `.First` on `SortedSet<AutoHashCmp>` (auto-hashable, HAS
// CompareTo) fell to the `!cmpFn` guard (fix #3956) and rejected with
// "does not implement IComparable<T>" — a FALSE claim, since AutoHashCmp
// does declare a dispatchable CompareTo. MEASURED pre-fix (this lane, on
// 6b0b167): exactly that rejection fired. This is the ordering half of
// #3987's fix — necessary so `.Add`/`.Contains`'s new CompareTo-based
// acceptance does not create a NEW accept-by-one-reject-by-another split in
// the opposite direction.

BEZEL_TEST(sortedset_sorteddict_compareto_identity_3987,
           sortedset_first_last_no_longer_falsely_reject) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var s := new SortedSet<AutoHashCmp>(); "
        "  s.Add(new AutoHashCmp(3)); s.Add(new AutoHashCmp(1)); "
        "  s.Add(new AutoHashCmp(5)); "
        "  Console.WriteLine(s.First.X); "
        "  Console.WriteLine(s.Last.X); "
        "  return 0; ")),
        std::string("1\n5"));
}

BEZEL_TEST(sortedset_sorteddict_compareto_identity_3987,
           sorteddictionary_firstkey_lastkey_no_longer_falsely_reject) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var d := new SortedDictionary<AutoHashCmp, int>(); "
        "  d.Add(new AutoHashCmp(3), 30); d.Add(new AutoHashCmp(1), 10); "
        "  d.Add(new AutoHashCmp(5), 50); "
        "  Console.WriteLine(d.FirstKey.X); "
        "  Console.WriteLine(d.LastKey.X); "
        "  return 0; ")),
        std::string("1\n5"));
}

// ── for-in iteration now visits in ascending CompareTo order ──────────────
//
// Pre-fix, `emitGenericKeyTableFor`'s sort call silently declined (`cmpFn`
// null via the same guard) and iteration fell back to hash/insertion order
// for an auto-hashable-with-CompareTo element — violating §4.18's "iteration
// in ascending order" for exactly this shape (a DIFFERENT residual from
// issue #3976's "no CompareTo at all" gap: here the element DOES have a
// CompareTo, so this is squarely within #3987's own architectural fix,
// not #3976's still-open indexer/for-in-over-NO-CompareTo residual).
BEZEL_TEST(sortedset_sorteddict_compareto_identity_3987,
           sortedset_forin_iterates_in_ascending_compareto_order) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var s := new SortedSet<P>(); "
        "  s.Add(new P(3, 0)); s.Add(new P(1, 0)); s.Add(new P(2, 0)); "
        "  for var e in s do Console.WriteLine(e.X); end; "
        "  return 0; ")),
        std::string("1\n2\n3"));
}

// ── Accept-side sweep (LANE_RULES §15): adjacent shapes must be unaffected ─

// The exact #3947 control rows this fix does NOT need to update: AutoHashCmp
// has a single field, so structural and CompareTo equality coincide and the
// printed values are identical whether dedup is structural or CompareTo-based
// — re-asserted here (mirroring the #3947 file's own rows) as a MEASURED
// confirmation that this fix leaves them unchanged, not an assumption.
BEZEL_TEST(sortedset_sorteddict_compareto_identity_3987,
           existing_3947_single_field_control_unaffected) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var s := new SortedSet<AutoHashCmp>(); "
        "  s.Add(new AutoHashCmp(3)); s.Add(new AutoHashCmp(1)); "
        "  s.Add(new AutoHashCmp(3)); "  // duplicate under EITHER identity notion
        "  Console.WriteLine(s.Count); "
        "  Console.WriteLine((if s.Contains(new AutoHashCmp(1)) then 1 else 0)); "
        "  return 0; ")),
        std::string("2\n1"));
}

// A record with NO CompareTo at all is still rejected at every member — this
// fix's `buildRecordCompareToThunk` call is a no-op for such a record
// (`recordHasDispatchableCompareTo` returns false internally), so the #3947
// rejection is untouched. `record Q(A: int)` is auto-hashable (single
// auto-property) with no CompareTo method declared.
BEZEL_TEST(sortedset_sorteddict_compareto_identity_3987,
           record_with_no_compareto_still_rejected_unaffected) {
    auto r = runAot(
        "module M; record Q(A: int); "
        "class C public static method Run(s: SortedSet<Q>): int "
        "  s.Add(new Q(1)); return 0; "
        "end Run; "
        "public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(
                "requires the element type to implement `IComparable<T>`") !=
            std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}
