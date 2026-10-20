// Codegen — fix #3776 / stdlib §4.8 / §4.17 / §28.9 (v0.286): `dictValueBoxFreer`
// (`LlvmBackend_Disposal.cpp:4488`) decides whether a dictionary's per-value
// heap box needs freeing at scope-end / Clear / indexer-overwrite. Before this
// fix it gated on `dictTy->headName() != "Dictionary"`, returning null for
// EVERY other head — including `SortedDictionary<K, V>`, for EVERY boxed V
// kind (record, nullable value type, choice), not just one. A `using var d :=
// new SortedDictionary<int, V>();` that `.Add()`s a boxed V and never reads it
// back still leaked one box per surviving entry, exactly like #3770's
// Dictionary-side bug (a DIFFERENT root cause: #3770 was the boxed-struct
// PREDICATE excluding a choice V; this is the HEAD-NAME GATE excluding an
// entire dictionary family regardless of V kind).
//
// stdlib §4.17: "Value semantics match Dictionary<K, V>: values are copied in
// and out for value types and string, borrowed for class types." and "There
// is no owning OwningSortedDictionary in this revision" — so SortedDictionary's
// value-boxing story is IDENTICAL to borrowing Dictionary<K, V>'s: the same
// broad `isStructTy()` encode-side test (LlvmBackend_{Calls,Assign}.cpp's
// `encodeValue`) boxes a record / nullable-value / choice V on `.Add`
// regardless of which of the two heads is used.
//
// FIX. `dictValueBoxFreer`'s head-name gate now reads `isBoxedValueDictHeadName`
// (a new, file-local, ONE-spelling predicate directly above it in
// LlvmBackend_Disposal.cpp) instead of the single-name `!= "Dictionary"`
// test — admitting `SortedDictionary` alongside `Dictionary`.
// `OwningDictionary` stays excluded on two INDEPENDENT axes: the name gate
// (only "Dictionary"/"SortedDictionary" match) and the `isStructTy()` boxed-
// struct predicate below it (`OwningDictionary<K, V>` requires `V: class`,
// stdlib §4.9, and a class value never lowers to a struct). The identical
// two-name test `collectionNodeFreeThunk` already spelled inline nearby (the
// dict-value string-retain-at-construction question) now reads the SAME
// named predicate instead of re-deriving a third copy — a mechanical,
// no-behavior-change de-duplication (that `head` can only ever be
// "Dictionary" or "SortedDictionary" at that call site, proven by the
// function's own preceding `common::isOwningPrefixedName` rejection).
//
// §6b — DOUBLE-FREE ANALYSIS. The danger after widening a "does this need
// freeing" gate is always a double free: does anything else already free a
// SortedDictionary value's box?
//   * The box itself: `dictValueBoxFreer` remains the ONLY caller of
//     `valueBoxFreeThunk()` for a borrowing dict-family value slot (grep
//     confirms one call site pre- and post-fix). The DECODE side
//     (`decodeDictValueAt`, and `wrapSortedDictionaryAsIEnumerable`'s own
//     independent decode copy, fixed by #3758) only LOADS the struct bytes
//     out of the box — never frees it — so decode and disposal never race
//     over the same free.
//   * Every call site that reaches `dictValueBoxFreer` derives its answer
//     from `dictTy`/`recvTy` — the SAME `sema::TypeRef` the runtime free call
//     targets — so widening the gate cannot make two DIFFERENT dictionaries
//     share one free.
//   * A choice V's payload analysis is unchanged from #3770 (see that fix's
//     own §6b note): an owning-classified choice cannot reach a live
//     `SortedDictionary<K, Choice>` at all (move-only, §15.3/§35.4 rule 1,
//     and `Add(key: K, value: V)` takes `value` by plain, non-owned
//     parameter), so the only class-typed payload ever stored is a BORROW —
//     freeing the box never touches it. `class_v_borrow_never_freed_by_map`
//     below measures this directly for SortedDictionary.
//
// SCOPE NOTE — `.Remove(key)` is a SEPARATE, PRE-EXISTING bug, NOT fixed
// here and NOT specific to SortedDictionary. `LlvmBackend_Calls.cpp`'s
// `.Remove(k)` arm (~line 7241) derives its value-disposer from
// `dictValClassDisposer` (OwningDictionary<K,class>) or the string-release
// thunk ONLY — it never calls `dictValueBoxFreer` as a fallback the way
// scope-end / Clear() / indexer-overwrite all do. Measured: a `Dictionary
// <int, Pt>` (Pt a 2-int record), 200x `.Add()` x2 then `.Remove()` both keys
// then `.Dispose()`, leaks 3200 B / 400 allocations on THIS fix (unchanged
// from parent — `LlvmBackend_Calls.cpp` is outside this lane's file
// ownership, and the leak is identical for `SortedDictionary`, so it is not
// this issue's "SortedDictionary never frees" framing). Filed as a new
// issue rather than fixed here.
//
// FAIL-WITHOUT / PASS-WITH (§R12-H): built a PARENT bezelc via `git archive`
// of this lane's pre-fix commit into a separate scratch tree. The issue's own
// two repros (a 200x loop of bare `.Add()` x2 + `using var` scope-end
// dispose, NO iteration): record V (`Pt(x: int, y: int)`) leaked
// `SUMMARY: AddressSanitizer: 3200 byte(s) leaked in 400 allocation(s)` on
// PARENT, LSan-clean on FIX; choice V (`Shape Circle(r)/Square(s)`) leaked
// `4800 byte(s) leaked in 400 allocation(s)` on PARENT (identical shape to
// #3770's own Dictionary repro), LSan-clean on FIX.

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

// #3733 toolchain-SKIP burn-down. This file's 8 absorbing rows used to
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
// the skip is now LOUD — the row FAILS — instead of being absorbed by a
// guard that reports a clean pass.
// Every substantive assertion in every row — `r.ran` and the exitCode pins —
// stays exactly where it was, and the local predicate is deleted so no
// absorbing shape survives. The guard stays a SEPARATE statement from the
// predicate on purpose: inlining it into an `if` recreates the shape the
// predicate exists to end.

}  // namespace

// ---------------------------------------------------------------------------
// The issue's own repro (record V): bare `.Add()` x2 + scope-end dispose, NO
// read of the dictionary at all. FAIL-WITHOUT: 3200 B / 400 allocations
// leaked (measured above, file header). PASS-WITH: LSan-clean.
// ---------------------------------------------------------------------------

BEZEL_TEST(sorted_dict_value_box_freer_3776,
           record_value_add_only_no_iteration_disposed_once_4_17) {
    const std::string src =
        "module M; "
        "record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    using var d := new SortedDictionary<int, Pt>(); "
        "    d.Add(2, new Pt(4, 5)); "
        "    d.Add(1, new Pt(3, 6)); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// The issue's own repro (choice V): same shape as #3770's own Dictionary
// repro, but over SortedDictionary. FAIL-WITHOUT: 4800 B / 400 allocations
// leaked. PASS-WITH: LSan-clean.
// ---------------------------------------------------------------------------

BEZEL_TEST(sorted_dict_value_box_freer_3776,
           choice_value_add_only_no_iteration_disposed_once_4_17) {
    const std::string src =
        "module M; "
        "choice Shape Circle(r: int), Square(s: int); end Shape; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    using var d := new SortedDictionary<int, Shape>(); "
        "    d.Add(2, Shape.Square(4)); "
        "    d.Add(1, Shape.Circle(3)); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// Clear() — stdlib §4.17's `Clear()` empties the table in place; the
// surviving values' boxes must be reclaimed exactly as Dictionary's Clear()
// already does (§4.8). Two rounds (mid-loop Clear, then a fresh Add whose box
// is freed at scope end) exercise both the explicit-Clear and the
// scope-end-after-Clear paths.
// ---------------------------------------------------------------------------

BEZEL_TEST(sorted_dict_value_box_freer_3776, clear_frees_surviving_boxes_4_17) {
    const std::string src =
        "module M; "
        "record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    using var d := new SortedDictionary<int, Pt>(); "
        "    d.Add(1, new Pt(3, 4)); "
        "    d.Add(2, new Pt(5, 6)); "
        "    d.Clear(); "                       // frees both boxes now
        "    d.Add(3, new Pt(7, 8)); "           // one more box, freed at scope end
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// Indexer overwrite (`d[k] := newVal`) — stdlib §4.17's indexer setter is
// "insert or replace"; replacing an EXISTING key's boxed value must free the
// DISPLACED box exactly once (not leaked, not double-freed against the
// surviving final box or the scope-end free). Two overwrites of the same key
// per iteration exercise two displacements before the final scope-end free
// of the one surviving box.
// ---------------------------------------------------------------------------

BEZEL_TEST(sorted_dict_value_box_freer_3776,
           indexer_overwrite_frees_displaced_box_exactly_once_4_17) {
    const std::string src =
        "module M; "
        "record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new SortedDictionary<int, Pt>(); "
        "    d.Add(1, new Pt(3, 4)); "         // box #1
        "    d[1] := new Pt(9, 9); "           // displaces #1, boxes #2
        "    d[1] := new Pt(1, 1); "           // displaces #2, boxes #3 (survives)
        "    total := total + d[1].x + d[1].y; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=400"));  // 200 x (1 + 1)
}

// ---------------------------------------------------------------------------
// Nullable-value V (`int?`) — issue #539 / §4.8/§4.17/§7: a nullable value
// type lowers to a tagged {i8 hasValue; T} struct and is heap-boxed the same
// way a record is. Both a present and an absent value are stored, exercising
// the tag byte through the box round-trip.
// ---------------------------------------------------------------------------

BEZEL_TEST(sorted_dict_value_box_freer_3776,
           nullable_value_type_boxed_and_freed_4_17) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    using var d := new SortedDictionary<int, int?>(); "
        "    d.Add(1, 5); "
        "    d.Add(2, null); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// Class V — never boxed (a class value lowers to a pointer, `isStructTy()`
// is false, `dictValueBoxFreer` returns null — the SAME "no box" answer as
// before this fix). The `Conn` instance is a real owned class, disposed
// separately via its own `using var`; the dictionary only BORROWS it
// (§4.17's "borrowed for class types"). Proves the dictionary's (absent) box
// free never touches the class instance's own disposal.
// ---------------------------------------------------------------------------

BEZEL_TEST(sorted_dict_value_box_freer_3776, class_v_borrow_never_freed_by_map) {
    const std::string src =
        "module M; "
        "class Conn "
        "  public id: int; "
        "  public constructor(id: int) this.id := id; end constructor; "
        "end Conn; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var conn := new Conn(3); "
        "    using var d := new SortedDictionary<int, Conn>(); "
        "    d.Add(1, conn); "
        "    total := total + d[1].id; "
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
// String V — #429 / §28.9: a borrowing dict co-owns a +1 per stored string
// value, released via `stringElementReleaseThunk`, a code path independent
// of `dictValueBoxFreer` (a string is never struct-boxed). Regression guard
// that the head-name widening did not disturb the string-release arm.
// ---------------------------------------------------------------------------

BEZEL_TEST(sorted_dict_value_box_freer_3776, string_value_released_4_17) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    using var d := new SortedDictionary<int, string>(); "
        "    d.Add(1, \"hello\"); "
        "    d.Add(2, \"world\"); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// Object-parity regression: `Dictionary<K, record>` (the pre-existing #401
// shape, NOT SortedDictionary) is UNCHANGED by this fix — `isBoxedValueDict
// HeadName("Dictionary")` agrees with the old `headName() != "Dictionary"`
// test for every input (`"Dictionary" == "Dictionary"` on both sides), so
// this only guards against a refactor that accidentally narrowed the
// Dictionary path while widening it to admit SortedDictionary.
// ---------------------------------------------------------------------------

BEZEL_TEST(sorted_dict_value_box_freer_3776,
           plain_dictionary_record_value_still_freed_regression_401) {
    const std::string src =
        "module M; "
        "record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, Pt>(); "
        "    d.Add(1, new Pt(3, 4)); "
        "    total := total + d[1].x + d[1].y; "
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
