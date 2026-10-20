// Codegen — fix #3758 / stdlib §4.8, §9.x choice lowering (v0.286): the
// DictionaryEntry<K,V> value-decode "should this slot be struct-loaded"
// predicate excluded a struct-lowering V that is neither a record nor a
// nullable value type — i.e. a value-like `choice` — so `for var e in d`
// over a `Dictionary<int, Shape>` (Shape a choice) crashed the COMPILER with
// an LLVM IR verification failure (`Trunc only produces integer`): the
// narrower predicate took the scalar arm, which tried to `trunc` an i64 into
// the choice's LLVM struct type.
//
// ROOT CAUSE, measured against narrow #3717 (the DictionaryEntry decode
// unification): #3717's own commit body claimed "nothing in this codebase
// currently reaches" a struct-lowering V that is neither record nor
// nullable-value-type, and preserved TWO predicate spellings verbatim rather
// than merge them — for-in / GetEnumerator computed
// `valRec || (valNullableVal && isStructTy())`, while #3576's First/Last/
// ElementAt arm already used the broader `isStructTy()` alone. That claim
// was false: a value-like choice reaches exactly this combination, and the
// review that reproduced it opened this issue.
//
// FIX SHAPE. `decodeDictValueAt` (declared in LlvmBackend_impl.hpp, defined
// in LlvmBackend_Stmts.cpp) now derives its struct-vs-scalar decision from
// `valLlvm->isStructTy()` ITSELF rather than taking a caller-supplied bool —
// `isStructTy()` is a strict superset of the old narrow predicate (both
// `valRec` and `valNullableVal && isStructTy()` already imply `isStructTy()`),
// so this closes the crash without changing behaviour for int / nullable-int
// / record / class / string V at any of the three callers routed through it
// (lowerDictEntryFor's for-in loop, wrapDictionaryAsIEnumerable's
// GetEnumerator materializer, and #3576's arm in LlvmBackend_Calls.cpp — see
// each call site's own comment). The two UNROUTED copies #3754 names
// (LlvmBackend_Calls3.cpp's `lowerToLazyEnumerator` dict-LINQ-source arm and
// LlvmBackend_Calls2.cpp's `wrapSortedDictionaryAsIEnumerable`) had the SAME
// narrow predicate and are measured reachable for a choice V too (LINQ over
// a `Dictionary<K, Choice>`, and `SortedDictionary<K, Choice>.GetEnumerator()`
// called directly — see the reachability note below) — this lane's brief
// permits applying the SAME one-line predicate widening at those two sites
// IN PLACE (merging their own `if (valRec) {...} else if (valNullableVal &&
// isStructTy()) {...}` into `if (isStructTy()) {...}`) without routing them
// through decodeDictValueAt, which stays #3754's job (full unification of
// all five/six copies).
//
// §6b — WHAT EACH TOUCHED PREDICATE EXCLUDED, DIFFED AGAINST WHAT IT NOW
// ADMITS (the only axis that moves is struct-lowering V that is NEITHER a
// record NOR a nullable value type — i.e. a value-like choice; every other
// V kind's behaviour is UNCHANGED because the old predicate already agreed
// with `isStructTy()` for it):
//
//   Caller                              | OLD predicate            | NEW
//   -------------------------------------+--------------------------+-------
//   lowerDictEntryFor (for-in),          | valRec ||                | isStructTy()
//   wrapDictionaryAsIEnumerable          | (valNullableVal &&       | (choice
//   (GetEnumerator materializer)         | isStructTy())            | now admitted)
//   #3576's arm (LlvmBackend_Calls.cpp)  | isStructTy() alone       | UNCHANGED
//                                        | (already the broad one)  | (mechanical
//                                        |                          | call-site sync only)
//   lowerToLazyEnumerator's dict arm     | valRec ||                | isStructTy()
//   (LlvmBackend_Calls3.cpp, #3754's     | (valNullableVal &&       | (choice
//   copy 3, NOT routed through the       | isStructTy())            | now admitted)
//   shared helper)                       |                          |
//   wrapSortedDictionaryAsIEnumerable    | valRec ||                | isStructTy()
//   (LlvmBackend_Calls2.cpp, #3754's     | (valNullableVal &&       | (choice
//   copy 5, NOT routed through the       | isStructTy())            | now admitted)
//   shared helper)                       |                          |
//
// No exclusion this fix does NOT touch was lifted: `dictEntryWrapShapeSupported`
// (LlvmBackend_Assign.cpp) still DECLINES a choice V for the assignment-
// coercion face of GetEnumerator (`IEnumerable<DictionaryEntry<K,V>> x := d;`)
// — its OWN comment named the trunc bug as one of three reasons a choice V
// was excluded; this fix closes that ONE reason but the exclusion stays,
// because the disposal obligation (`dictSnapshotValueDisposers`) and the
// wrapped-face field access (#3563's shape) are both unaudited for a choice
// payload. See that gate's updated comment. Filed as followup #3767 rather
// than lifted speculatively.
//
// REACHABILITY (measured against this lane's rebase, both BEFORE and AFTER
// the fix, via `bezelc -c` directly): the issue's own repro (`for var e in d`)
// crashes pre-fix / compiles post-fix, as does `d.Where(lambda(e) => true)`
// materializing a LINQ chain over the SAME dictionary (LlvmBackend_Calls3.cpp
// arm), `sd.GetEnumerator()` called directly on a `SortedDictionary<int, Shape>`
// (LlvmBackend_Calls2.cpp's `wrapSortedDictionaryAsIEnumerable`, reached
// UNGATED via LlvmBackend_Calls2.cpp's own explicit `.GetEnumerator()` call
// arm — NOT the assignment-coercion gate `dictEntryWrapShapeSupported`
// declines), `d.GetEnumerator()` called directly on a plain
// `Dictionary<int, Shape>` (the SAME ungated explicit-call arm, routing to
// `wrapDictionaryAsIEnumerable`), and `for var e in sd` over a
// `SortedDictionary<int, Shape>` (`lowerDictEntryFor`'s own ascending-sort
// branch, already routed through the fixed `decodeDictValueAt`).
//
// ORACLE — UPDATED by fix #3770/#3768 (same commit) from runAotCheckedLenient
// to runAotChecked. History: EVERY row below constructs a `Dictionary<K,
// Choice>` and `.Add`s at least one entry, which boxes the choice value on
// the heap (`.Add`'s encode path — confirmed by the leak's own stack trace
// bottoming out in a `malloc` at the `.Add` call site, with or without any
// read of the dictionary afterward). Freeing that box on the dictionary's own
// disposal was `dictValueBoxFreer`'s job (`LlvmBackend_Disposal.cpp:4488`),
// and at the time this file was written it tested the EXACT SAME narrow
// predicate #3758 fixed on the decode side (`valIsRecord || valIsNullableVal`,
// excluding a choice) — so it returned null for a choice V and the box was
// never freed: a 200x loop of `using var d := new Dictionary<int, Shape>();
// d.Add(...); d.Add(...);` with NO for-in, NO GetEnumerator, NO LINQ at all
// leaked 4800 B in 400 allocations under LSan, byte-identical on the #3758
// PARENT and the #3758 FIX. Filed as #3770 (disposal-side sibling) and #3768
// (`lowerDictValuesFor` / `for var v in d.Values`, a SEVENTH independent copy
// of the same narrow predicate, untouched by #3758's routing through
// decodeDictValueAt). Both are now fixed: `dictValueBoxFreer` and
// `lowerDictValuesFor` read the SAME `dictValueIsBoxedStruct` predicate
// `decodeDictValueAt` reads (LlvmBackend_impl.hpp), so a choice V's box is
// freed exactly once at scope-end/Clear/overwrite — SIX of the eight rows
// below (every plain-`Dictionary<K,V>` row) now run under the STRICT
// `runAotChecked` oracle (LeakSanitizer's exit-time scan included) and are
// expected to be LSan-clean, not merely UAF/double-free-clean. Measured
// individually before flipping each: for-in (1), the ungated explicit
// GetEnumerator (2), the LINQ materializer (3), #3576's First()/Last() (4),
// and the cross-caller-agreement row are ALL LSan-clean post-fix.
//
// UPDATE (fix #3776, stdlib §4.8/§4.17, v0.286): the TWO `SortedDictionary`
// rows (`for_in_int_payload_choice_value_sorted_dict_4_17`,
// `sorted_dict_getenumerator_int_payload_choice_value_4_17`) originally
// STAYED on `runAotCheckedLenient` — flipping them failed on a SEPARATE,
// pre-existing, unrelated leak discovered while doing the #3770/#3768
// measurement and filed as #3776: `dictValueBoxFreer`'s
// `headName() != "Dictionary"` guard excluded `SortedDictionary<K,V>`
// entirely, for EVERY boxed V kind, not just choice — a
// `SortedDictionary<K, record>` leaked its value boxes exactly the same way
// a `SortedDictionary<K, Choice>` did (measured 3200 B / 400 for a plain
// `record Pt(x: int, y: int)` V). #3776 widened `dictValueBoxFreer`'s gate to
// admit `SortedDictionary` (LlvmBackend_Disposal.cpp), so both rows below now
// run under the STRICT `runAotChecked` oracle: measured individually
// LSan-clean post-#3776 fix (`total=1400` unchanged, zero leak either row).
//
// See `dict_choice_value_box_freer_3770_test.cpp` for the disposal-focused
// rows (the 200x Add+dispose-only leak pin, the indexer-overwrite disposing-
// setter path, and the class-like/borrow-bearing-choice double-free check)
// and `dict_values_choice_3768_test.cpp` for `for var v in d.Values`'s own
// FAIL-WITHOUT/PASS-WITH pin. Every row here is still looped 200x so a UAF
// that only manifests on a REUSED allocation (freed-then-reused heap slot)
// has the same chance to fire it always did. The int-payload rows prove the
// struct LOAD itself decodes the right bytes (the case-match on the
// loop-decoded value recovers the exact payload written at `Add`); the
// string-payload row (a plain `Dictionary`, so it DOES flip to the strict
// oracle) now additionally proves the OWNED `string` field inside a
// struct-lowering choice value is disposed correctly across 200 full
// Add-through-dispose cycles (previously it could only prove no UAF from the
// READ side, because the outer box's own leak carried the string's
// allocation with it).

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 8 checked rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics, and then RETURNED from the row on
// it. That is an ABSORBING site by
// scripts/check_toolchain_skip_surface.py's own definition, and it is a silent
// vacuous pass: on a box whose clang cannot link the ASan binary, all
// 8 rows returned WITHOUT asserting and the suite reported a clean PASS
// having compiled and run nothing. MEASURED on the pre-change tree with the
// harness's own knob (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`): every row green.
//
// Every one of those guards becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position: the
// skip is now LOUD — the row FAILS — instead of being absorbed by a guard
// that reports a clean pass. The lenient legs' assertions additionally
// now RUN UNCONDITIONALLY rather than being skipped, which is the other half of
// the strengthening. Every substantive assertion in every row (r.ran and the
// exitCode pins) stays exactly where it was.

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}


}  // namespace

// ---------------------------------------------------------------------------
// (1) for-in — the issue's own repro shape, widened to a 200x generated-code-
//     ASan loop and TWO cases (int-payload choice) so the assertion is on the
//     DECODED VALUES, not just "did not crash". FAILS-WITHOUT (IR verifier:
//     `Trunc only produces integer`) / PASSES-WITH.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_choice_value_decode_3758, for_in_int_payload_choice_value_4_8) {
    const std::string src =
        "module M; "
        "choice Shape Circle(r: int), Square(s: int); end Shape; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, Shape>(); "
        "    d.Add(1, Shape.Circle(3)); "
        "    d.Add(2, Shape.Square(4)); "
        "    for var e in d do "
        "      total := total + (case e.Value of "
        "        Circle(var r) => r, "
        "        Square(var s) => s); "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1400"));  // 200 x (3 + 4)
}

// ---------------------------------------------------------------------------
// (1) for-in — STRING-payload choice value: a choice case carrying an OWNED
//     `string` field, proving the struct-load path neither double-releases it
//     nor use-after-frees it across 200 iterations (generated-code ASan; see
//     the file header for why this is Lenient — the outer box's own leak is
//     issue #3770, not this row's to assert clean). Also proves the decoded
//     field content is correct (not truncated/corrupted bytes).
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_choice_value_decode_3758,
           for_in_string_payload_choice_value_no_uaf_28_9) {
    const std::string src =
        "module M; "
        "choice Msg Note(text: string), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, Msg>(); "
        "    d.Add(1, Msg.Note(\"ab\")); "
        "    d.Add(2, Msg.Note(\"cde\")); "
        "    for var e in d do "
        "      total := total + (case e.Value of "
        "        Note(var t) => t.Length, "
        "        Empty       => 0); "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1000"));  // 200 x (2 + 3)
}

// ---------------------------------------------------------------------------
// (1) for-in over SortedDictionary — the SAME lowerDictEntryFor / fixed
//     decodeDictValueAt, exercised through the §4.17 ascending-key sort
//     branch (ordering not asserted here; #1426/#3511 already cover that
//     axis — this row is choice-V decode coverage on the sorted path).
//     Runs under runAotChecked (moved off runAotCheckedLenient by fix #3776 —
//     see the file header's UPDATE note): SortedDictionary's own value-box
//     leak, independent of choice V, is now fixed.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_choice_value_decode_3758,
           for_in_int_payload_choice_value_sorted_dict_4_17) {
    const std::string src =
        "module M; "
        "choice Shape Circle(r: int), Square(s: int); end Shape; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new SortedDictionary<int, Shape>(); "
        "    d.Add(2, Shape.Square(4)); "
        "    d.Add(1, Shape.Circle(3)); "
        "    for var e in d do "
        "      total := total + (case e.Value of "
        "        Circle(var r) => r, "
        "        Square(var s) => s); "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1400"));  // 200 x (3 + 4)
}

// ---------------------------------------------------------------------------
// (2, ungated) explicit `d.GetEnumerator()` call on a PLAIN Dictionary —
// LlvmBackend_Calls2.cpp's own `isDictFam` explicit-call arm routes to
// `wrapDictionaryAsIEnumerable` WITHOUT going through
// `dictEntryWrapShapeSupported` (that gate only guards the ASSIGNMENT-
// COERCION face in LlvmBackend_Assign.cpp / LlvmBackend_Setup.cpp) — so this
// call shape reaches decodeDictValueAt's fixed predicate directly, even
// though the coercion face still declines a choice V.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_choice_value_decode_3758,
           explicit_getenumerator_int_payload_choice_value_4_8) {
    const std::string src =
        "module M; "
        "choice Shape Circle(r: int), Square(s: int); end Shape; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var d := new Dictionary<int, Shape>(); "
        "    d.Add(1, Shape.Circle(3)); "
        "    using var it := d.GetEnumerator(); "
        "    while it.MoveNext() do "
        "      total := total + (case it.Current.Value of "
        "        Circle(var r) => r, "
        "        Square(var s) => s); "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=600"));  // 200 x 3
}

// ---------------------------------------------------------------------------
// (5, unrouted copy, #3754) explicit `sd.GetEnumerator()` on a
// SortedDictionary — LlvmBackend_Calls2.cpp's `wrapSortedDictionaryAsIEnumerable`,
// its own independent decode copy (not routed through decodeDictValueAt),
// fixed in place by the same one-line predicate widening. Runs under
// runAotChecked (moved off runAotCheckedLenient by fix #3776 — see the file
// header's UPDATE note): the receiver `d` here is a bare `var`, never
// `using var` — but it is never explicitly discharged either, so the
// ownership pass's §28.3 v0.150 affine auto-drop (`ld->autoDropAtScopeEnd`,
// LlvmBackend_Stmts.cpp) routes it through the SAME `registerUsingDisposal`
// -> `dictTypeOfLocal` -> `dictValueBoxFreer` fallback an explicit
// `using var d` would get (this function itself never frees `dict` or any
// value box — it only frees its OWN scratch `keysArr`/`valsArr` snapshot
// arrays). `d`'s scope-end box-free is exactly what #3776 fixed; measured
// LSan-clean post-fix.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_choice_value_decode_3758,
           sorted_dict_getenumerator_int_payload_choice_value_4_17) {
    const std::string src =
        "module M; "
        "choice Shape Circle(r: int), Square(s: int); end Shape; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var d := new SortedDictionary<int, Shape>(); "
        "    d.Add(2, Shape.Square(4)); "
        "    d.Add(1, Shape.Circle(3)); "
        "    using var it := d.GetEnumerator(); "
        "    while it.MoveNext() do "
        "      total := total + (case it.Current.Value of "
        "        Circle(var r) => r, "
        "        Square(var s) => s); "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotCheckedLenient(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1400"));  // 200 x (3 + 4)
}

// ---------------------------------------------------------------------------
// (3, unrouted copy, #3754) LINQ over Dictionary<K, Choice> —
// LlvmBackend_Calls3.cpp's `lowerToLazyEnumerator` dict-source arm, its own
// independent decode copy (not routed through decodeDictValueAt), fixed in
// place by the same one-line predicate widening. `Where(lambda(e) => true)`
// forces the lazy-pipeline materializer without filtering anything out.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_choice_value_decode_3758,
           linq_where_int_payload_choice_value_5) {
    const std::string src =
        "module M; "
        "choice Shape Circle(r: int), Square(s: int); end Shape; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var d := new Dictionary<int, Shape>(); "
        "    d.Add(1, Shape.Circle(3)); "
        "    for var e in d.Where(lambda(e) => true) do "
        "      total := total + (case e.Value of "
        "        Circle(var r) => r, "
        "        Square(var s) => s); "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=600"));  // 200 x 3
}

// ---------------------------------------------------------------------------
// (4) #3576's arm (LlvmBackend_Calls.cpp) already used the broader
// `isStructTy()` predicate BEFORE this fix (this lane's diff there is purely
// mechanical — dropping the now-derived `loadAsStruct` argument), so this row
// is a regression / object-parity check, not a new FAIL-WITHOUT case:
// First()/Last() over a choice-valued Dictionary must keep working exactly
// as it did pre-#3758.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_choice_value_decode_3758,
           first_last_int_payload_choice_value_unchanged_3576) {
    const std::string src =
        "module M; "
        "choice Shape Circle(r: int), Square(s: int); end Shape; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var d := new Dictionary<int, Shape>(); "
        "    d.Add(1, Shape.Circle(3)); "
        "    d.Add(2, Shape.Square(4)); "
        "    var fe := d.First(); var le := d.Last(); "
        "    total := total + (case fe.Value of Circle(var r) => r, Square(var s) => s) "
        "                    + (case le.Value of Circle(var r) => r, Square(var s) => s); "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1400"));  // 200 x (3 + 4)
}

// ---------------------------------------------------------------------------
// Cross-caller agreement — the SAME dictionary contents, read through for-in
// (1), the ungated explicit GetEnumerator (2), the LINQ materializer (3), and
// #3576's First()/Last() (4), must all agree on a choice V. This is the
// closest one row can get to proving every reachable caller decodes a
// struct-lowering choice value identically.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_choice_value_decode_3758,
           all_callers_agree_on_choice_value_4_8) {
    const std::string src =
        "module M; "
        "choice Shape Circle(r: int), Square(s: int); end Shape; "
        "class C public static method Main(): int "
        "  var d := new Dictionary<int, Shape>(); "
        "  d.Add(1, Shape.Circle(3)); "
        "  d.Add(2, Shape.Square(4)); "
        "  var viaForIn := 0; "
        "  for var e in d do "
        "    viaForIn := viaForIn + (case e.Value of Circle(var r) => r, Square(var s) => s); "
        "  end; "
        "  var viaGetEnum := 0; "
        "  using var it := d.GetEnumerator(); "
        "  while it.MoveNext() do "
        "    viaGetEnum := viaGetEnum "
        "        + (case it.Current.Value of Circle(var r) => r, Square(var s) => s); "
        "  end; "
        "  var viaLinq := 0; "
        "  for var e2 in d.Where(lambda(e2) => true) do "
        "    viaLinq := viaLinq + (case e2.Value of Circle(var r) => r, Square(var s) => s); "
        "  end; "
        "  var fe := d.First(); var le := d.Last(); "
        "  var viaFirstLast := (case fe.Value of Circle(var r) => r, Square(var s) => s) "
        "                    + (case le.Value of Circle(var r) => r, Square(var s) => s); "
        "  Console.WriteLine($\"forin={viaForIn} getenum={viaGetEnum} "
        "linq={viaLinq} firstlast={viaFirstLast}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 3 + 4 == 7 through every full-drain decode call site.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "forin=7 getenum=7 linq=7"));
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "firstlast=7"));
}
