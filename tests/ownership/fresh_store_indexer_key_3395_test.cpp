// Spec-derived regression for issue #3395 — §15.4's fresh-store warning at the
// INDEXER-SET KEY position.
//
// §15.4 *Collection literals* (v0.283) states the must-warn once, for both
// store sinks and BOTH positions, verbatim:
//
//   "Because the retention is statically certain to outlive the value, the
//    compiler **must warn** when a store-position argument on a
//    **borrowing-family** collection — `Add`/`Insert` and the indexer-set, key
//    and value positions alike — is a **fresh unbound owned temporary of class
//    type** (§28.3): a `new` expression, a collection literal, an
//    `owned`-returning call result, a `move` result, or a **`Task`-family call
//    result** (calls mint, §28.3) … The warning does not narrow the admission:
//    the program remains accepted, no `using var` is demanded, and the recorded
//    literal-only-rejection asymmetry above stands unchanged."
//
// `fix #3364` (04557b3) wired that mandate, but only three of its four cells:
// the storing-CALL sink answers for both positions (`Ownership_Checks.cpp`
// selects the key variant with `dictHead && i == 0`), while the indexer sink
// `checkIndexerSetBorrowStoreEscape` passed `isDictionaryKeyPosition=false`
// unconditionally and inspected only `as.value`. So the two sinks DISAGREED
// about one store — measured on 0e95d36:
//
//     d.Add(new Key(2), 7);      →  1 §15.4 fresh-store warning
//     d[new Key(1)] := 5;        →  0 diagnostics
//
// This file pins the symmetry. §15.4's severity note applies throughout: the
// warning "does not narrow the admission", so no program's acceptance changes
// — which makes OVER-warning, not under-warning, the failure mode here. The
// negative controls below are the larger half of the file for that reason.
//
// SCOPE, recorded. The index is examined only for a DICTIONARY head, because
// only there is it a store position: `d[k] := v` upserts, so an absent key is
// RETAINED (stdlib §4.8 / §4.17). An `Array`/`Stack`/`Queue`/`LinkedList` index
// selects a position and is retained in no slot. `OwningDictionary` stays
// outside this sink entirely (its indexer-set takes the value `owned`), exactly
// as the storing-call sink excludes it — §15.4 scopes the must-warn to "a
// store-position argument on a **borrowing-family** collection", and the two
// sinks stay symmetric. Whether an `OwningDictionary`'s (borrowing) KEY should
// draw the warning at BOTH sinks is filed as #3429 rather than decided here;
// `SortedSet`'s absence from both sinks' head lists is #3428 (unobservable
// today — a class-element `SortedSet` cannot be constructed at all).
//
// // per §15.4 (fresh-store warning, v0.243; store-position list quoted above),
//    §28.11 *Borrowing collections*, §28.3 *Unbound owned temporaries*

#include <string>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// `Key`/`Item` are trivial-dispose classes (§28.9); `Ord` additionally satisfies
// `SortedDictionary<K, V>`'s `where K: IComparable<K>`; `Pt` is a record, whose
// §28.11 element column "copies the value in" and so cannot dangle.
const char* kPreamble =
    "module M; "
    "record Pt(X: int, Y: int); "
    "class Key "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Key; "
    "class Item "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Item; "
    "class Ord: IComparable<Ord> "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  public method CompareTo(other: Ord): int return V - other.V; "
    "  end CompareTo; "
    "end Ord; ";

OwnershipResult check(const std::string& body) {
    return parseAndCheckOwnership(std::string(kPreamble) + body);
}

// Keyed on the stable rule identifier (the §-reference plus the rule's name),
// never on the whole sentence — the convention the #3364 test established.
int freshStoreWarnings(const OwnershipResult& r) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("§15.4 fresh-store warning") == std::string::npos)
            continue;
        ++n;
    }
    return n;
}

// The DOUBLE-FIRE GUARD, inherited from the #3364 test: counts every warning
// claiming a fresh unbound owned temporary whatever its wording, so a second
// predicate emitting a second message for one store is a red test rather than a
// quiet duplicate. It matters twice here: the key and the value of one
// indexer-set are two positions of ONE statement, and a naive wiring that warned
// the index for every head would double-count `a[0] := new Item(1)`.
int anyFreshTempWarnings(const OwnershipResult& r) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("fresh unbound owned temporary") == std::string::npos)
            continue;
        ++n;
    }
    return n;
}

// The text of the first §15.4 fresh-store warning mentioning `needle`, or "".
std::string freshStoreTextWith(const OwnershipResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("§15.4 fresh-store warning") == std::string::npos)
            continue;
        if (d.message.find(needle) == std::string::npos) continue;
        return d.message;
    }
    return std::string();
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ── The #3395 gap itself ─────────────────────────────────────────────────────

// §15.4: "the indexer-set, KEY and value positions alike". Pre-fix: 0.
BEZEL_TEST(fresh_store_indexer_key_3395, fresh_class_key_warns) {
    OwnershipResult r = check(
        "class C public static method M(): int "
        "  using var d := new Dictionary<Key, int>(); "
        "  d[new Key(1)] := 5; "
        "  return d.Count; "
        "end M; end C;");
    BEZEL_EXPECT_EQ(freshStoreWarnings(r), 1);
    BEZEL_EXPECT_EQ(anyFreshTempWarnings(r), 1);
    // "The warning does not narrow the admission" — it stays a Warning.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const std::string w = freshStoreTextWith(r, "key store");
    BEZEL_EXPECT_FALSE(w.empty());
    BEZEL_EXPECT_TRUE(contains(w, "K8 full-expression boundary"));
    BEZEL_EXPECT_TRUE(contains(w, "named holder"));
    BEZEL_EXPECT_TRUE(contains(w, "§28.11"));
    // The key variant omits the owning-family remedy: `OwningDictionary`'s key
    // position is borrowing exactly like `Dictionary`'s, so naming it would send
    // the reader to a remedy that does not work. This is the
    // `isDictionaryKeyPosition` flag's whole observable effect.
    BEZEL_EXPECT_FALSE(contains(w, "OwningDictionary"));
}

// THE ASYMMETRY #3395 NAMES: the two sinks of §15.4's one mandate must answer
// the same store the same way. Pre-fix the `Add` cell was 1 and the indexer
// cell 0.
BEZEL_TEST(fresh_store_indexer_key_3395, add_and_indexer_key_sinks_agree) {
    OwnershipResult viaAdd = check(
        "class C public static method M(): int "
        "  using var d := new Dictionary<Key, int>(); "
        "  d.Add(new Key(2), 7); "
        "  return d.Count; "
        "end M; end C;");
    OwnershipResult viaIndexer = check(
        "class C public static method M(): int "
        "  using var d := new Dictionary<Key, int>(); "
        "  d[new Key(2)] := 7; "
        "  return d.Count; "
        "end M; end C;");
    BEZEL_EXPECT_EQ(freshStoreWarnings(viaAdd), 1);
    BEZEL_EXPECT_EQ(freshStoreWarnings(viaIndexer), 1);
    BEZEL_EXPECT_EQ(freshStoreWarnings(viaAdd), freshStoreWarnings(viaIndexer));
}

// "key and value positions alike" — one statement, two store positions, two
// warnings, and the key's is the one that omits the owning-family remedy.
BEZEL_TEST(fresh_store_indexer_key_3395, fresh_key_and_fresh_value_both_warn) {
    OwnershipResult r = check(
        "class C public static method M(): int "
        "  using var d := new Dictionary<Key, Item>(); "
        "  d[new Key(1)] := new Item(2); "
        "  return d.Count; "
        "end M; end C;");
    BEZEL_EXPECT_EQ(freshStoreWarnings(r), 2);
    BEZEL_EXPECT_EQ(anyFreshTempWarnings(r), 2);
    BEZEL_EXPECT_FALSE(freshStoreTextWith(r, "key store").empty());
    const std::string v = freshStoreTextWith(r, "coll[i]");
    BEZEL_EXPECT_FALSE(v.empty());
    BEZEL_EXPECT_TRUE(contains(v, "OwningDictionary"));
}

// `SortedDictionary<K, V>` is a borrowing-family map with the same upsert
// indexer (stdlib §4.17 "Value semantics match `Dictionary<K, V>`"), and it is
// in this sink's head list — so its key position answers identically. Its
// warning names NO owning remedy at all (there is no `OwningSortedDictionary`).
BEZEL_TEST(fresh_store_indexer_key_3395, sorted_dictionary_fresh_key_warns) {
    OwnershipResult r = check(
        "class C public static method M(): int "
        "  using var sd := new SortedDictionary<Ord, int>(); "
        "  sd[new Ord(1)] := 5; "
        "  return sd.Count; "
        "end M; end C;");
    BEZEL_EXPECT_EQ(freshStoreWarnings(r), 1);
    const std::string w = freshStoreTextWith(r, "key store");
    BEZEL_EXPECT_FALSE(w.empty());
    BEZEL_EXPECT_TRUE(contains(w, "SortedDictionary"));
    BEZEL_EXPECT_FALSE(contains(w, "use the owning family"));
}

// §15.4's fresh set is `producerState`'s §28.3 producers table, not a shape list
// local to this sink: "an `owned`-returning call result" and "a `move` result"
// are members of it and reach the key position through the same one predicate.
BEZEL_TEST(fresh_store_indexer_key_3395, owned_call_and_move_keys_warn) {
    OwnershipResult call = check(
        "class C "
        "  public static method MakeKey(): owned Key return new Key(3); "
        "  end MakeKey; "
        "  public static method M(): int "
        "    using var d := new Dictionary<Key, int>(); "
        "    d[MakeKey()] := 5; "
        "    return d.Count; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_EQ(freshStoreWarnings(call), 1);

    OwnershipResult mv = check(
        "class C public static method M(): int "
        "  using var d := new Dictionary<Key, int>(); "
        "  var k := new Key(1); "
        "  d[move k] := 5; "
        "  return d.Count; "
        "end M; end C;");
    BEZEL_EXPECT_EQ(freshStoreWarnings(mv), 1);
}

// ── Negative controls: over-warning is this fix's failure mode ───────────────

// The documented remedy: a named holder is not an unbound temporary — in the
// key position exactly as in the value position.
BEZEL_TEST(fresh_store_indexer_key_3395, named_holder_key_does_not_warn) {
    OwnershipResult r = check(
        "class C public static method M(k: Key, it: Item): int "
        "  using var d := new Dictionary<Key, Item>(); "
        "  d[k] := it; "
        "  using var k2 := new Key(9); "
        "  d[k2] := it; "
        "  return d.Count; "
        "end M; end C;");
    // Non-vacuity: a source that failed to compile would also yield 0 warnings.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(freshStoreWarnings(r), 0);
    BEZEL_EXPECT_EQ(anyFreshTempWarnings(r), 0);
}

// §15.4 scopes the population to a fresh temporary of CLASS type; §28.11's
// element table puts the others in columns whose store cannot dangle — a value
// type "copies the value in", a runtime-managed reference "stores the
// reference". `int`, `string` and `record` keys are all silent.
BEZEL_TEST(fresh_store_indexer_key_3395, value_string_and_record_keys_do_not_warn) {
    OwnershipResult r = check(
        "class C public static method M(s: string, it: Item): int "
        "  using var di := new Dictionary<int, Item>(); "
        "  di[1 + 2] := it; "
        "  using var ds := new Dictionary<string, Item>(); "
        "  ds[s + \"x\"] := it; "
        "  using var dp := new Dictionary<Pt, Item>(); "
        "  dp[new Pt(1, 2)] := it; "
        "  return di.Count + ds.Count + dp.Count; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(freshStoreWarnings(r), 0);
    BEZEL_EXPECT_EQ(anyFreshTempWarnings(r), 0);
}

// THE INDEX OF A NON-DICTIONARY HEAD IS NOT A STORE POSITION. `a[0] := v`
// selects a slot; nothing retains `0`. This is also the double-count guard: the
// fresh VALUE must still warn exactly once, not once per position.
BEZEL_TEST(fresh_store_indexer_key_3395, array_index_is_not_a_store_position) {
    OwnershipResult r = check(
        "class C public static method M(t: Item): int "
        "  using var a := new Array<Item>(); "
        "  a.Add(t); "
        "  a[0] := new Item(2); "
        "  return a.Count; "
        "end M; end C;");
    // Exactly one — the value's. Unchanged from the #3364 baseline.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(freshStoreWarnings(r), 1);
    BEZEL_EXPECT_EQ(anyFreshTempWarnings(r), 1);
    BEZEL_EXPECT_FALSE(freshStoreTextWith(r, "coll[i]").empty());
    BEZEL_EXPECT_TRUE(freshStoreTextWith(r, "key store").empty());
}

// `OwningDictionary`'s indexer-set takes the value `owned` (a §28.8 transfer,
// not a borrow store), so the whole sink declines it — as the storing-call sink
// declines it too. §15.4's must-warn is scoped to "a borrowing-family
// collection"; warning here would contradict the remedy the warning itself
// names.
BEZEL_TEST(fresh_store_indexer_key_3395, owning_dictionary_indexer_set_does_not_warn) {
    OwnershipResult r = check(
        "class C public static method M(): int "
        "  using var od := new OwningDictionary<string, Item>(); "
        "  od[\"a\"] := new Item(1); "
        "  return od.Count; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(freshStoreWarnings(r), 0);
    BEZEL_EXPECT_EQ(anyFreshTempWarnings(r), 0);
}

// A user type's own indexer carries its own §28.14 obligations, not this
// collection-specific rule: the sink requires a builtin head (a Named TypeRef
// with a null declaration), so a user indexer taking a class-typed index is
// untouched.
BEZEL_TEST(fresh_store_indexer_key_3395, user_declared_indexer_does_not_warn) {
    OwnershipResult r = check(
        "class Table "
        "  private _n: int := 0; "
        "  public property [k: Key]: int get => _n + k.V; "
        "                             set => Console.WriteLine(value); "
        "  end; "
        "end Table; "
        "class C public static method M(): int "
        "  using var t := new Table(); "
        "  t[new Key(1)] := 5; "
        "  return 0; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(freshStoreWarnings(r), 0);
    BEZEL_EXPECT_EQ(anyFreshTempWarnings(r), 0);
}

// ── Admission is unchanged (§15.4: "the program remains accepted") ───────────

// End-to-end through the real driver: the warned program still compiles, links
// and runs, and both entries are present. `runAot` discards compiler stderr on a
// successful compile, so this asserts the ADMISSION half only — the warning
// counts above are the ownership-pass half.
BEZEL_TEST(fresh_store_indexer_key_3395, warned_program_still_compiles_and_runs) {
    auto r = runAot(
        std::string(kPreamble) +
        "class C public static method Main(): int "
        "  using var d := new Dictionary<Key, int>(); "
        "  d[new Key(1)] := 5; "
        "  d[new Key(2)] := 7; "
        "  Console.WriteLine(\"count=\" + d.Count.ToString()); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("count=2") != std::string::npos);
}
