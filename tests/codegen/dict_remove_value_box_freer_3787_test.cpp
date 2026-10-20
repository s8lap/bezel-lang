// Codegen — fix #3787 / stdlib §4.8 / §4.17 / §28.9 (v0.286): `Dictionary<K,V>
// .Remove(key)` / `SortedDictionary<K,V>.Remove(key)` never freed a removed
// entry's boxed value (record, nullable value type, choice) — the ONLY
// disposal call site in `LlvmBackend_Calls.cpp` without the `dictValueBoxFreer`
// fallback that scope-end (`LlvmBackend_Setup.cpp`), `.Clear()` (this file,
// ~L7015) and the indexer-overwrite path (`LlvmBackend_Assign.cpp`) already
// have.
//
// ROOT CAUSE (`LlvmBackend_Calls.cpp`'s `.Remove(k)` arm, ~L7241, both the
// generic-key and string-key branches): `valDisp` was derived only from
//   llvm::Function* valDisp = dictValClassDisposer;      // OwningDictionary<K,class>
//   if (!valDisp && dictIsStringValue)
//       valDisp = stringElementReleaseThunk();            // borrowing Dictionary<K,string>
// — with no `if (!valDisp) valDisp = dictValueBoxFreer(recvTy);` fallback, so
// a borrowing `Dictionary<K,V>` (or `SortedDictionary<K,V>`) whose V is
// heap-boxed leaked one box per removed entry. Repro (issue's own): 200x
// `Dictionary<int, Pt>` with two `.Add()`s then two `.Remove()`s then
// `.Dispose()` — the dict is empty at Dispose time, so any leak is 100%
// attributable to Remove — leaked "3200 byte(s) leaked in 400 allocation(s)"
// on the pre-fix compiler.
//
// FIX. Added the SAME `if (!valDisp) valDisp = dictValueBoxFreer(recvTy);`
// fallback at the SAME position in the cascade `.Clear()` already uses
// (class-disposer, then box-freer, then string-release) — one spelling,
// now four call sites (scope-end, Clear, indexer-overwrite, Remove).
//
// §6b — DOUBLE-FREE ANALYSIS. `valDisp` is looked up in the SAME
// `if (!valDisp)`-guarded cascade `.Clear()` uses, so:
//   * class V (OwningDictionary<K,class>): `dictValClassDisposer` is
//     non-null and short-circuits before `dictValueBoxFreer` is ever
//     called — UNCHANGED, byte-identical to before this fix.
//   * class V (borrowing Dictionary<K,class>): `dictValClassDisposer` is
//     null (not owning) and `dictValueBoxFreer` returns null too (a class
//     value lowers to a pointer, `isStructTy()` is false) — `valDisp` stays
//     null, falls through to the plain non-disposing `bz_dict_remove_*`
//     entry points, exactly as before — UNCHANGED.
//   * string V: `dictValueBoxFreer` returns null (string is never
//     struct-boxed), so the cascade falls through to the EXISTING
//     `dictIsStringValue -> stringElementReleaseThunk()` arm exactly as
//     before this fix — UNCHANGED.
//   * int / other scalar V: no box, `dictValueBoxFreer` returns null,
//     `valDisp` stays null — UNCHANGED.
//   * record / nullable-value / choice V (the newly-fixed case): the box
//     allocated by `encodeValue` at `.Add`/indexer-set time is freed
//     EXACTLY ONCE by the runtime's `dispose_val(e.val)` call inside
//     `bz_dict_remove_{gen,str}_disposing`, gated on `idx >= 0` (key found)
//     and `e.val` non-null, and the entry is unlinked in the SAME call
//     before returning — a subsequent `Remove` of the same key returns
//     `false` (idx < 0) and disposes nothing, and a subsequent scope-end
//     Dispose iterates only entries STILL in the table, so the removed
//     entry's box is never visited again. Remove-then-re-Add allocates a
//     FRESH box on the re-Add (via `encodeValue`), so the freed box and the
//     new one are distinct allocations — no double free.
//   * SortedDictionary<K,V>: `dictValueBoxFreer(recvTy)` reads `recvTy`'s
//     head name via `isBoxedValueDictHeadName` (fix #3776, cherry-picked
//     onto this branch — see below), so once #3776 lands SortedDictionary
//     reaches the SAME cascade Dictionary does; this fix required no
//     SortedDictionary-specific code because `.Remove(k)`'s codegen for a
//     generic key is head-name-agnostic (both heads share the same
//     bz_dict_t* runtime representation, #1414 registry).
//
// #3776 RELATIONSHIP: this issue's own scope note (filed while writing
// #3776's tests) states #3776 and #3787 are INDEPENDENT root causes — #3776
// is `dictValueBoxFreer`'s head-name GATE excluding SortedDictionary
// entirely (fixed in `LlvmBackend_Disposal.cpp`, owned by c12-b54, commit
// 49f201a4c1f365dda518403ca1889f3ed74ee04f); #3787 is THIS call site never
// even trying the fallback, for BOTH heads. 49f201a is cherry-picked onto
// this branch (underneath this fix) so both are closed together here — the
// SortedDictionary rows below exercise the composition of both fixes: WITH
// only this fix and WITHOUT #3776, `dictValueBoxFreer(recvTy)` would still
// return null for a SortedDictionary receiver (old `headName() !=
// "Dictionary"` gate) and the SortedDictionary rows would still leak; WITH
// both fixes (this branch's actual state) they are LSan-clean.
//
// FAIL-WITHOUT / PASS-WITH (§R12-H): measured against a PARENT bezelc built
// from this branch's pre-fix commit (i.e. with 49f201a already applied, so
// the SortedDictionary rows isolate #3787 specifically) in a separate
// scratch tree. Record V (Dictionary): "3200 byte(s) leaked in 400
// allocation(s)" on PARENT, LSan-clean on FIX. Choice V (Dictionary): "4800
// byte(s) leaked in 400 allocation(s)" on PARENT, LSan-clean on FIX.
// SortedDictionary record V: "3200 byte(s) leaked in 400 allocation(s)" on
// PARENT, LSan-clean on FIX.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 9 checked rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics, and then RETURNED from the row on
// it. That is an ABSORBING site by
// scripts/check_toolchain_skip_surface.py's own definition, and it is a silent
// vacuous pass: on a box whose clang cannot link the ASan binary, all
// 9 rows returned WITHOUT asserting and the suite reported a clean PASS
// having compiled and run nothing. MEASURED on the pre-change tree with the
// harness's own knob (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`): every row green.
//
// Every one of those guards becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position: the
// skip is still recognised as an environment condition, but it now MARKS THE
// ROW FAILED instead of hiding it. The lenient legs' assertions additionally
// now RUN UNCONDITIONALLY rather than being skipped, which is the other half of
// the strengthening. Every substantive assertion in every row (r.ran and the
// exitCode pins) stays exactly where it was.

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}


}  // namespace

// ---------------------------------------------------------------------------
// The issue's own repro (record V, Dictionary, regression tag #401): bare
// .Add() x2, .Remove() x2, explicit .Dispose() — the dict is EMPTY at
// Dispose time, so any leak is 100% attributable to Remove. FAIL-WITHOUT:
// 3200 B / 400 allocations leaked. PASS-WITH: LSan-clean.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_remove_value_box_freer_3787,
           record_value_removed_both_keys_no_leak_regression_401_4_8) {
    const std::string src =
        "module M; "
        "record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    var d := new Dictionary<int, Pt>(); "
        "    d.Add(2, new Pt(4, 5)); "
        "    d.Add(1, new Pt(3, 6)); "
        "    d.Remove(2); "
        "    d.Remove(1); "
        "    d.Dispose(); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// choice V (Dictionary) — same shape as #3770's own repro, but exercised
// through Remove instead of scope-end/Clear. Uses `.Add()` only (the
// indexer-SET `d[k] := choiceVal` form hits the separate, pre-existing
// #3780 compiler crash for a choice V — not exercised here).
// FAIL-WITHOUT: 4800 B / 400 allocations leaked. PASS-WITH: LSan-clean.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_remove_value_box_freer_3787,
           choice_value_removed_both_keys_no_leak_4_8) {
    const std::string src =
        "module M; "
        "choice Shape Circle(r: int), Square(s: int); end Shape; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, Shape>(); "
        "    d.Add(2, Shape.Square(4)); "
        "    d.Add(1, Shape.Circle(3)); "
        "    d.Remove(2); "
        "    d.Remove(1); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// nullable-value V (`int?`, Dictionary) — issue #539 / §4.8/§7: a nullable
// value type is boxed the same way a record is. Both a present and an
// absent value are Removed, exercising the tag byte through the box's
// removal path (not just the read path).
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_remove_value_box_freer_3787,
           nullable_value_type_removed_no_leak_4_8) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, int?>(); "
        "    d.Add(1, 5); "
        "    d.Add(2, null); "
        "    d.Remove(1); "
        "    d.Remove(2); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// class V (borrowing Dictionary<K, class>) — never boxed, so Remove must NOT
// free the class instance (`dictValueBoxFreer` returns null: a class value
// lowers to a pointer, `isStructTy()` is false). The `Conn` instance is a
// real owned class disposed separately via its own `using var`; reading
// `conn.id` directly AFTER the dict.Remove() proves the instance survived
// (no premature free, no double free at the later scope-end Dispose).
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_remove_value_box_freer_3787,
           class_value_remove_borrow_never_freed_4_8) {
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
        "    using var d := new Dictionary<int, Conn>(); "
        "    d.Add(1, conn); "
        "    d.Remove(1); "
        "    total := total + conn.id; "
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
// string V (Dictionary<int, string>) — #429 / §28.9 regression guard: a
// borrowing dict co-owns a +1 per stored string value, released on Remove
// via `stringElementReleaseThunk` (a path already correct BEFORE this fix).
// Proves the newly-added `dictValueBoxFreer` step ahead of it in the cascade
// does not disturb the string-release arm (string is never struct-boxed,
// so `dictValueBoxFreer` returns null and the existing arm still fires).
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_remove_value_box_freer_3787, string_value_remove_releases_once_4_8) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, string>(); "
        "    d.Add(1, \"hello\"); "
        "    d.Add(2, \"world\"); "
        "    d.Remove(1); "
        "    d.Remove(2); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// int V (Dictionary<int, int>) — no box at all (`int` never lowers to a
// struct); regression guard that Remove's value and control flow are
// UNCHANGED for the scalar, no-disposer case. Value-checked (not just
// LSan-checked) to also catch a control-flow regression.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_remove_value_box_freer_3787, int_value_remove_no_box_regression_4_8) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, int>(); "
        "    d.Add(1, 10); "
        "    d.Add(2, 20); "
        "    var hit := d.Remove(1); "
        "    total := total + int(hit) + d[2]; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=4200"));  // 200 x (1 + 20)
}

// ---------------------------------------------------------------------------
// OwningDictionary<K, class> (stdlib §4.9) — Remove disposes the owned value
// via `dictValClassDisposer`, which short-circuits BEFORE `dictValueBoxFreer`
// is ever consulted (`if (!valDisp) valDisp = dictValueBoxFreer(recvTy);`).
// Regression guard that this fix's cascade insertion did not disturb the
// pre-existing owning-Remove disposal path (already covered in depth by
// owned_collection_remove_test.cpp's M1 rows; this is a light touch confirming
// composition with the new fallback, not a re-derivation of that coverage).
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_remove_value_box_freer_3787,
           owning_dictionary_class_remove_disposes_once_4_9) {
    const std::string src =
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var d := new OwningDictionary<int, R>(); "
        "  d.Add(1, move (new R())); "
        "  d.Remove(1); "
        "  return R._n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runAot(src).exitCode, 1);  // destructor ran exactly once
}

// ---------------------------------------------------------------------------
// Missing key — `.Remove(key)` on an absent key returns false and disposes
// nothing; must not crash. `d` still holds its one entry afterward, proving
// the miss did not corrupt or free anything.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_remove_value_box_freer_3787,
           missing_key_remove_returns_false_no_crash_4_8) {
    const std::string src =
        "module M; "
        "record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, Pt>(); "
        "  d.Add(1, new Pt(3, 4)); "
        "  var hit := d.Remove(99); "
        "  return int(hit) * 100 + d[1].x + d[1].y; "
        "end Main; end C;";
    // hit=false(0) -> 0*100 + 3 + 4 = 7, and d[1] must still read back intact.
    BEZEL_EXPECT_EQ(runAot(src).exitCode, 7);
}

// ---------------------------------------------------------------------------
// Remove-then-re-Add the same key (record V) — the box freed by Remove and
// the box allocated by the following re-Add are DISTINCT allocations; the
// final scope-end Dispose must free only the SURVIVING (re-added) box, not
// double-free the removed one.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_remove_value_box_freer_3787,
           remove_then_readd_same_key_no_double_free_4_8) {
    const std::string src =
        "module M; "
        "record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, Pt>(); "
        "    d.Add(1, new Pt(3, 4)); "     // box #1
        "    d.Remove(1); "                // frees box #1
        "    d.Add(1, new Pt(9, 1)); "     // box #2 (fresh, distinct)
        "    total := total + d[1].x + d[1].y; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=2000"));  // 200 x (9 + 1)
}

// ---------------------------------------------------------------------------
// SortedDictionary<K, V> (stdlib §4.17), record V — composition of #3787
// (this fix) with #3776 (49f201a, cherry-picked underneath): dictValueBoxFreer
// now admits the SortedDictionary head name (#3776) AND .Remove(k) now calls
// it as a fallback (#3787). Same shape as the issue's own SortedDictionary
// repro. FAIL-WITHOUT (measured against a PARENT built from the pre-#3787
// commit, i.e. with only #3776 applied): "3200 byte(s) leaked in 400
// allocation(s)" — isolates #3787 specifically, since #3776 alone does not
// touch the Remove call site. PASS-WITH: LSan-clean.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_remove_value_box_freer_3787,
           sorted_dict_record_value_removed_no_leak_4_17) {
    const std::string src =
        "module M; "
        "record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    using var d := new SortedDictionary<int, Pt>(); "
        "    d.Add(2, new Pt(4, 5)); "
        "    d.Add(1, new Pt(3, 6)); "
        "    d.Remove(2); "
        "    d.Remove(1); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// SortedDictionary<K, V>, choice V — same composition, choice payload.
// FAIL-WITHOUT (this fix's own PARENT, #3776 already applied): "4800 byte(s)
// leaked in 400 allocation(s)". PASS-WITH: LSan-clean.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_remove_value_box_freer_3787,
           sorted_dict_choice_value_removed_no_leak_4_17) {
    const std::string src =
        "module M; "
        "choice Shape Circle(r: int), Square(s: int); end Shape; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    using var d := new SortedDictionary<int, Shape>(); "
        "    d.Add(2, Shape.Square(4)); "
        "    d.Add(1, Shape.Circle(3)); "
        "    d.Remove(2); "
        "    d.Remove(1); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}
