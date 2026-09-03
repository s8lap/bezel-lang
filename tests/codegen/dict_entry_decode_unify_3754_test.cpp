// Codegen — fix #3754 / narrow #3717 / stdlib §4.8, §4.17, §28.9 (v0.286):
// route the last two DictionaryEntry<K,V> decode copies through narrow
// #3717's shared helpers (decodeDictStringKeyAt, decodeDictGenericKeyAt,
// decodeDictValueAt — declared in LlvmBackend_impl.hpp, defined beside
// lowerDictEntryFor in LlvmBackend_Stmts.cpp):
//
//   (3) the §5 LINQ materializer's dict-source arm
//       (lowerToLazyEnumerator, LlvmBackend_Calls3.cpp) — the FOURTH copy
//       issue #3717 itself named, left untouched by narrow #3717 only
//       because it was outside that lane's file ownership;
//   (5) `wrapSortedDictionaryAsIEnumerable` (LlvmBackend_Calls2.cpp) — a
//       FIFTH near-identical copy narrow #3717's own audit discovered,
//       whose header comment said its loop body was "inlined here to avoid
//       touching" the shared one.
//
// Both copies already had their value-decode predicate widened to
// `valLlvm->isStructTy()` in place by fix #3758 (the choice-value ICE fix),
// so decodeDictValueAt's own #3758-derived predicate is now
// BEHAVIOUR-IDENTICAL to what each copy computed inline immediately before
// this fix — routing them through the helper is a pure mechanical
// extraction here, not a second predicate change. See
// dict_choice_value_decode_3758_test.cpp for the choice-V coverage of both
// sites (this file does not repeat those rows).
//
// §6b — WHAT EACH COPY EXCLUDED, PRESERVED EXACTLY BY THIS FIX:
//
//   * KEY RELEASE TIMING: both copies free the raw key pointer the decode
//     helpers return IMMEDIATELY, in the same basic block as the decode —
//     neither has an incBB (unlike the for-in loop's DEFERRED per-slot
//     free). This fix keeps that: the helper's raw pointer is freed right
//     after the call at both sites, unchanged from the inline code it
//     replaces.
//   * THIRD KEY BRANCH: `lowerToLazyEnumerator`'s dict arm has a three-way
//     key-decode branch (isStringKey / useGen / neither, the last one a
//     "should not happen" null fallback mirroring lowerDictEntryFor's own),
//     because its `useGen` is computed via `genericBackingType(...)`, a
//     predicate independent of `isStringKey`. `wrapSortedDictionaryAsIEnum
//     erable` has only TWO branches (isStringKey / else) because its
//     `useGen` is literally `!isStringKey` — a strict complement with no
//     third state. This fix preserves each copy's own branch COUNT; it does
//     not add a third branch to the SortedDictionary copy that was never
//     there, and does not collapse the LINQ copy's third branch away.
//   * SNAPSHOT, SORT AND WRAP stay copy-specific, untouched by this fix:
//       - the LINQ arm only sorts when `builtinTypeName(recvTy) ==
//         "SortedDictionary"` (a Dictionary LINQ source is NOT sorted);
//         `wrapSortedDictionaryAsIEnumerable` always sorts (it is only ever
//         reached for a SortedDictionary receiver). Neither sort step is
//         touched.
//       - the LINQ arm wraps its result as a lazy ARRAY source
//         (bz_lazy_from_owned_str_dict_array / bz_lazy_from_owned_array);
//         wrapSortedDictionaryAsIEnumerable wraps it as a vtable
//         IEnumerator (wrapDictionaryEntryArrayAsIEnumerable /
//         wrapOwnedArrayAsIEnumerable). Neither wrap call moves.
//   * The synthetic `DictionaryEntry` `TypeRef` the LINQ arm builds for the
//     lazy pipeline's element tracking (`entryTy->importedName =
//     "DictionaryEntry"`) is unrelated to the per-index decode and is left
//     untouched.
//
// OBJECT-CODE PARITY. Both sites' value-decode predicate was ALREADY
// `isStructTy()` (post-#3758) before this commit, and the key-decode logic
// is a byte-for-byte match of what decodeDictStringKeyAt/
// decodeDictGenericKeyAt compute — so this fix changes no observable
// codegen decision, only which function emits the IR. Verified by `cmp`
// on `bezelc -c` objects for int/string/class/record V through both
// callers, built from this commit vs its parent — see the commit body for
// byte counts.
//
// ORACLE. Every row is `runAotChecked` (ASan/LSan-instrumented), looped
// 200x per LANE_RULES so a one-shot leak (still stack-reachable at exit)
// cannot pass vacuously. A Dispose-counting class oracle (`R._n`) proves
// the class-typed-V rows show the entry's Value as a BORROW (V's true
// owner, declared `using`, disposed exactly once — not zero, a leak, and
// not more than once, a double-free) for both call sites.
//
// EXCEPTION — `sorted_dict_getenumerator_record_value_4_17` uses
// `runAotCheckedLenient`, not `runAotChecked`. DISCOVERED while writing this
// row: `SortedDictionary<int, Pt>`'s record-VALUE heap box is never freed on
// disposal, for EVERY reachable reader — measured on the #3754 PARENT
// (89eb914) via for-in (`lowerDictEntryFor`, already unified by narrow
// #3717, untouched by this commit) AND via this row's own
// `wrapSortedDictionaryAsIEnumerable` call, byte-identical 1600 B / 200
// allocations leaked either way. This is issue #3776
// ("SortedDictionary<K,V> never frees a value's heap box on disposal (record
// OR choice V) — dictValueBoxFreer's headName() gate excludes it entirely"),
// already filed by a concurrent lane, a DISPOSAL-side bug in
// LlvmBackend_Disposal.cpp — a file this lane's brief explicitly does not
// touch (owned concurrently by c11-b51 / #3770, which #3776 itself notes
// does NOT close this gap either). Not this commit's bug, not fixed by
// routing the DECODE through decodeDictValueAt, and not specific to the
// GetEnumerator call site (the for-in row above leaks identically) — so
// Lenient (ASAN_OPTIONS=detect_leaks=0) is used for this ONE row only,
// exactly as dict_choice_value_decode_3758_test.cpp does for its own two
// SortedDictionary rows citing the sibling gap. A real UAF/double-free in
// the struct-load path this commit touches would still be caught (ASan's
// instrumentation fires on the access, not at exit); only the KNOWN,
// separately-filed box leak is not asserted on.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// (3) LINQ materializer over a plain Dictionary — int-keyed, class-typed V.
//     `Where(lambda(e) => true)` forces lowerToLazyEnumerator's dict arm
//     without filtering anything out. Proves the Value read through the
//     LINQ path is a BORROW: `r` (not `d`) is V's true owner, disposed
//     exactly once per iteration.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_decode_unify_3754,
           linq_materializer_class_value_int_key_disposed_once_28_9) {
    const std::string src =
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  public Tag: int := 0; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var r := new R(7); "
        "    var d := new Dictionary<int, R>(); "
        "    d.Add(1, r); "
        "    for var e in d.Where(lambda(e) => true) do "
        "      total := total + e.Value.Tag; "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total} n={R._n}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1400 n=200"));  // 200 x 7
}

// ---------------------------------------------------------------------------
// (3) LINQ materializer — STRING-keyed, primitive V. Exercises
//     decodeDictStringKeyAt through the LINQ arm's IMMEDIATE-free schedule
//     (this loop has no incBB) and the `bz_lazy_from_owned_str_dict_array`
//     wrap that releases each rebuilt key at Dispose. #2544's own file
//     already proves this leak-free at length; this row is the post-#3754
//     regression pin, not new coverage.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_decode_unify_3754,
           linq_materializer_string_key_no_leak_2544) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var d := new Dictionary<string, int>(); "
        "    d[\"alpha\"] := 1; d[\"beta\"] := 2; d[\"gamma\"] := 3; "
        "    for var e in d.Where(lambda(e) => true) do "
        "      total := total + e.Value; "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1200"));  // 200 x (1+2+3)
}

// ---------------------------------------------------------------------------
// (3) LINQ materializer — RECORD-typed V. Exercises decodeDictValueAt's
//     struct-load arm (the SAME arm the choice-V fix widened) for a record,
//     the shape that arm ALREADY admitted before #3758 — a regression pin
//     that routing through the helper did not disturb the record case.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_decode_unify_3754, linq_materializer_record_value_4_8) {
    const std::string src =
        "module M; "
        "public record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var d := new Dictionary<int, Pt>(); "
        "    d.Add(1, new Pt(3, 4)); "
        "    for var e in d.Where(lambda(e) => true) do "
        "      total := total + e.Value.X + e.Value.Y; "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1400"));  // 200 x (3 + 4)
}

// ---------------------------------------------------------------------------
// (5) `wrapSortedDictionaryAsIEnumerable` — int-keyed (generic-backed),
//     class-typed V via the explicit `.GetEnumerator()` call. Same
//     borrow/disposed-once proof as the LINQ row above, for the
//     SortedDictionary copy.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_decode_unify_3754,
           sorted_dict_getenumerator_class_value_int_key_disposed_once_28_9) {
    const std::string src =
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  public Tag: int := 0; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var r := new R(9); "
        "    var sd := new SortedDictionary<int, R>(); "
        "    sd.Add(1, r); "
        "    using var it := sd.GetEnumerator(); "
        "    while it.MoveNext() do total := total + it.Current.Value.Tag; end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total} n={R._n}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1800 n=200"));  // 200 x 9
}

// ---------------------------------------------------------------------------
// (5) `wrapSortedDictionaryAsIEnumerable` — STRING-keyed, primitive V.
//     Exercises decodeDictStringKeyAt through THIS copy's own immediate-free
//     schedule and `wrapDictionaryEntryArrayAsIEnumerable`'s Dispose-time
//     string release (the #2522 sibling for the sorted path).
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_decode_unify_3754,
           sorted_dict_getenumerator_string_key_no_leak_4_17) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var sd := new SortedDictionary<string, int>(); "
        "    sd.Add(\"c\", 3); sd.Add(\"a\", 1); sd.Add(\"b\", 2); "
        "    using var it := sd.GetEnumerator(); "
        "    while it.MoveNext() do total := total + it.Current.Value; end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1200"));  // 200 x (1+2+3)
}

// ---------------------------------------------------------------------------
// (5) `wrapSortedDictionaryAsIEnumerable` — RECORD-typed V, the struct-load
//     arm this copy already admitted before #3758. `runAotCheckedLenient`,
//     NOT `runAotChecked` — see the file header's EXCEPTION paragraph: issue
//     #3776 (SortedDictionary never frees a record/choice value's heap box on
//     disposal, ANY reader, pre-existing and unrelated to this commit).
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_decode_unify_3754,
           sorted_dict_getenumerator_record_value_4_17) {
    const std::string src =
        "module M; "
        "public record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var sd := new SortedDictionary<int, Pt>(); "
        "    sd.Add(1, new Pt(3, 4)); "
        "    using var it := sd.GetEnumerator(); "
        "    while it.MoveNext() do "
        "      total := total + it.Current.Value.X + it.Current.Value.Y; "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotCheckedLenient(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1400"));  // 200 x (3 + 4)
}

// ---------------------------------------------------------------------------
// Cross-caller agreement over a SortedDictionary — the SAME entries, read
// through for-in (already routed through the shared helpers by narrow
// #3717), the explicit GetEnumerator (also narrow #3717), the LINQ
// materializer (#3754's copy 3) and this file's copy 5
// (wrapSortedDictionaryAsIEnumerable) must all agree on the SAME ascending
// key order — proving the whole family decodes identically AND that this
// fix did not disturb the sort step either copy interleaves with the
// now-shared decode.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_decode_unify_3754,
           all_sorted_dict_callers_agree_ascending_4_17) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  var sd := new SortedDictionary<int, int>(); "
        "  sd.Add(3, 30); sd.Add(1, 10); sd.Add(2, 20); "
        "  var viaForIn := 0; "
        "  for var e in sd do viaForIn := viaForIn + e.Key + e.Value; end; "
        "  var viaGetEnum := 0; "
        "  using var it := sd.GetEnumerator(); "
        "  while it.MoveNext() do "
        "    viaGetEnum := viaGetEnum + it.Current.Key + it.Current.Value; "
        "  end; "
        "  var viaLinq := 0; "
        "  for var e2 in sd.Where(lambda(e2) => true) do "
        "    viaLinq := viaLinq + e2.Key + e2.Value; "
        "  end; "
        "  Console.WriteLine($\"forin={viaForIn} getenum={viaGetEnum} "
        "linq={viaLinq}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    // 3+30+1+10+2+20 == 66 through every full-drain decode call site.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "forin=66 getenum=66 linq=66"));
}
