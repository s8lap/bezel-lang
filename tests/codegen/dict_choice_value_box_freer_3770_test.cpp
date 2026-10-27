// Codegen — fix #3770 / stdlib §4.8 / §28.9 (v0.286): `dictValueBoxFreer`
// (`LlvmBackend_Disposal.cpp:4488`) decides whether a borrowing
// `Dictionary<K,V>`'s per-value heap box needs freeing at scope-end / Clear /
// indexer-overwrite. Before this fix it tested `valIsRecord ||
// valIsNullableVal` directly — the EXACT narrow predicate fix #3758 replaced
// on the DECODE side (`decodeDictValueAt`) — so a struct-lowering V that is
// neither a record nor a nullable value type (a `choice`, value-like OR
// borrow-bearing: `choiceStruct` lowers every classification to a bare LLVM
// struct, §35.1/§35.4) was excluded: `dictValueBoxFreer` returned null for a
// choice V and its box was NEVER freed. Every `using var d := new
// Dictionary<K, Choice>(); d.Add(...);` leaked one box per surviving entry —
// not specific to iteration; a program that never reads the dictionary back
// still leaked.
//
// FIX. `dictValueBoxFreer` now derives its answer from the SAME
// `dictValueIsBoxedStruct` predicate the decoder (`decodeDictValueAt`) and
// `lowerDictValuesFor` (#3768, `dict_values_choice_3768_test.cpp`) read
// (LlvmBackend_impl.hpp) — `dictValueIsBoxedStruct(lowerTypeRef(valTy))`
// instead of `valIsRecord || valIsNullableVal`. `isStructTy()` is a strict
// superset of the old predicate for every V this function is reachable with:
// `valIsRecord` and `valIsNullableVal` both already imply `isStructTy()`, so
// int / nullable-int / string / class V are UNCHANGED (none of them is
// struct-lowering) and only a struct-lowering V that is neither record nor
// nullable-value — a choice — newly qualifies.
//
// §6b — DOUBLE-FREE ANALYSIS (the mandatory deliverable on a unification
// lane, LANE_RULES §6b). The danger after widening a "does this need
// freeing" predicate is always a DOUBLE free: does anything else already
// free a choice value's box, or dispose its payload?
//   * The box itself: `dictValueBoxFreer` is the ONLY caller of
//     `valueBoxFreeThunk()` for a borrowing `Dictionary<K,V>`'s value slot —
//     confirmed by grep (`valueBoxFreeThunk` has exactly one call site, this
//     function). The decode side (`decodeDictValueAt`) only LOADS the struct
//     bytes out of the box (a borrow of the box's contents, never freeing
//     it) — see that function's own comment: "The box is owned by the dict
//     and must not be freed — we only read its bytes." So decode and
//     disposal never race over the same free.
//   * The choice's PAYLOAD: a choice's active-case payload can be (a) a
//     value (int, etc.) — nothing to dispose; (b) a `string` — refcounted,
//     handled identically to a record's string field (this fix does not
//     touch that path at all — `valueBoxFreeThunk` was always a plain
//     `free()`, unchanged by this fix, and the ENCODE side's string handling
//     is independent of whether the DISPOSE side frees the outer box); (c) a
//     class-typed field. A class-typed choice payload field can ONLY be a
//     BORROW here, never `owned`: an owning-classified choice is a move-only
//     value ([§15.3](move-only value types), §35.4 rule 1), and
//     `Dictionary<K, V>.Add(key: K, value: V)` takes `value` by an ordinary
//     (non-`owned`, non-`move`) parameter — a move-only V cannot reach that
//     signature (a NAMED local of move-only type is consumed, not copied, by
//     the call). So the only class-typed payload a LIVE `Dictionary<K,
//     Choice>` can hold is a borrow-bearing choice's raw pointer field, which
//     nothing in this dictionary's disposal cascade ever disposes — freeing
//     the box therefore reclaims exactly the malloc'd storage and touches
//     nothing the payload's real owner still needs. See
//     `class_like_choice_borrowed_class_field_boxed_and_freed_once_28_9`
//     below for the measured proof (a real owned `Connection`, disposed
//     separately via its own `using var`, survives the dictionary's
//     disposal with zero leak and zero double-free, 200x under ASan/LSan).
//   (Sema does NOT currently reject an owning-classified choice as a
//   Dictionary V at all — measured and filed separately as #3775, a
//   pre-existing gap this fix does not create or worsen: `valueBoxFreeThunk`
//   still only frees the box in that case too, exactly as it already did for
//   a record V — it just does not know to cascade into the payload, which is
//   #3775's scope, not this fix's.)
//
// FAIL-WITHOUT / PASS-WITH (measured against this lane's rebase, 89eb914):
// built a PARENT bezelc via `git archive HEAD` into a separate scratch tree
// (this worktree's uncommitted edits untouched). The issue's own repro (a
// 200x loop of bare `.Add()` x2 + `using var` scope-end dispose, NO for-in,
// NO GetEnumerator, NO LINQ) leaks `SUMMARY: AddressSanitizer: 4800 byte(s)
// leaked in 400 allocation(s)` on the PARENT and is LSan-clean on the FIX.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

}  // namespace

// ---------------------------------------------------------------------------
// The issue's own repro: bare `.Add()` x2 + scope-end dispose, NO read of the
// dictionary at all. FAIL-WITHOUT: 4800 B / 400 allocations leaked (measured
// above). PASS-WITH: LSan-clean.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_choice_value_box_freer_3770,
           add_only_no_iteration_disposed_once_4_8) {
    const std::string src =
        "module M; "
        "choice Shape Circle(r: int), Square(s: int); end Shape; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, Shape>(); "
        "    d.Add(1, Shape.Circle(3)); "
        "    d.Add(2, Shape.Square(4)); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// Indexer-overwrite (`d[k] := newVal`, displacing an existing choice value)
// is DEFERRED, not covered here. LlvmBackend_Assign.cpp's indexer-set
// `encodeValue` lambda (the ENCODE side of `d[k] := v`) boxes a struct-typed
// value only when `(valRec || valNullableVal) && isStructTy()` — the SAME
// narrow predicate family #3758/#3770 replaced elsewhere with the broad
// `isStructTy()`-derived test — so a choice value assigned via the indexer
// crashes the IR verifier (`ZExt only operates on integer`) on the VERY
// FIRST write to a fresh key, before displacement/disposal is even reached.
// Measured: `var d := new Dictionary<int, Shape>(); d[1] := Shape.Circle(3);`
// alone crashes on both this lane's base commit and this lane's fix
// (independent of #3770/#3768 — `.Add()`'s OWN `encodeValue` lambda in
// LlvmBackend_Calls.cpp already has fix #685's broad boxing test, which is
// why `.Add()` works and the indexer does not). `LlvmBackend_Assign.cpp` is
// outside this lane's file ownership (#3770/#3768: LlvmBackend_Disposal.cpp,
// LlvmBackend_Stmts.cpp, LlvmBackend_impl.hpp, tests/codegen/**) — filed
// separately as #3780 rather than fixed here. Once #3780 lands, the
// displaced-box / no-double-free coverage
// dict_record_value_leak_test.cpp's #401 rows carry for a record V should
// gain a choice-V sibling here.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Clear() — stdlib §4.8's `Clear()` "drops every entry" and must also reclaim
// each surviving value's box (the box is internal storage, not a user-visible
// disposal obligation: §4.8 "disposes neither keys and values" describes V
// itself, which a borrowing Dictionary never owns — the malloc'd box is a
// separate, compiler-internal reclaim, exactly as it already is for a record
// V). Two Clear() calls per iteration (mid-loop and at scope end via the
// EMPTY dict) exercise both the explicit and the scope-end-of-an-emptied-dict
// paths.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_choice_value_box_freer_3770, clear_frees_surviving_boxes_4_8) {
    const std::string src =
        "module M; "
        "choice Shape Circle(r: int), Square(s: int); end Shape; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, Shape>(); "
        "    d.Add(1, Shape.Circle(3)); "
        "    d.Add(2, Shape.Square(4)); "
        "    d.Clear(); "                   // frees both boxes now
        "    d.Add(3, Shape.Circle(5)); "   // one more box, freed at scope end
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// "Class-like" (borrow-bearing) choice V — a case payload field of CLASS
// type, unmarked (a BORROW, per §35.4 rule 2: "borrow-bearing"), NOT `owned`
// (an owning-classified choice cannot reach this signature at all — see the
// file header's §6b analysis and #3775). The `Connection` is a REAL owned
// class instance, disposed separately via its own `using var` — this is the
// row the brief's "choice payload with an owned class field disposed once"
// deliverable measures: the dictionary's box-free must reclaim ONLY the
// malloc'd struct storage (tag + raw pointer field) and must NEVER touch —
// let alone dispose — the `Connection` object the pointer field borrows.
// Both disposals (the dict's box-free, the Connection's own destructor path)
// run every iteration; zero leak and zero double-free proves neither
// interferes with the other.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_choice_value_box_freer_3770,
           class_like_choice_borrowed_class_field_boxed_and_freed_once_28_9) {
    const std::string src =
        "module M; "
        "class Connection "
        "  public id: int; "
        "  public constructor(id: int) this.id := id; end constructor; "
        "end Connection; "
        "choice Wrap Item(conn: Connection), None; end Wrap; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var conn := new Connection(7); "
        "    using var d := new Dictionary<int, Wrap>(); "
        "    d.Add(1, Wrap.Item(conn)); "
        "    total := total + (case d[1] of Item(var c) => c.id, None => 0); "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1400"));  // 200 x 7
}

// ---------------------------------------------------------------------------
// Object-parity regression: a record V (the pre-existing #401 shape) is
// UNCHANGED by this fix — `valIsRecord` already implied `isStructTy()`, so
// `dictValueIsBoxedStruct` answers identically. Guards against a refactor
// that accidentally narrowed the predicate while widening it.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_choice_value_box_freer_3770,
           record_value_still_boxed_and_freed_regression_401) {
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
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1400"));  // 200 x (3 + 4)
}
