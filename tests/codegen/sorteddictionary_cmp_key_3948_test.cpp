// Spec-derived regression tests for fix #3948: `SortedDictionary<K, V>` admits
// a non-hashable record KEY that declares a dispatchable single-parameter
// `CompareTo` — the legal §4.17 shape `SortedSet<T>`'s ELEMENT dispatch got
// from #3740 but whose KEY analogue had not been implemented.
//
// AUTHORITY. stdlib §4.17 (v0.294) declares, verbatim:
//
//     public class SortedDictionary<K, V>: IReadOnlyCollection<DictionaryEntry<K, V>>
//         where K: IComparable<K>
//
// under "Two keys are the same key exactly when `CompareTo` returns `0`" and
// "Ordering is defined by `K`'s `IComparable<K>.CompareTo`" — i.e. `K`'s sole
// constraint is `IComparable<K>`, no hashability requirement. §9.2 excludes a
// body record from auto-hashable when it declares an explicit FIELD (not an
// auto-property) that is not `equality skip`ped, so a record CAN satisfy
// §4.17's `IComparable<K>` constraint while failing §3.6's hashability test
// — exactly the shape this issue names.
//
// THE #3948 DEFECT (measured on 62db87b, pre-fix). `genericBackingType`
// (LlvmBackend_Calls3.cpp) admitted a `SortedSet<T>` RECORD element with a
// dispatchable `CompareTo` (fix #3740's `isSet && collTy->headName() ==
// "SortedSet"` exception), but the IDENTICAL exception had never been added
// for `SortedDictionary<K, V>`'s KEY — that arm was unconditionally `isSet`-
// gated, so for `isSet=false` it never fired. Net effect on
// `SortedDictionary<Cmp, int>` (where `Cmp` is a non-auto-hashable record
// declaring `CompareTo`): `genericBackingType` returned null, the
// `tryLowerCollectionInstanceCall` `isDict` block's `!keyTy` convention
// (which assumes `null = "the key is string"`, correct for `Dictionary` /
// `OwningDictionary`) fell through to the `bz_dict_*_str` fast path with a
// `%record.Cmp` struct fed into `%bz.string`-shaped GEP/load pairs, and
// `bezelc -c` aborted during LLVM IR verification (the
// `SortedDictionary` twin of fix #3865's own `SortedSet` `.Contains` /
// `.First` malformed-IR crash, with a DIFFERENT diagnostic text).
//
// Fix #3865's own commit body already noted the gap ("filed as issue 3948"),
// narrowed its diagnostic to a distinct "not yet supported in codegen"
// message rather than the false "does not implement IComparable" claim an
// actual non-comparable record (e.g. `NoCmp`) gets. The point of #3948 is
// that the gap is now closed: `genericBackingType`'s record-`CompareTo`
// exception is widened to also admit `!isSet && collTy->headName() ==
// "SortedDictionary"`, the downstream comparator builds were already in
// place (`buildSortedObjectKeyIdentity` from `lowerNew` already called for
// `colName == "SortedDictionary"`, fix #3488; `buildSortedKeyComparator`'s
// own record-`CompareTo` arm, fix #3740, for `FirstKey`/`LastKey`), and the
// two predicates agree on what shapes they admit and what they decline.
//
// This file is the end-to-end functional coverage. A second, narrower
// sibling test `sorteddictionary_cmp_key_add_not_yet_implemented` in
// sorted_set_sorted_dict_no_compareto_3865_test.cpp was REPLACED by
// `sorteddictionary_nocmp_key_still_rejected` (no-`CompareTo` still rejected,
// proving the widening does not falsely admit an illegal key).

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

// The VALUE channel for an observable outside 0..255 (WEXITSTATUS truncates).
std::string out(const std::string& src) { return runAotOut(src); }

// True when some diagnostic in the compile carries `needle` as a substring.
bool hasDiagnostic(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// `Cmp` is a non-auto-hashable record (an explicit field `Item: int` —
// not an auto-property — so §9.2 excludes it from auto-hashable; per §4.17
// "hashable" is irrelevant anyway, the SOLE constraint is `IComparable<Cmp>`)
// declaring a dispatchable single-parameter `CompareTo` — the legal §4.17
// key shape fix #3948 admits. Mirrors sorted_set_sorted_dict_no_compareto_
// 3865_test.cpp's own `Cmp` definition so the borrowed-parameter reachability
// shape (sema never checks `where K: IComparable<K>` for a non-`new`
// occurrence of a builtin generic, #3490) can be exercised here too; a
// class-typed field on the record would force the borrowed-receiver shape
// to surface §9.7/§28.11's "borrow-bearing record cannot be copied into a
// collection reachable beyond this method's scope" rejection, unrelated to
// #3948's eligibility widening.
const char* PRELUDE =
    "module M; import System; "
    "record Cmp: IComparable<Cmp> "
    "  public Item: int; "
    "  public constructor(i: int) Item := i; end constructor; "
    "  public method CompareTo(other: Cmp): int "
    "    return Item - other.Item; "
    "  end CompareTo; "
    "end Cmp; ";

std::string prog(const std::string& body) {
    return std::string(PRELUDE) +
           "class C public static method Main(): int " + body +
           " end Main; end C;";
}

// A borrowed-parameter program: `body` runs inside a static method that takes
// the receiver `paramDecl` as a PARAMETER (never constructed in-unit) — the
// exact reachability shape #3490/#3865 describe: sema never checks the
// builtin generic's constraint for a non-`new` occurrence of the type.
std::string borrowedParamProg(const std::string& paramDecl,
                              const std::string& body) {
    return std::string(PRELUDE) +
           "class C "
           "  public static method Run(" + paramDecl + "): int " + body +
           "    return 0; "
           "  end Run; "
           "  public static method Main(): int return 0; end Main; "
           "end C;";
}

}  // namespace

// ── Issue repro, verbatim in shape: §4.17's `where K: IComparable<K>` ──────
// ── admits a non-hashable record KEY with a dispatchable `CompareTo` ──────

// per stdlib §4.17 (v0.294), fix #3948: the issue's verbatim repro, MODIFIED
// to invoke `Run` so the program actually exercises the inserted key.
// Pre-#3948: clean compile-time diagnostic
//   "`SortedDictionary<K, V>` with a non-hashable record key that declares
//    `CompareTo` (`Cmp`) is not yet supported in codegen (stdlib §4.17)..."
// Post-#3948: program runs and returns 0.
BEZEL_TEST(sorteddictionary_cmp_key_3948, verbatim_issue_repro_now_runs) {
    BEZEL_EXPECT_EQ(run(borrowedParamProg(
        "d: SortedDictionary<Cmp, int>",
        "var k := new Cmp(5); d.Add(k, 1); ")),
        0);
}

// ── Construction + a real Add + a real Count ──────────────────────────────

// per stdlib §4.17 (v0.294), fix #3948: construction plus a real `Add` and a
// real `Count` read — not just "did not crash".
BEZEL_TEST(sorteddictionary_cmp_key_3948, construction_then_add_and_count) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<Cmp, int>(); "
        "  d.Add(new Cmp(5), 100); "
        "  return d.Count; ")),
        1);
}

// ── `CompareTo`-equality membership, not structural §9.1/§9.2 equality ────
// (stdlib §4.17: "Two keys are the same key exactly when `CompareTo`
// returns `0`" — and `Cmp`'s `CompareTo` only compares `Item`, so two
// freshly-constructed `Cmp` records with the same `Item` must dedup,
// exactly as fix #2773 (B1) pinned for a `SortedSet<T>` reference element
// and #3488 pinned for a `SortedDictionary<K, V>` class key.)

// per stdlib §4.17 (v0.294), fix #3948: `Add` of a `CompareTo`-equal key
// raises `ArgumentException` per §4.17's duplicate-is-a-bug tier (v0.282);
// the `TryAdd` shape below is the spec's `false`-and-no-mutation row.
BEZEL_TEST(sorteddictionary_cmp_key_3948, tryadd_reports_compareto_duplicate) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<Cmp, int>(); "
        "  d.Add(new Cmp(5), 100); "
        "  var added := d.TryAdd(new Cmp(5), 200); "
        "  return (if added then 1 else 0) + d.Count * 10; ")),
        /* TryAdd returns false (0) + Count remains 1 (10) */ 10);
}

// per stdlib §4.17 (v0.294), fix #3948: `ContainsKey` and `Remove` both
// key on `CompareTo`-equality — a freshly-constructed `Cmp` with the same
// `Item` value must still `ContainsKey`/`Remove` to true.
BEZEL_TEST(sorteddictionary_cmp_key_3948, contains_and_remove_use_compareto_equality) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<Cmp, int>(); "
        "  d.Add(new Cmp(5), 100); "
        "  var found := (if d.ContainsKey(new Cmp(5)) then 10 else 0); "
        "  var removed := (if d.Remove(new Cmp(5)) then 1 else 0); "
        "  return found + removed * 100 + d.Count; ")),
        /* ContainsKey true (10) + Remove true (100) + Count 0 afterward */ 110);
}

// per stdlib §4.17 (v0.294), fix #3948: a DISTINCT (non-`CompareTo`-equal)
// set of keys all land.
BEZEL_TEST(sorteddictionary_cmp_key_3948, distinct_keys_all_land) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<Cmp, int>(); "
        "  d.Add(new Cmp(1), 100); "
        "  d.Add(new Cmp(2), 200); "
        "  d.Add(new Cmp(3), 300); "
        "  return d.Count; ")),
        3);
}

// ── Ordering (stdlib §4.17's "iteration in ascending order") ──────────────

// per stdlib §4.17 (v0.294), fix #3948: `.FirstKey` / `.LastKey` via
// `buildSortedKeyComparator`'s record-`CompareTo` arm (from fix #3740)
// — already worked pre-fix (proved by
// `sorteddictionary_cmp_key_firstkey_still_works` in
// sorted_set_sorted_dict_no_compareto_3865_test.cpp); asserted here
// end-to-end against a CONSTRUCTED (not merely borrowed) receiver.
BEZEL_TEST(sorteddictionary_cmp_key_3948, firstkey_and_lastkey_on_constructed_receiver) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var d := new SortedDictionary<Cmp, int>(); "
        "  d.Add(new Cmp(3), 30); d.Add(new Cmp(1), 10); d.Add(new Cmp(2), 20); "
        "  Console.WriteLine(d.FirstKey.Item); "
        "  Console.WriteLine(d.LastKey.Item); "
        "  return 0; ")),
        std::string("1\n3"));
}

// per stdlib §4.17 (v0.294), fix #3948: full functional sweep — Add +
// TryAdd + ContainsKey + FirstKey + LastKey — returning one int that
// encodes each operation's outcome.
BEZEL_TEST(sorteddictionary_cmp_key_3948, full_functional_sweep) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var d := new SortedDictionary<Cmp, int>(); "
        "  d.Add(new Cmp(5), 100); "
        "  d.Add(new Cmp(10), 300); "
        "  var dup := (if d.TryAdd(new Cmp(5), 999) then 1 else 0); "
        "  d.Add(new Cmp(7), 200); "
        "  var has5 := (if d.ContainsKey(new Cmp(5)) then 1 else 0); "
        "  var has3 := (if d.ContainsKey(new Cmp(3)) then 1 else 0); "
        "  Console.WriteLine(d.FirstKey.Item); "
        "  Console.WriteLine(d.LastKey.Item); "
        "  Console.WriteLine(d.Count); "
        "  Console.WriteLine(dup + has5 * 10 + has3 * 100); "
        "  return 0; ")),
        /* FirstKey 5, LastKey 10, Count 3, dup(0) + has5(10) + has3(0) */ std::string("5\n10\n3\n10"));
}

// ── Borrowed-parameter reachability (#3490/#3865's reachability shape) ─────

// per stdlib §4.17 (v0.294), fix #3948: the exact reachability shape
// #3490/#3865 describe — sema never checks the builtin generic's
// constraint for a non-`new` occurrence, so a borrowed
// `SortedDictionary<Cmp, int>` parameter exercises the fix on a path
// where `Cmp` was never locally constructed. Asserted on a CONSTRUCTED
// borrowed pass-through (`Main` constructs, hands to `Run`, `Run`
// `ContainsKey`s) so the test compiles AND runs; the borrowed-only path
// (where `Main` never calls `Run`) is already covered by the existing
// `sorteddictionary_cmp_key_firstkey_still_works` row in
// sorted_set_sorted_dict_no_compareto_3865_test.cpp.
BEZEL_TEST(sorteddictionary_cmp_key_3948, containskey_over_constructed_then_borrowed) {
    BEZEL_EXPECT_EQ(run(std::string(PRELUDE) +
        "class C "
        "  public static method Run(d: SortedDictionary<Cmp, int>): int "
        "    return (if d.ContainsKey(new Cmp(5)) then 1 else 0); "
        "  end Run; "
        "  public static method Main(): int "
        "    var d := new SortedDictionary<Cmp, int>(); "
        "    d.Add(new Cmp(5), 100); "
        "    return (if C.Run(d) = 1 then 7 else 0); "
        "  end Main; "
        "end C;"),
        7);  // Main's Run call returns 1 (Cmp(5) is in the dictionary), then
             // 1 → 7 in the `if … then 7 else 0` mapping.
}

// ── Generic record exclusion, inherited from #3740 ─────────────────────────
//
// Per-fix-review parity with #3740: `recordHasDispatchableCompareTo` is the
// shared ELIGIBILITY predicate for the record-`CompareTo` exception, and
// `buildRecordCompareToThunk` is the actual comparator BUILDER. The two must
// agree on what they admit and what they decline — #3740's own review fix
// extended `recordHasDispatchableCompareTo` to exclude a GENERIC record so
// the BUILD side (which declines one) and the ELIGIBILITY side (which had
// previously admitted one) matched; widening the ELIGIBILITY for
// SortedDictionary in #3948 inherits that same shared exclusion. The
// GENERIC-RECORD key analogue of the SortedSet ELEMENT shape fix #3740
// already pinned (sorted_set_record_element_3740_test.cpp's
// `generic_record_construction_declines_loudly` /
// `generic_record_compareto_equal_elements_never_silently_double_count`) is
// deliberately NOT pinned HERE — that fix's own commit body noted
// "monomorphizing a generic record's `CompareTo` (`buildRecordCompareToThunk`
// learning to handle it, the alternative this fix declines) is a
// self-contained follow-up, not attempted here". #3948 inherits the same
// scope boundary, narrowing the fix to non-generic records exactly as
// #3740 did.