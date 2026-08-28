// Spec-derived regression tests for fix #3488 — `SortedDictionary<K, V>` with a
// REFERENCE key (a class or an interface that satisfies `IComparable<K>`) obeys
// stdlib §4.17's ordering AND its identity rule, instead of silently obeying
// neither.
//
// AUTHORITY. stdlib §4.17 (v0.283) declares, verbatim:
//
//     public class SortedDictionary<K, V>: IReadOnlyCollection<DictionaryEntry<K, V>>
//                                          where K: IComparable<K>
//       public constructor();
//       ...
//       public method Add(key: K, value: V);
//       public method TryAdd(key: K, value: V): bool;
//       ...
//       public pure readonly property FirstKey: K;   // least key; InvalidOperationException if empty
//       public pure readonly property LastKey: K;    // greatest key; InvalidOperationException if empty
//     end SortedDictionary;
//
// and states the two rules this file tests, both verbatim:
//
//     "Ordering is defined by `K`'s `IComparable<K>.CompareTo`"
//     "Two keys are the same key exactly when `CompareTo` returns `0`."
//     "iteration is **O(n)** in ascending key order"
//
// with `Add` carrying "v0.282: ArgumentException if the key is present" and
// `TryAdd` "v0.282 (§1 shape (d)): false and no mutation if present".
//
// THE DEFECT (measured on ef339fa, three ways, all silent — the construction
// compiles):
//
//   1. Iteration came out in INSERTION order. Keys added 3, 1, 2 printed
//      3, 1, 2 while the `int` control correctly printed 1, 2, 3.
//   2. `FirstKey` / `LastKey` did not lower at all, so lowerLocalDecl degraded
//      the initializer (#2434) to a zero-initialised local, and the first
//      member read off it SEGV'd — "AddressSanitizer: SEGV on unknown address
//      0x00000000000c (READ)", offset 12 being the field's offset in a null
//      object. Memory-unsafe, not merely wrong.
//   3. Two `CompareTo`-equal keys were stored as DISTINCT keys: `Ord(5)` then a
//      second `Ord(5)` gave `Count == 2` with `TryAdd` returning `true`, where
//      §4.17 makes them one key.
//
// THE CAUSE, AND WHY THE FILED ROOT CAUSE WAS ONLY HALF RIGHT. #3488 (and
// #3428's commit body before it) recorded that "there is no
// `CompareTo`-through-the-vtable comparator anywhere in the compiler", and
// deferred the whole sorted-reference-collection family on that ground. That
// was measured wrong: `Array<T>.Sort()` has built exactly such a thunk since
// #1195 (`bz$classcmp$<C>`) and #2768 (`bz$ifacecmp$<I>`), at the IDENTICAL
// `i32 (ptr, ptr)` slot-address ABI `buildSortedKeyComparator` needs. Only the
// wiring was missing: `buildSortedKeyComparator` switched on the key's LLVM
// TYPE and a reference key lowers to `ptr`, which took its `return nullptr`
// arm — and every consumer of a null comparator degrades quietly (iteration
// keeps the unsorted snapshot; FirstKey/LastKey fail to lower).
//
// Defect 3 is a separate rule and needed a separate fix: the generic-key
// table's stored identity for a reference key is #2773's §8.3
// `Equals`/`GetHashCode` pair — reference identity, which is right for
// `Dictionary` (§4.8) and is precisely NOT §4.17's rule. `SortedDictionary`
// alone now stores such a key under (constant hash, `CompareTo == 0`), the only
// pair that satisfies both a hash table's `eq ⇒ equal hashes` contract and
// §4.17's identity rule for references. The recorded cost is §4.17's
// "Point operations … are **O(log n)**" clause, which that pair makes O(n) for
// a reference key; the ordered backing that would restore it is filed as a
// follow-up.
//
// EVERY OBSERVABLE HERE IS A VALUE, NOT "IT COMPILED". Ordering rows print the
// visited keys and compare the string, because an exit code cannot distinguish
// insertion order from ascending order when both are permutations of the same
// keys. `runAotOut` carries observables outside 0..255, which WEXITSTATUS would
// mask.
//
// The second half of the file is the FALSE-REJECT / NO-COLLATERAL guard. Every
// key category that already worked must still work and still give the same
// answer, and the two identity decisions this fix did NOT retarget —
// `Dictionary`'s §8.3 reference identity and a reference key that declares no
// `CompareTo` — must be bit-for-bit unchanged. Every row in that half passes on
// the PRE-fix compiler too, which is what makes them controls rather than
// restatements of the fix.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// The VALUE channel: a process exit status is masked to 8 bits, so an ordering
// observable travels on stdout. Reads `stdoutText` DIRECTLY rather than through
// `runAotOut`, which "returns the program's stdout with a SINGLE trailing
// line-ending stripped" — its own header prescribes this shape for a test that
// "prints MULTIPLE lines". The `<did-not-run>` sentinel is kept so a rejected or
// crashed program is distinguishable from a merely wrong value: `runAot`'s
// WIFSIGNALED path reports ran=false, which is how the pre-fix `FirstKey` SEGV
// would show up here.
std::string out(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// `Ord` satisfies §4.17's `where K: IComparable<K>`. `IOrd`/`Node` are the
// interface-key shape (spec §14.2 conformance rule 3). `Plain` is a class that
// declares NO `CompareTo` — the shape nothing checks the constraint for
// (#3490), which must keep its pre-fix behaviour exactly.
const char* PRELUDE =
    "module M; import System.Collections.Generic; "
    "record Rec(A: int); "
    "enum Col  Red, Green;  end Col; "
    "class Ord: IComparable<Ord> "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  public method CompareTo(other: Ord): int return V - other.V; end CompareTo; "
    "end Ord; "
    "interface IOrd: IComparable<IOrd> "
    "  method Val(): int; "
    "end IOrd; "
    "class Node: IOrd "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  public method Val(): int return V; end Val; "
    "  public method CompareTo(other: IOrd): int return V - other.Val(); end CompareTo; "
    "end Node; "
    "class Plain "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Plain; "
    "class Virt: IComparable<Virt> "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  public virtual method CompareTo(other: Virt): int return V - other.V; end CompareTo; "
    "end Virt; "
    "class Desc: Virt "
    "  public constructor(v: int) base(v); end constructor; "
    "  public override method CompareTo(other: Virt): int return other.V - V; end CompareTo; "
    "end Desc; ";

std::string prog(const std::string& body) {
    return std::string(PRELUDE) +
           "class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// ── §4.17's ordering rule now holds for a reference key ─────────────────────

// per stdlib §4.17 (v0.283), #3488: "iteration is **O(n)** in ascending key
// order". The filed repro's exact shape — keys added 3, 1, 2. Pre-fix this
// printed "312" (insertion order); the `int` control in the same program
// printed "123" throughout, which is what made the defect a silent one.
BEZEL_TEST(sorted_dictionary_class_key_3488, class_key_iterates_ascending) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var o1 := new Ord(3); "
        "  using var o2 := new Ord(1); "
        "  using var o3 := new Ord(2); "
        "  var sd := new SortedDictionary<Ord, int>(); "
        "  sd.Add(o1, 10); sd.Add(o2, 20); sd.Add(o3, 30); "
        "  var sb := new StringBuilder(); "
        "  for var e in sd do sb.Append($\"{e.Key.V}:{e.Value},\"); end; "
        "  Console.WriteLine(sb.ToString()); "
        "  return 0; ")),
        "1:20,2:30,3:10,\n");
}

// per stdlib §4.17 (v0.283), #3488: `FirstKey` is documented "least key;
// InvalidOperationException if empty" and `LastKey` "greatest key;
// InvalidOperationException if empty". Pre-fix BOTH reads failed to
// lower, lowerLocalDecl degraded them to zero-initialised locals (#2434), and
// reading `.V` off one SEGV'd at address 0xc under ASan. A crash is what this
// row's VALUE observable rules out — the harness reports a signalled run as
// "did not run", so a wrong-but-alive answer and a crash are distinguishable.
BEZEL_TEST(sorted_dictionary_class_key_3488, class_key_first_and_last_key) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var o1 := new Ord(3); "
        "  using var o2 := new Ord(1); "
        "  using var o3 := new Ord(2); "
        "  var sd := new SortedDictionary<Ord, int>(); "
        "  sd.Add(o1, 10); sd.Add(o2, 20); sd.Add(o3, 30); "
        "  var fk := sd.FirstKey; "
        "  var lk := sd.LastKey; "
        "  Console.WriteLine($\"first={fk.V} last={lk.V}\"); "
        "  return 0; ")),
        "first=1 last=3\n");
}

// ── §4.17's identity rule now holds for a reference key ─────────────────────

// per stdlib §4.17 (v0.283), #3488: "Two keys are the same key exactly when
// `CompareTo` returns `0`", with `TryAdd` "false and no mutation if present".
// `Ord(5)` and a second, DISTINCT `Ord(5)` are one key. Pre-fix this printed
// "tryAdd=True count=3" — the §8.3 reference-identity pair the generic table
// gives a class key (#2773), which is `Dictionary`'s rule, not the sorted
// map's.
BEZEL_TEST(sorted_dictionary_class_key_3488, compareto_equal_keys_are_one_key) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var a := new Ord(5); "
        "  using var b := new Ord(7); "
        "  using var c := new Ord(5); "
        "  var sd := new SortedDictionary<Ord, int>(); "
        "  sd.Add(a, 10); sd.Add(b, 20); "
        "  Console.WriteLine($\"tryAdd={sd.TryAdd(c, 30)} count={sd.Count}\"); "
        "  return 0; ")),
        "tryAdd=False count=2\n");
}

// per stdlib §4.17 (v0.283), #3488: `Add` carries "v0.282: ArgumentException if
// the key is present — the §4.8 duplicate-is-a-bug tier". With the two keys
// being the SAME key under `CompareTo`, the second `Add` must raise. Pre-fix it
// returned normally and the map held both.
BEZEL_TEST(sorted_dictionary_class_key_3488, compareto_equal_key_add_raises) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var a := new Ord(5); "
        "  using var b := new Ord(5); "
        "  var sd := new SortedDictionary<Ord, int>(); "
        "  sd.Add(a, 10); "
        "  try "
        "    sd.Add(b, 20); "
        "    Console.WriteLine(\"no-throw\"); "
        "  catch "
        "    var e: ArgumentException => Console.WriteLine(\"ArgumentException\") "
        "  end; "
        "  Console.WriteLine($\"count={sd.Count}\"); "
        "  return 0; ")),
        "ArgumentException\ncount=1\n");
}

// per stdlib §4.17 (v0.283), #3488: the indexer's "set: insert or replace" and
// the read-through must agree with `TryAdd` on what "the same key" is — an
// assignment through a `CompareTo`-equal key REPLACES rather than inserting,
// and `ContainsKey` / `Remove` find the entry through a different instance.
// Pre-fix each of these three saw a distinct key.
BEZEL_TEST(sorted_dictionary_class_key_3488, compareto_equal_key_indexer_and_remove) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var a := new Ord(5); "
        "  using var b := new Ord(5); "
        "  var sd := new SortedDictionary<Ord, int>(); "
        "  sd.Add(a, 10); "
        "  sd[b] := 99; "
        "  Console.WriteLine($\"count={sd.Count} val={sd[a]} has={sd.ContainsKey(b)}\"); "
        "  Console.WriteLine($\"removed={sd.Remove(b)} count={sd.Count}\"); "
        "  return 0; ")),
        "count=1 val=99 has=True\nremoved=True count=0\n");
}

// per stdlib §4.17 (v0.283) + spec §14.2 conformance rule 3, #3488: an
// INTERFACE key. §14.2 admits an interface substitution for `where K:
// IComparable<K>` — "**T is an interface** that has I in its extension chain" —
// and the ordering dispatch is #2768's §13 itable thunk rather than #1195's
// vtable one, so it is a genuinely different code path and gets its own row.
BEZEL_TEST(sorted_dictionary_class_key_3488, interface_key_orders_and_dedups) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var n1 := new Node(3); "
        "  using var n2 := new Node(1); "
        "  using var n3 := new Node(3); "
        "  var sd := new SortedDictionary<IOrd, int>(); "
        "  sd.Add(n1, 10); sd.Add(n2, 20); "
        "  Console.WriteLine($\"tryAdd={sd.TryAdd(n3, 30)} count={sd.Count}\"); "
        "  var sb := new StringBuilder(); "
        "  for var e in sd do sb.Append($\"{e.Key.Val()},\"); end; "
        "  Console.WriteLine(sb.ToString()); "
        "  Console.WriteLine($\"first={sd.FirstKey.Val()} last={sd.LastKey.Val()}\"); "
        "  return 0; ")),
        "tryAdd=False count=2\n1,3,\nfirst=1 last=3\n");
}

// per stdlib §4.17 (v0.283), #3488: the identity pair for a reference key makes
// every key share one probe cluster, so this row exercises what a 3-entry row
// cannot — repeated REHASHES (the table starts at capacity 8 and grows at 75%
// load), a duplicate rejected at scale, and a REMOVE leaving a tombstone that
// later probes must skip without losing the entries behind it. 40 keys are
// inserted in DESCENDING order so insertion order and ascending order are
// maximally different.
BEZEL_TEST(sorted_dictionary_class_key_3488, class_key_survives_rehash_and_tombstones) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var big := new OwningArray<Ord>(); "
        "  var sd := new SortedDictionary<Ord, int>(); "
        "  var i := 0; "
        "  while i < 40 do "
        "    var k := new Ord(39 - i); "
        "    big.Add(owned k); "
        "    sd.Add(big[i], i); "
        "    i := i + 1; "
        "  end; "
        "  using var dupe := new Ord(20); "
        "  Console.WriteLine($\"count={sd.Count} first={sd.FirstKey.V} last={sd.LastKey.V}\"); "
        "  Console.WriteLine($\"dupe={sd.TryAdd(dupe, 999)} removed={sd.Remove(dupe)} count={sd.Count}\"); "
        "  var sb := new StringBuilder(); "
        "  for var e in sd do sb.Append($\"{e.Key.V},\"); end; "
        "  Console.WriteLine(sb.ToString()); "
        "  big.Dispose(); "
        "  return 0; ")),
        "count=40 first=0 last=39\n"
        "dupe=False removed=True count=39\n"
        "0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,"
        "21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,\n");
}

// per spec §12 (virtual dispatch) + stdlib §4.17 (v0.283), #3488: the comparator
// thunk has TWO arms — a vtable load when the key's `CompareTo` occupies a
// vtable slot, and a direct call to the compiled body otherwise — and only the
// direct arm is exercised by the rows above. `Desc` OVERRIDES `Virt`'s virtual
// `CompareTo` to reverse the order, and the keys are stored at the BASE type, so
// the base's ascending body and the override's descending one give opposite
// answers: `first`/`last` come out 2 and 1 only if the override was dispatched.
// A row that used the base body would read "first=1 last=2" and look correct in
// every other respect, which is exactly why the observable is the reversal.
BEZEL_TEST(sorted_dictionary_class_key_3488, virtual_compareto_dispatches_the_override) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var d1 := new Desc(1); "
        "  using var d2 := new Desc(2); "
        "  var sv := new SortedDictionary<Virt, int>(); "
        "  sv.Add(d1, 1); sv.Add(d2, 2); "
        "  var sb := new StringBuilder(); "
        "  for var e in sv do sb.Append($\"{e.Key.V},\"); end; "
        "  Console.WriteLine(sb.ToString()); "
        "  Console.WriteLine($\"first={sv.FirstKey.V} last={sv.LastKey.V}\"); "
        "  return 0; ")),
        "2,1,\nfirst=2 last=1\n");
}

// per stdlib §4.17 (v0.283), #3488: the thunk is built DURING the lowering of
// the function that constructs the map, and it resolves the key's `CompareTo`
// through `findMethodByName`, which demands a compiled body (`methodFns_`). This
// row puts the key class AFTER its only use in source order, so a build that
// depended on declaration order would silently find no `CompareTo`, fall back to
// the §8.3 pair, and print insertion order with `tryAdd=True count=3` — a
// failure mode indistinguishable from the original defect.
BEZEL_TEST(sorted_dictionary_class_key_3488, key_class_declared_after_its_use) {
    BEZEL_EXPECT_EQ(out(std::string(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var o1 := new Late(3); "
        "  using var o2 := new Late(1); "
        "  using var o3 := new Late(3); "
        "  var sd := new SortedDictionary<Late, int>(); "
        "  sd.Add(o1, 10); sd.Add(o2, 20); "
        "  Console.WriteLine($\"tryAdd={sd.TryAdd(o3, 30)} count={sd.Count}\"); "
        "  var sb := new StringBuilder(); "
        "  for var e in sd do sb.Append($\"{e.Key.V},\"); end; "
        "  Console.WriteLine(sb.ToString()); "
        "  return 0; "
        " end Main; end C; "
        "class Late: IComparable<Late> "
        "  public V: int := 0; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public method CompareTo(other: Late): int return V - other.V; end CompareTo; "
        "end Late;")),
        "tryAdd=False count=2\n1,3,\n");
}

// ── NO-COLLATERAL GUARD: every row below also passes on the PRE-fix compiler ─

// per stdlib §4.8 (v0.283) + spec §8.3, #3488: `Dictionary<K, V>`'s identity is
// NOT retargeted. §8.3 gives a class reference identity and §4.8 keys on the
// `Equals`/`GetHashCode` pair, so two `CompareTo`-equal-but-distinct `Ord`s are
// two keys there. This is the single most important control in the file: the
// fix reaches the shared generic-key table, and this row is what pins that it
// reaches it only for the collection §4.17 names.
BEZEL_TEST(sorted_dictionary_class_key_3488, dictionary_class_key_keeps_reference_identity) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var a := new Ord(5); "
        "  using var b := new Ord(5); "
        "  var d := new Dictionary<Ord, int>(); "
        "  d.Add(a, 10); "
        "  Console.WriteLine($\"tryAdd={d.TryAdd(b, 20)} count={d.Count}\"); "
        "  return 0; ")),
        "tryAdd=True count=2\n");
}

// per stdlib §4.17 (v0.283), #3488: a reference key that declares no
// `CompareTo` has no comparator to dispatch, so it keeps its pre-fix §8.3 pair
// unchanged rather than acquiring a rejection this fix did not measure. §4.17's
// `where K: IComparable<K>` is unchecked for a builtin generic (#3490); this
// row pins the behaviour that gap leaves, so the day the constraint IS checked
// this row is the one that has to move.
BEZEL_TEST(sorted_dictionary_class_key_3488, key_without_compareto_unchanged) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var p1 := new Plain(5); "
        "  using var p2 := new Plain(5); "
        "  var sd := new SortedDictionary<Plain, int>(); "
        "  sd.Add(p1, 1); "
        "  Console.WriteLine($\"tryAdd={sd.TryAdd(p2, 2)} count={sd.Count}\"); "
        "  return 0; ")),
        "tryAdd=True count=2\n");
}

// per stdlib §4.17 (v0.283) + spec §8.1, #3488: an `Object` key is a reference key
// (`hasObjectHeaderEquality` admits it) with no `CompareTo` to dispatch, so it
// too is untouched — it must keep constructing and counting, not crash on a
// comparator that was never built.
BEZEL_TEST(sorted_dictionary_class_key_3488, object_key_still_constructs) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var o := new Ord(1); "
        "  var sd := new SortedDictionary<Object, int>(); "
        "  sd.Add(o, 7); "
        "  return sd.Count; ")),
        1);
}

// per stdlib §4.17 (v0.283), #3488: the `int` key — the control the filed issue
// used to show the class key was wrong. Ordering and value identity are both
// unchanged, and its hash stays the real §8.3 one (the constant-hash pair is
// reference-key-only).
BEZEL_TEST(sorted_dictionary_class_key_3488, int_key_still_sorted_and_dedups) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var sd := new SortedDictionary<int, int>(); "
        "  sd.Add(3, 10); sd.Add(1, 20); sd.Add(2, 30); "
        "  var sb := new StringBuilder(); "
        "  for var e in sd do sb.Append($\"{e.Key}:{e.Value},\"); end; "
        "  Console.WriteLine(sb.ToString()); "
        "  Console.WriteLine($\"tryAdd={sd.TryAdd(1, 99)} first={sd.FirstKey} last={sd.LastKey}\"); "
        "  return 0; ")),
        "1:20,2:30,3:10,\ntryAdd=False first=1 last=3\n");
}

// per stdlib §4.17 (v0.283), #3488: the `string` key keeps its DEDICATED
// bz_sort_kv_str / bz_dict_min_key_str path, which never consults the
// comparator this fix widened — the row exists to pin that the widening did not
// pull it onto the generic path.
BEZEL_TEST(sorted_dictionary_class_key_3488, string_key_still_sorted) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var sd := new SortedDictionary<string, int>(); "
        "  sd.Add(\"c\", 1); sd.Add(\"a\", 2); sd.Add(\"b\", 3); "
        "  var sb := new StringBuilder(); "
        "  for var e in sd do sb.Append($\"{e.Key},\"); end; "
        "  Console.WriteLine(sb.ToString()); "
        "  Console.WriteLine($\"first={sd.FirstKey} last={sd.LastKey}\"); "
        "  return 0; ")),
        "a,b,c,\nfirst=a last=c\n");
}

// per stdlib §4.17 (v0.283), #3488: a RECORD key and an ENUM key are the two
// non-scalar categories `genericBackingType` already admitted; neither is a
// reference type, so neither takes the new arm. `buildSortedKeyComparator`
// still declines a record (a pre-existing gap, out of scope here), so this row
// asserts identity and count only — never an order it does not promise.
BEZEL_TEST(sorted_dictionary_class_key_3488, record_and_enum_keys_unchanged) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var sr := new SortedDictionary<Rec, int>(); "
        "  sr.Add(new Rec(2), 1); sr.Add(new Rec(1), 2); "
        "  var se := new SortedDictionary<Col, int>(); "
        "  se.Add(Col.Green, 1); se.Add(Col.Red, 2); "
        "  Console.WriteLine($\"rec={sr.Count} dupe={sr.TryAdd(new Rec(2), 9)}\"); "
        "  Console.WriteLine($\"enum={se.Count} firstIsRed={se.FirstKey = Col.Red}\"); "
        "  return 0; ")),
        "rec=2 dupe=False\nenum=2 firstIsRed=True\n");
}

// per stdlib §4.4 (`Array<T>`'s `public method Sort();  // in place`) and §2.8
// (`IComparable<T>`) + spec §14.2, #3488: `Array<T>.Sort()` for a class element is
// the ORIGINAL home of the `bz$classcmp$<C>` thunk (#1195); this fix lifted that
// builder out so the sorted map and Sort share one. The row pins that the lift
// is behaviour-preserving — a silent regression here would be a Sort() that
// stopped calling `CompareTo` at all, which is exactly what #2768 found for the
// interface case.
BEZEL_TEST(sorted_dictionary_class_key_3488, array_class_element_sort_unchanged) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var s1 := new Ord(3); "
        "  using var s2 := new Ord(1); "
        "  using var s3 := new Ord(2); "
        "  var arr := new Array<Ord>(); "
        "  arr.Add(s1); arr.Add(s2); arr.Add(s3); "
        "  arr.Sort(); "
        "  Console.WriteLine($\"{arr[0].V}{arr[1].V}{arr[2].V}\"); "
        "  return 0; ")),
        "123\n");
}

// per stdlib §4.4 (`public method Sorted(): owned Array<T>;  // fresh sorted
// copy`) + spec
// §14.2, v0.283, narrow #3488: `Sorted()` carried a THIRD, verbatim inline copy
// of the class comparator builder — which #3488's own commit body denied existed
// when it called Sort()'s "the only such builder in the compiler". That copy is
// now deleted and the arm routed through `buildObjectCompareToThunk`. This row
// is the same-module no-collateral guard for that deletion: it passes on both
// trees, and would catch a routing that stopped calling `CompareTo` at all —
// exactly the silent no-op the deleted copy's own dead constant-0 fallback would
// have produced. The cross-module half (where the two spellings genuinely
// diverged) is `xmod_class_comparator_collision_3512`.
BEZEL_TEST(sorted_dictionary_class_key_3488, array_class_element_sorted_unchanged) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var s1 := new Ord(3); "
        "  using var s2 := new Ord(1); "
        "  using var s3 := new Ord(2); "
        "  var arr := new Array<Ord>(); "
        "  arr.Add(s1); arr.Add(s2); arr.Add(s3); "
        "  var srt := arr.Sorted(); "
        "  Console.WriteLine($\"{srt[0].V}{srt[1].V}{srt[2].V}"
        " src={arr[0].V}{arr[1].V}{arr[2].V}\"); "
        "  return 0; ")),
        "123 src=312\n");
}

// per stdlib §4.4 + spec §12, v0.283, narrow #3488: the deleted `Sorted()` copy
// also carried its own vtable-slot dispatch, so the routing must keep dispatching
// a VIRTUAL `CompareTo` to the override rather than to the base declaration.
// `Desc` overrides `Virt`'s ascending comparison with a descending one; a
// non-virtual bind would print the ascending answer. Passes on both trees.
BEZEL_TEST(sorted_dictionary_class_key_3488,
           array_sorted_dispatches_the_virtual_override) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var d1 := new Desc(3); "
        "  using var d2 := new Desc(1); "
        "  using var d3 := new Desc(2); "
        "  var arr := new Array<Virt>(); "
        "  arr.Add(d1); arr.Add(d2); arr.Add(d3); "
        "  var srt := arr.Sorted(); "
        "  Console.WriteLine($\"{srt[0].V}{srt[1].V}{srt[2].V}\"); "
        "  return 0; ")),
        "321\n");
}

// per stdlib §4.18 (v0.283), #3488 / #3428: `SortedSet<T>` is a DIFFERENT arm
// and is deliberately untouched. Its class-element construction stays rejected
// by #3428 (its fresh-store dormancy row depends on that), and its `int`
// element must still sort — the comparator widening reaches
// `buildSortedKeyComparator`, which `SortedSet` shares.
BEZEL_TEST(sorted_dictionary_class_key_3488, sorted_set_arm_untouched) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(3); s.Add(1); s.Add(3); s.Add(2); "
        "  var sb := new StringBuilder(); "
        "  for var v in s do sb.Append($\"{v},\"); end; "
        "  Console.WriteLine(sb.ToString()); "
        "  Console.WriteLine($\"count={s.Count} first={s.First} last={s.Last}\"); "
        "  return 0; ")),
        "1,2,3,\ncount=3 first=1 last=3\n");
}
