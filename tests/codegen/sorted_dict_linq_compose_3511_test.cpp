// Codegen + sema — fix #3511 / stdlib §4.17, §5 (v0.285):
// `SortedDictionary<K, V>` composing with the §5 LINQ operators.
//
// stdlib §4.17 declares the conformance that makes it legal, and says so in the
// document's own words:
//
//   public class SortedDictionary<K, V>: IReadOnlyCollection<DictionaryEntry<K, V>>
//     where K: IComparable<K>   // conformance added v0.165 (#1354): makes
//                               //   for-in/LINQ legal per spec §29
//
// Measured on fca2f02 (v0.285): `for var e in sd.Take(1) do … end` was rejected
//
//   `for` requires an iterable source: a type implementing `IEnumerable<T>`, an
//   inline array, a string, or a `Range<T>` (§21.4); this source is not iterable
//
// while the identical expression on a `Dictionary<K, V>` nine lines later was
// accepted, and a bare `for var e in sd` (no operator) also worked.
//
// TWO distinct defects, both pinned below:
//
//   1. sema (ExprTypecheck_Calls.cpp) — the shared Dictionary/SortedDictionary
//      block DOES type `sd.Take(1)` as a lazy `IEnumerable<DictionaryEntry<K,
//      V>>`; the `SortedDictionary` block far below then CLOBBERS that type back
//      to the bare value type through its catch-all V-return arm, so the `for`
//      sees an `int` source. That is the identical clobber #3372 recorded for
//      `TryAdd` and #2546 for `GetEnumerator` — each previously patched by
//      adding one more name. `Dictionary<K, V>` never had the bug only because
//      its own block has no catch-all.
//
//   2. codegen (LlvmBackend_Calls3.cpp) — the §5 materializer snapshots the
//      dictionary's entries in STORAGE order for every `isDictTypeName`
//      receiver, `SortedDictionary` included, and never sorted them. Fixing
//      sema alone therefore turns a loud rejection into a QUIET WRONG ANSWER:
//      measured, `sd.Take(1)` over a map built `Add(3,30); Add(1,10);
//      Add(2,20)` yielded key **3**, where §4.17's "iteration visits entries in
//      ascending key order" requires key **1**.
//
// So every row below asserts the ELEMENT ACTUALLY YIELDED, never merely that the
// program compiled: a row that only checked acceptance would pass over defect 2.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& mainBody) {
    return "module M; class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

// A sorted map built deliberately OUT of key order, so storage order and
// ascending order differ. Keys 3, 1, 2 inserted in that sequence.
const char* kOutOfOrderMap =
    "var sd := new SortedDictionary<int, int>(); "
    "sd.Add(3, 30); sd.Add(1, 10); sd.Add(2, 20); ";

}  // namespace

// ---------------------------------------------------------------------------
// stdlib §4.17 + §5 — the operator composes AND yields in ascending key order
// ---------------------------------------------------------------------------

// stdlib §4.17 ("iteration visits entries in ascending key order") + §5
// (`Take(count: int): IEnumerable<T>`). `Take(1)` must yield the LEAST key —
// key 1 — not the first-stored one (key 3, measured pre-fix).
// Encoded: key * 100 + value -> 1 * 100 + 10 = 110.
BEZEL_TEST(sorted_dict_linq_compose_3511, take_one_yields_the_least_key_4_17) {
    BEZEL_EXPECT_EQ(run(prog(std::string(kOutOfOrderMap) +
        "var k := 0; var v := 0; "
        "for var e in sd.Take(1) do k := e.Key; v := e.Value; end; "
        "return k * 100 + v; ")), 110);
}

// The whole prefix, in order: `Take(2)` must yield keys 1 then 2.
// Encoded: first * 10 + second -> 1 * 10 + 2 = 12.
BEZEL_TEST(sorted_dict_linq_compose_3511, take_two_yields_the_two_least_keys_in_order_4_17) {
    BEZEL_EXPECT_EQ(run(prog(std::string(kOutOfOrderMap) +
        "var acc := 0; "
        "for var e in sd.Take(2) do acc := acc * 10 + e.Key; end; "
        "return acc; ")), 12);
}

// The full sequence through a §5 operator must equal the sequence the BARE
// for-in produces — the two paths are different codegen (the §5 materializer in
// LlvmBackend_Calls3.cpp vs `wrapSortedDictionaryAsIEnumerable` in
// LlvmBackend_Calls2.cpp), and #3511's residue was exactly that they disagreed.
BEZEL_TEST(sorted_dict_linq_compose_3511, linq_chain_agrees_with_bare_for_in_4_17) {
    BEZEL_EXPECT_EQ(runAotOut(prog(std::string(kOutOfOrderMap) +
        "var a := \"\"; var b := \"\"; "
        "for var e in sd do a := a + $\"{e.Key}\"; end; "
        "for var e in sd.Take(3) do b := b + $\"{e.Key}\"; end; "
        "Console.WriteLine($\"{a}|{b}\"); "
        "return 0; ")),
        "123|123");
}

// stdlib §5 Slicing: `Skip` over the sorted source drops the LEAST keys.
// Encoded: remaining keys concatenated -> 23.
BEZEL_TEST(sorted_dict_linq_compose_3511, skip_drops_the_least_keys_4_17) {
    BEZEL_EXPECT_EQ(run(prog(std::string(kOutOfOrderMap) +
        "var acc := 0; "
        "for var e in sd.Skip(1) do acc := acc * 10 + e.Key; end; "
        "return acc; ")), 23);
}

// stdlib §5 Filtering: `Where` preserves the source order, so the survivors
// come out ascending.
// Encoded: keys 2 then 3 -> 23.
BEZEL_TEST(sorted_dict_linq_compose_3511, where_preserves_ascending_order_4_17) {
    BEZEL_EXPECT_EQ(run(prog(std::string(kOutOfOrderMap) +
        "var acc := 0; "
        "for var e in sd.Where(lambda(e) => e.Key > 1) do "
        "  acc := acc * 10 + e.Key; end; "
        "return acc; ")), 23);
}

// stdlib §5 Projection: `Select` over the entry element type.
// Encoded: values in ascending-key order, 10/20/30 -> 10*100 + 20*10 + 30 ... too
// wide for an 8-bit exit, so the digits are the KEYS: 1,2,3 -> 123.
BEZEL_TEST(sorted_dict_linq_compose_3511, select_over_entries_ascending_4_17) {
    BEZEL_EXPECT_EQ(run(prog(std::string(kOutOfOrderMap) +
        "var acc := 0; "
        "for var k in sd.Select(lambda(e) => e.Key) do acc := acc * 10 + k; end; "
        "return acc; ")), 123);
}

// ---------------------------------------------------------------------------
// The CARVE-OUT — the four §5 element-access terminals stay LOUD (issue 3576)
// ---------------------------------------------------------------------------
//
// `First()` / `Last()` / `Single()` / `ElementAt(i)` over an entry source do not
// lower on ANY member of the dictionary family, `Dictionary` included: codegen
// has no arm for an entry-struct element type there, so it emits the `#2434`
// "degraded to a zero-initialized value" NOTE — not an error — and the program
// compiles and yields a zero-filled entry. Measured on a plain
// `Dictionary<int, int>`: `dictFirst=0`, `dictLast=0`, `dictElemAt1=0`.
//
// The §5 typing hand-off this fix introduces therefore EXCLUDES those four
// names (`dictEntryElemTyped` in ExprTypecheck_Calls.cpp), so `sd.First().Key`
// keeps the loud rejection it had before the fix instead of joining the quiet
// path. The four rows below are that carve-out's only enforcement: without them
// a later edit could widen the hand-off and turn a compile error into a silent
// zero with no test noticing.
//
// They assert the diagnostic TEXT, not `hasErrors()` alone, so a row cannot pass
// on an unrelated error. When 3576 is fixed these four rows should be REPLACED
// by value assertions (the least entry for the sorted family, insertion-ordered
// for `Dictionary`) — not simply deleted.

namespace {

// The four terminals are typed as the bare value type by the sorted block's
// catch-all, so consuming the result as an entry — the only way an entry is ever
// used — is a "no member on primitive type" reject.
void expectEntryAccessStaysLoud(const std::string& call) {
    auto r = runAot(prog(std::string(kOutOfOrderMap) +
        "var f := " + call + "; "
        "return f.Key + f.Value; "));
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("no member `Key` on primitive type") !=
            std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
    // And it must be a REJECTION, not a note beside a produced binary.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

}  // namespace

// issue 3576 / stdlib §5: `First()` over a sorted source must still be rejected
// rather than silently yielding a zero-filled entry.
BEZEL_TEST(sorted_dict_linq_compose_3511, first_stays_a_loud_reject_3576) {
    expectEntryAccessStaysLoud("sd.First()");
}

// issue 3576 / stdlib §5: same for `Last()`.
BEZEL_TEST(sorted_dict_linq_compose_3511, last_stays_a_loud_reject_3576) {
    expectEntryAccessStaysLoud("sd.Last()");
}

// issue 3576 / stdlib §5: same for `Single()` — it sits in the same sema arm as
// the other three and so is in the carve-out, though it was not in 3576's
// originally stated scope.
BEZEL_TEST(sorted_dict_linq_compose_3511, single_stays_a_loud_reject_3576) {
    expectEntryAccessStaysLoud("sd.Single()");
}

// issue 3576 / stdlib §5: same for `ElementAt(i)`.
BEZEL_TEST(sorted_dict_linq_compose_3511, element_at_stays_a_loud_reject_3576) {
    expectEntryAccessStaysLoud("sd.ElementAt(1)");
}

// stdlib §5 Quantifiers: `Count()` over the sorted source. A cheap row, but it
// pins that the plain-terminal shape is typed too — pre-fix the catch-all
// clobber typed `sd.Count()` as V, which happens to also be `int` here, so this
// row is deliberately paired with the ordering rows above rather than standing
// alone as the acceptance oracle.
BEZEL_TEST(sorted_dict_linq_compose_3511, count_terminal_over_sorted_source_5) {
    BEZEL_EXPECT_EQ(run(prog(std::string(kOutOfOrderMap) +
        "return sd.Where(lambda(e) => e.Value > 10).Count(); ")), 2);
}

// stdlib §5 Materialization: `ToArray()` yields the entries ascending.
BEZEL_TEST(sorted_dict_linq_compose_3511, to_array_is_ascending_4_17) {
    BEZEL_EXPECT_EQ(run(prog(std::string(kOutOfOrderMap) +
        "using var a := sd.ToArray(); "
        "return a[0].Key * 100 + a[1].Key * 10 + a[2].Key; ")), 123);
}

// ---------------------------------------------------------------------------
// The STRING-key sorted map — a separate codegen edge (bz_sort_kv_str)
// ---------------------------------------------------------------------------

// stdlib §4.17: ordering is `K`'s `CompareTo`; for `string` that is the same
// ordinal ordering the for-in path gets from `bz_sort_kv_str`.
BEZEL_TEST(sorted_dict_linq_compose_3511, string_key_take_one_is_least_4_17) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var sd := new SortedDictionary<string, int>(); "
        "sd.Add(\"pear\", 3); sd.Add(\"apple\", 1); sd.Add(\"fig\", 2); "
        "var k := \"\"; "
        "for var e in sd.Take(1) do k := e.Key; end; "
        "Console.WriteLine(k); "
        "return 0; ")),
        "apple");
}

// ---------------------------------------------------------------------------
// The CONTROL — `Dictionary<K, V>` must be UNCHANGED
// ---------------------------------------------------------------------------

// stdlib §4.8: `Dictionary` iterates in INSERTION order, and the §5 chain over
// it must keep doing so. The fix sorts only the `SortedDictionary` receiver, so
// a map built 3, 1, 2 still yields 3, 1, 2 here. Without this row the sort could
// have been applied to the whole dict family and no other row would notice.
BEZEL_TEST(sorted_dict_linq_compose_3511, dictionary_linq_stays_insertion_ordered_4_8) {
    // The observable is 312, which does NOT fit an 8-bit exit status
    // (`runAot(...).exitCode` is WEXITSTATUS-masked — 312 & 0xFF = 56), so it
    // goes through the VALUE channel like the cross-check row above. Every
    // exit-code row in this file is deliberately <= 255.
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var d := new Dictionary<int, int>(); "
        "d.Add(3, 30); d.Add(1, 10); d.Add(2, 20); "
        "var acc := \"\"; "
        "for var e in d.Take(3) do acc := acc + $\"{e.Key}\"; end; "
        "Console.WriteLine(acc); "
        "return 0; ")),
        "312");
}

// The pre-fix diagnostic must be GONE — asserted by TEXT, so the row cannot
// pass on some unrelated error.
BEZEL_TEST(sorted_dict_linq_compose_3511, no_longer_rejected_as_not_iterable_21_4) {
    auto r = runAot(prog(std::string(kOutOfOrderMap) +
        "for var e in sd.Take(1) do Console.WriteLine($\"{e.Key}\"); end; "
        "return 0; "));
    for (const auto& d : r.diagnostics.diagnostics()) {
        BEZEL_EXPECT_TRUE(d.message.find("this source is not iterable") ==
                          std::string::npos);
    }
    BEZEL_EXPECT_TRUE(r.ran);
}
