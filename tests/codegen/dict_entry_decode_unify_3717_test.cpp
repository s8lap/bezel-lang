// Codegen — narrow #3717 / stdlib §4.8, §28.9 (v0.286): route the per-index
// DictionaryEntry<K,V> decode shared by
//   (1) the for-in loop over a Dictionary/SortedDictionary
//       (lowerDictEntryFor, LlvmBackend_Stmts.cpp),
//   (2) the explicit `.GetEnumerator()` materializer
//       (wrapDictionaryAsIEnumerable, LlvmBackend_Assign.cpp), and
//   (4) #3576's First()/Last()/ElementAt(i) arm
//       (LlvmBackend_Calls.cpp)
// through THREE additive helpers (decodeDictStringKeyAt, decodeDictGenericKeyAt,
// decodeDictValueAt — declared in LlvmBackend_impl.hpp, defined beside
// lowerDictEntryFor in LlvmBackend_Stmts.cpp) rather than three independently
// drifting copies of the same malloc+memcpy string-key rebuild, the same
// generic-key raw-bytes load, and the same record/nullable-value/scalar/
// pointer value decode.
//
// (3) the §5 LINQ materializer (`lowerToLazyEnumerator`'s dict arm,
// LlvmBackend_Calls3.cpp) is the FOURTH copy the issue names. It is NOT
// touched by this lane: it lives outside this lane's file ownership (the
// brief's "Files you own" list is Calls.cpp / Assign.cpp / Stmts.cpp / tests
// only), so `narrow #3717` rather than `fix #3717` — three of the four
// copies are unified, the fourth is untouched and still its own, independent
// copy of the same logic. Its own extensive existing coverage
// (dict_linq_source_string_key_release_2544_test.cpp, sorted_dict_linq_
// compose_3511_test.cpp) re-run clean, unmodified by this commit, proving the
// untouched copy still agrees with the three now-unified ones (see the
// rows below, which pin the CROSS-CALLER agreement on the same dictionary
// contents).
//
// §6b — WHAT EACH COPY EXCLUDES, PRESERVED EXACTLY (not lifted, not merged):
//
//   * VALUE decode's "should this slot be struct-loaded" predicate differs
//     between the two now-shared call sites:
//       - for-in / GetEnumerator:  valRec || (valNullableVal && isStructTy())
//       - #3576's arm:             isStructTy() alone (broader)
//     These are NOT proven equivalent for a struct-lowering V that is
//     neither a record nor a nullable value type (e.g. a Choice) — nothing
//     in this codebase exercises that combination today, so this lane does
//     not resolve which is "more correct"; decodeDictValueAt takes
//     `loadAsStruct` as an explicit bool so each caller keeps computing its
//     OWN predicate, verbatim, unchanged by this refactor.
//   * `wrapDictionaryAsIEnumerable`'s caller-side gate (`dictEntryWrapShape
//     Supported`, unmodified by this fix) DECLINES a record/class/interface-
//     typed V outright (issue #3563: `x.Value.<member>` fails to lower on
//     the wrapped face) — a restriction this lane does NOT touch or lift.
//     The GetEnumerator rows below therefore use a primitive V (the
//     admitted shape); the for-in and #3576 rows use a class-typed V (a
//     shape only THOSE two callers admit) to prove the shared value-decode
//     helper's pointer-passthrough (borrow) arm still behaves identically
//     for both callers post-refactor.
//   * String-key admission differs three ways: for-in and GetEnumerator both
//     rebuild an owned string per entry (freed on different schedules — an
//     incBB deferral for for-in, an immediate free for GetEnumerator, BOTH
//     preserved verbatim: decodeDictStringKeyAt returns the raw pointer and
//     lets the caller free it whenever it always did); #3576's arm still
//     rejects a string key LOUDLY (narrow #3576 / #3720) — that reject is
//     UNTOUCHED by this commit (no string-key branch was added to #3576's
//     arm; see the existing dictionary_string_key_*_rejected_loudly_3720
//     rows in dict_first_last_elementat_3576_test.cpp, re-run unmodified
//     below via the `3576` filter).
//   * Free-loop TIMING is copy-specific by design (issue #3717's own
//     recommended shape: "freeing is copy-specific... should stay
//     separate") and is NOT touched here: for-in frees each raw key blob in
//     incBB (deferred one iteration) plus an end-of-loop cleanup pass for
//     `break`; GetEnumerator frees inline, in the same iteration; #3576's
//     arm frees every blob in ONE bulk pass after decoding the single
//     chosen index. None of these move.
//
// ORACLE. Every row is `runAotChecked` (ASan/LSan-instrumented), looped 200x
// per LANE_RULES so a one-shot leak (still stack-reachable at exit) cannot
// pass vacuously. A Dispose-counting class oracle (`R._n`) proves the
// class-typed-V rows dispose exactly once, not zero (leak) or more than once
// (double-free).

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// #3733 toolchain-SKIP burn-down. This file's 6 absorbing rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics and returned from the row on it —
// the definition scripts/check_toolchain_skip_surface.py uses for an
// ABSORBING site. It was also a silent vacuous pass: on a box whose clang
// cannot link the ASan binary, every one of those rows returned WITHOUT
// asserting, and the suite reported a clean PASS having compiled and run
// nothing.
//
// Each guard becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position:
// the skip is still recognised as an environment condition rather than a
// product failure, but it now MARKS THE ROW FAILED instead of hiding it.
// Every substantive assertion in every row — `r.ran` and the exitCode pins —
// stays exactly where it was, and the local predicate is deleted so no
// absorbing shape survives. The guard stays a SEPARATE statement from the
// predicate on purpose: inlining it into an `if` recreates the shape the
// predicate exists to end.

}  // namespace

// ---------------------------------------------------------------------------
// (1) for-in — int-keyed, class-typed V (decodeDictGenericKeyAt +
//     decodeDictValueAt's pointer-passthrough / borrow arm)
// ---------------------------------------------------------------------------

// Mirrors #3576's own class-value oracle (dictionary_class_value_read_while_
// alive_disposed_once_28_9) but for the for-in path this lane also touched:
// `r` (not `d`) is V's true owner (§28.11's borrowing-dictionary contract),
// so 200 iterations must show exactly 200 dispositions.
BEZEL_TEST(dict_entry_decode_unify_3717, for_in_class_value_int_key_disposed_once_28_9) {
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
        "    for var e in d do total := total + e.Value.Tag; end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total} n={R._n}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1400 n=200"));  // 200 x 7
}

// ---------------------------------------------------------------------------
// (1) for-in — STRING-keyed, class-typed V (decodeDictStringKeyAt, freed in
//     incBB per the ORIGINAL deferred schedule, plus the same value-decode
//     borrow arm as above)
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_decode_unify_3717, for_in_class_value_string_key_disposed_once_28_9) {
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
        "    var d := new Dictionary<string, R>(); "
        "    d.Add(\"k\", r); "
        "    for var e in d do total := total + e.Key.Length + e.Value.Tag; end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total} n={R._n}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 200 x (1 + 9) == 2000; disposed exactly once per iteration == 200.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=2000 n=200"));
}

// ---------------------------------------------------------------------------
// (2) explicit `.GetEnumerator()` — int-keyed, primitive V (the only V shape
//     `dictEntryWrapShapeSupported` admits; UNTOUCHED by this fix — see the
//     header §6b note). Exercises decodeDictGenericKeyAt + decodeDictValueAt's
//     scalar ptrtoint/trunc arm.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_decode_unify_3717, getenumerator_int_key_no_leak_4_8) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, int>(); "
        "    d.Add(3, 30); d.Add(1, 10); "
        "    using var e := d.GetEnumerator(); "
        "    while e.MoveNext() do total := total + e.Current.Value; end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=8000"));  // 200 x (30 + 10)
}

// ---------------------------------------------------------------------------
// (2) explicit `.GetEnumerator()` — STRING-keyed, primitive V. Exercises
//     decodeDictStringKeyAt with GetEnumerator's ORIGINAL immediate-free
//     schedule (freed in the SAME iteration it is decoded, unlike for-in's
//     incBB deferral) — the string-key leak this call site's own #2522 fix
//     originally closed, re-verified post-refactor.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_decode_unify_3717, getenumerator_string_key_no_leak_2522) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<string, int>(); "
        "    d[\"alpha\"] := 1; d[\"beta\"] := 2; d[\"gamma\"] := 3; "
        "    using var e := d.GetEnumerator(); "
        "    while e.MoveNext() do total := total + e.Current.Value; end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1200"));  // 200 x (1+2+3)
}

// ---------------------------------------------------------------------------
// (4) #3576's arm — Dictionary<int, int?>: a NULLABLE-value-type V, pinning
//     that decodeDictValueAt's `loadAsStruct = dtValLlvm->isStructTy()`
//     (this arm's OWN, broader predicate — see the header §6b note) still
//     struct-loads the tagged {i8,i32} box post-refactor exactly as the
//     inline pre-refactor code did (int? was never separately tested for
//     this arm before this lane).
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_decode_unify_3717, first_nullable_value_type_v_isstructty_4_8) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var d := new Dictionary<int, int?>(); "
        "    d.Add(1, 42); "
        "    var fe := d.First(); "
        "    total := total + (fe.Value ?? -1); "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=8400"));  // 200 x 42
}

// ---------------------------------------------------------------------------
// Cross-caller agreement — the SAME dictionary contents read through for-in
// (1), GetEnumerator (2), the untouched LINQ materializer (3, via a no-op
// Where(_ => true) LINQ chain forcing lowerToLazyEnumerator), and #3576's
// First()/Last() (4) must all agree. This is the closest this lane can get
// to a single row proving all four copies still decode identically without
// editing copy (3) itself.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_decode_unify_3717, all_four_callers_agree_on_same_dict_4_8) {
    // Every quantity is printed to stdout, not folded into the exit code
    // (LANE_RULES: AOT exit status truncates to 8 bits) and no comparison is
    // tautological: via3576 = fe.Key+fe.Value+le.Key+le.Value is compared
    // against the STDLIB-KNOWN expected constant (First=insertion-order
    // first entry, key=3 val=30; Last=insertion-order last entry, key=2
    // val=20 -- see dict_first_last_elementat_3576_test.cpp's identical
    // insertion-order comment), not against itself.
    const std::string src =
        "module M; class C public static method Main(): int "
        "  var d := new Dictionary<int, int>(); "
        "  d.Add(3, 30); d.Add(1, 10); d.Add(2, 20); "
        "  var viaForIn := 0; "
        "  for var e in d do viaForIn := viaForIn + e.Key + e.Value; end; "
        "  var viaGetEnum := 0; "
        "  using var it := d.GetEnumerator(); "
        "  while it.MoveNext() do "
        "    viaGetEnum := viaGetEnum + it.Current.Key + it.Current.Value; "
        "  end; "
        "  var viaLinq := 0; "
        "  for var e2 in d.Where(lambda(e2) => true) do "
        "    viaLinq := viaLinq + e2.Key + e2.Value; "
        "  end; "
        "  var fe := d.First(); var le := d.Last(); "
        "  var via3576 := fe.Key + fe.Value + le.Key + le.Value; "
        "  Console.WriteLine($\"forin={viaForIn} getenum={viaGetEnum} "
        "linq={viaLinq} t3576={via3576}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 3+30+1+10+2+20 == 66 through every full-drain decode call site.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "forin=66 getenum=66 linq=66"));
    // First=(key 3, val 30), Last=(key 2, val 20): 3+30+2+20 == 55.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "t3576=55"));
}
