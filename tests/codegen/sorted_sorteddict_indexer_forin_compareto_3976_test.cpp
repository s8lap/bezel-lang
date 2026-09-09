// Spec-derived regression tests for fix #3976: the `SortedSet<T>`/
// `SortedDictionary<K, V>` indexer and `for`-in iteration still silently
// admitted an eligible-but-not-orderable element/key (auto-hashable record
// or class/interface, no dispatchable `CompareTo`) AFTER fix #3947 closed
// the identical gap for `.Add`/`.Contains`/`.TryAdd`/etc.
//
// AUTHORITY. stdlib §4.18 (v0.295): "A comparison-ordered set: membership by
// `CompareTo`-equality, iteration in ascending order" — the SOLE constraint
// is `T: IComparable<T>`. stdlib §4.17 states the identical shape for
// `SortedDictionary<K, V>`'s key (`K: IComparable<K>`). `genericBackingType`
// (LlvmBackend_Calls3.cpp) has two branches that admit an element/key
// REGARDLESS of ordering — the auto-hashable RECORD branch (`recordTypeOfRef`,
// §9.2) and the class/interface branch (`hasObjectHeaderEquality`) — because
// they exist to serve `Set<T>`/`Dictionary<K,V>`, which need only hashing.
// `SortedSet`/`SortedDictionary` merely inherit them.
//
// Fix #3947 closed this for the CallExpr member-dispatch surface
// (`.Add`/`.Contains`/etc). It is a SEPARATE C++ translation unit's worth of
// call sites — `LlvmBackend_Assign.cpp`'s indexer (`sd[k]` / `sd[k] := v`)
// and `LlvmBackend_Stmts.cpp`'s `for`-in (both the `SortedSet<T>` element
// loop, `lowerDictBackedSetFor`, and the `SortedDictionary<K,V>` entry loop,
// `lowerDictEntryFor`) — that also consult `genericBackingType` and were left
// silently admitting the identical shape via structural (§9.1/§9.2 or §8.3)
// identity, or (for `for`-in) falling back to an unsorted/dead-but-compiling
// snapshot.
//
// THE DEFECT (measured on 6b0b167, post-#3987, verified on THIS tree before
// writing this file's fix, per the lane brief's own reproduction
// requirement):
//
//     module M;
//     record Pt
//       public property X: int;
//       public constructor(x: int) X := x; end constructor;
//     end Pt;
//     class C
//       public static method Run(s: SortedSet<Pt>): int
//         for var e in s do end;   // compiled clean, NO diagnostic
//         return 0;
//       end Run;
//       public static method Main(): int return 0; end Main;
//     end C;
//
// THE FIX (this file). A new shared authority,
// `Emitter::reportSortedElementNeedsCompareTo` (LlvmBackend_Calls3.cpp),
// carries the EXACT `needsOrdering && !hasOrdering` predicate + diagnostic
// wording fix #3947 wrote out inline twice in
// `tryLowerCollectionInstanceCall`. The indexer get/set
// (`lowerCollectionIndexerGet`/`lowerCollectionIndexerSet`,
// LlvmBackend_Assign.cpp) and both `for`-in entry points
// (`lowerDictBackedSetFor`/`lowerDictEntryFor`, LlvmBackend_Stmts.cpp) now
// call this ONE authority instead of silently admitting the shape — per this
// project's own standing rule against duplicated classification predicates.
// `.Add`/`.Contains`'s own two inline copies (#3947) are UNCHANGED — out of
// this fix's scope.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string out(const std::string& src) { return runAotOut(src); }

bool rejectsWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// `Pt`: auto-hashable record (§9.2), NO `CompareTo` — the issue's own shape.
// `ClsNoCmp`: class, NO `CompareTo` — the reference-key/element analogue.
// `Cmp`: auto-hashable record WITH `CompareTo` — an accept-side control.
const char* PRELUDE =
    "module M; "
    "record Pt "
    "  public property X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "end Pt; "
    "class ClsNoCmp "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "end ClsNoCmp; "
    "record Cmp "
    "  public property X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "  public method CompareTo(other: Cmp): int return X - other.X; "
    "    end CompareTo; "
    "end Cmp; ";

std::string prog(const std::string& body) {
    return std::string(PRELUDE) +
           "class C public static method Main(): int " + body +
           " end Main; end C;";
}

// A borrowed-parameter program — the receiver is never locally constructed,
// so no earlier construction-time check can pre-empt this fix's own check.
// (Mirrors sortedset_sorteddict_contains_add_no_compareto_3947_test.cpp's
// own `borrowedParamProg`.)
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

const char* NEEDLE_SET =
    "requires the element type to implement `IComparable<T>`";
const char* NEEDLE_DICT =
    "requires the key type to implement `IComparable<K>`";

}  // namespace

// ── THE ISSUE'S OWN REPRO, VERBATIM IN SHAPE ────────────────────────────────

BEZEL_TEST(sorted_sorteddict_indexer_forin_compareto_3976,
           verbatim_issue_repro_forin_over_sortedset_now_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<Pt>", "for var e in s do end; "),
        NEEDLE_SET));
}

// ── SortedDictionary<K, V> indexer: record key, no CompareTo ──────────────

BEZEL_TEST(sorted_sorteddict_indexer_forin_compareto_3976,
           sorteddictionary_indexer_get_record_no_compareto_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("d: SortedDictionary<Pt, int>",
                          "var v := d[new Pt(1)]; "),
        NEEDLE_DICT));
}

BEZEL_TEST(sorted_sorteddict_indexer_forin_compareto_3976,
           sorteddictionary_indexer_set_record_no_compareto_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("d: SortedDictionary<Pt, int>",
                          "d[new Pt(1)] := 5; "),
        NEEDLE_DICT));
}

// ── SortedDictionary<K, V> indexer: class key, no CompareTo ───────────────
// (Mirrors the exact shape sortedset_sorteddict_contains_add_no_compareto_
// 3947_test.cpp's own residual_indexer_now_rejected_by_fix_3976 row uses —
// re-asserted here as this fix's own primary coverage, invoked via a REAL,
// constructed receiver rather than a dormant borrowed parameter, since
// SortedDictionary<ClsNoCmp,int> CAN be constructed (no construction-time
// check for a class key, unlike SortedSet's element — narrow #3428).)

BEZEL_TEST(sorted_sorteddict_indexer_forin_compareto_3976,
           sorteddictionary_indexer_class_key_no_compareto_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "  var d := new SortedDictionary<ClsNoCmp, int>(); "
        "  var k := new ClsNoCmp(5); "
        "  d[k] := 1; "
        "  return d[k]; "),
        NEEDLE_DICT));
}

// ── for-in: SortedSet<T> element, class, no CompareTo ──────────────────────

BEZEL_TEST(sorted_sorteddict_indexer_forin_compareto_3976,
           sortedset_forin_class_element_no_compareto_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<ClsNoCmp>", "for var e in s do end; "),
        NEEDLE_SET));
}

// ── for-in: SortedDictionary<K, V> ENTRIES (`for var e in sd`) ────────────

BEZEL_TEST(sorted_sorteddict_indexer_forin_compareto_3976,
           sorteddictionary_entry_forin_record_key_no_compareto_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("d: SortedDictionary<Pt, int>",
                          "for var e in d do end; "),
        NEEDLE_DICT));
}

BEZEL_TEST(sorted_sorteddict_indexer_forin_compareto_3976,
           sorteddictionary_entry_forin_class_key_no_compareto_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("d: SortedDictionary<ClsNoCmp, int>",
                          "for var e in d do end; "),
        NEEDLE_DICT));
}

// ── Accept-side sweep (LANE_RULES §15): every legitimate shape must still ──
// ── compile AND behave correctly through the indexer AND for-in. ──────────

// int key/element — the pre-existing common case, untouched.
BEZEL_TEST(sorted_sorteddict_indexer_forin_compareto_3976,
           accept_int_indexer_and_forin_unaffected) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var sd := new SortedDictionary<int, string>(); "
        "  sd.Add(1, \"a\"); sd.Add(2, \"b\"); "
        "  sd[1] := \"aa\"; "
        "  Console.WriteLine(sd[1]); "
        "  for var e in sd do Console.WriteLine(e.Key); end; "
        "  var s := new SortedSet<int>(); "
        "  s.Add(3); s.Add(1); s.Add(2); "
        "  for var x in s do Console.WriteLine(x); end; "
        "  return 0; ")),
        std::string("aa\n1\n2\n1\n2\n3"));
}

// string key/element — its own dedicated fast path, untouched (fix #2553's
// SortedSet-string comparator; the dict string-key indexer path).
BEZEL_TEST(sorted_sorteddict_indexer_forin_compareto_3976,
           accept_string_indexer_and_forin_unaffected) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var sd := new SortedDictionary<string, int>(); "
        "  sd.Add(\"b\", 2); sd.Add(\"a\", 1); "
        "  sd[\"a\"] := 11; "
        "  Console.WriteLine(sd[\"a\"]); "
        "  for var e in sd do Console.WriteLine(e.Key); end; "
        "  var s := new SortedSet<string>(); "
        "  s.Add(\"z\"); s.Add(\"a\"); "
        "  for var x in s do Console.WriteLine(x); end; "
        "  return 0; ")),
        std::string("11\na\nb\na\nz"));
}

// record key/element WITH a dispatchable CompareTo (`Cmp` is auto-hashable,
// §9.2 — the fix #3987 shape, not #3740/#3948's non-auto-hashable one) —
// must keep working through the indexer and for-in, unaffected by THIS fix.
BEZEL_TEST(sorted_sorteddict_indexer_forin_compareto_3976,
           accept_record_with_compareto_indexer_and_forin_unaffected) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var sd := new SortedDictionary<Cmp, int>(); "
        "  sd.Add(new Cmp(2), 20); sd.Add(new Cmp(1), 10); "
        "  sd[new Cmp(1)] := 100; "
        "  Console.WriteLine(sd[new Cmp(1)]); "
        "  for var e in sd do Console.WriteLine(e.Key.X); end; "
        "  return 0; ")),
        std::string("100\n1\n2"));
}

// class key/element WITH a dispatchable CompareTo (fix #3488/#2773's own
// shape) — must keep working through the indexer and for-in.
BEZEL_TEST(sorted_sorteddict_indexer_forin_compareto_3976,
           accept_class_with_compareto_indexer_and_forin_unaffected) {
    BEZEL_EXPECT_EQ(out(std::string(
        "module M; "
        "class Ord: IComparable<Ord> "
        "  public V: int := 0; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public method CompareTo(other: Ord): int return V - other.V; "
        "    end CompareTo; "
        "end Ord; "
        "class C public static method Main(): int "
        "  using var a := new Ord(1); "
        "  using var b := new Ord(2); "
        "  var sd := new SortedDictionary<Ord, int>(); "
        "  sd.Add(a, 10); sd.Add(b, 20); "
        "  Console.WriteLine(sd[a]); "
        "  for var e in sd do Console.WriteLine(e.Key.V); end; "
        "  return 0; "
        "end Main; end C;")),
        std::string("10\n1\n2"));
}

// enum key — a scalar shape (i32-lowered), never eligible for this fix's
// `needsOrdering` test (neither recordTypeOfRef nor hasObjectHeaderEquality);
// untouched through the indexer and for-in.
BEZEL_TEST(sorted_sorteddict_indexer_forin_compareto_3976,
           accept_enum_key_indexer_and_forin_unaffected) {
    BEZEL_EXPECT_EQ(out(std::string(
        "module M; enum Col Red, Green, Blue; end Col; "
        "class C public static method Main(): int "
        "  var sd := new SortedDictionary<Col, int>(); "
        "  sd.Add(Col.Green, 1); sd.Add(Col.Red, 2); "
        "  sd[Col.Red] := 22; "
        "  Console.WriteLine(sd[Col.Red]); "
        "  Console.WriteLine(sd.Count); "
        "  for var e in sd do Console.WriteLine(e.Value); end; "
        "  return 0; "
        "end Main; end C;")),
        std::string("22\n2\n22\n1"));
}

// auto-hashable record WITH CompareTo (fix #3987's own shape) — the indexer
// and for-in must keep using CompareTo-based identity/ordering, unaffected
// by THIS fix (already exercised end-to-end by
// sortedset_sorteddict_compareto_identity_3987_test.cpp; re-asserted here as
// the accept-side boundary immediately adjacent to what this fix rejects —
// same auto-hashable-record shape as `Pt`, but WITH CompareTo).
BEZEL_TEST(sorted_sorteddict_indexer_forin_compareto_3976,
           accept_autohashable_record_with_compareto_indexer_and_forin) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var sd := new SortedDictionary<Cmp, int>(); "
        "  sd.Add(new Cmp(3), 30); sd.Add(new Cmp(1), 10); "
        "  Console.WriteLine(sd.Count); "
        "  var s := new SortedSet<Cmp>(); "
        "  s.Add(new Cmp(3)); s.Add(new Cmp(1)); "
        "  for var e in s do Console.WriteLine(e.X); end; "
        "  return 0; ")),
        std::string("2\n1\n3"));
}
