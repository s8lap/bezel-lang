// Codegen — fix #3780 / stdlib §4.8 / §28.9 (v0.286): `Dictionary<K, V>`'s
// indexer-SET (`d[key] := value`) crashed the COMPILER with an LLVM IR
// verification failure ("ZExt only operates on integer") when `V` is a
// `choice` — even on the VERY FIRST write to a fresh key, no
// overwrite/displacement involved at all.
//
// ROOT CAUSE. `LlvmBackend_Assign.cpp`'s indexer-set `encodeValue` lambda
// (`lowerCollectionIndexerSet`) boxed a struct-typed value only when
// `(valRec || valNullableVal) && v->getType()->isStructTy()` — the EXACT
// narrow predicate family the #3758 (decode side) and #3770 (disposal side,
// `dictValueBoxFreer`) fixes already replaced with the broad
// `isStructTy()`-derived `dictValueIsBoxedStruct` predicate
// (LlvmBackend_impl.hpp). A struct-lowering V that is neither a record nor a
// nullable value type — i.e. a bare (non-nullable) `choice`, which lowers
// every classification (value-like or borrow-bearing) to a bare LLVM struct,
// §35.1/§35.4 — fell through the boxing arm to the scalar-widening tail,
// which tried to `zext` a struct-typed LLVM value: illegal, rejected by the
// IR verifier. `.Add()`'s OWN `encodeValue` lambda (LlvmBackend_Calls.cpp)
// already carries fix #685's broad, unconditional `isStructTy()` test, which
// is why `.Add()` worked correctly for the same V and the indexer-set did
// not.
//
// FIX. The indexer-set `encodeValue` lambda now reads `dictValueIsBoxedStruct`
// (the SAME shared predicate #3758/#3770 route through) instead of
// re-deriving its own eighth copy of the decision — see that predicate's
// declaration in LlvmBackend_impl.hpp for why `isStructTy()` alone is a
// strict superset of the old `(valRec || valNullableVal)` gate (a record's
// and a nullable value type's LLVM lowering both already imply
// `isStructTy()`, so nothing already admitted is narrowed).
//
// §6b — WHAT THE OLD PREDICATE EXCLUDED, DIFFED AGAINST WHAT THE NEW ONE
// ADMITS. The only axis that moves is a struct-lowering V that is NEITHER a
// record NOR a nullable value type — i.e. a bare (non-nullable) `choice`.
// Every other V kind this arm is reachable with (int, nullable-int, string,
// class, record, nullable-choice — nullable choice already satisfied
// `valNullableVal` via `isNullableChoiceTy`, fix #2447, so it was already
// boxed correctly BEFORE this fix) is UNCHANGED, because the old predicate
// already agreed with `isStructTy()` for it. No exclusion outside this diff
// was lifted. The now-unused local `valRec` (record-vs-not classification)
// was removed from `lowerCollectionIndexerSet`; nothing else in that
// function read it.
//
// §6b — DOUBLE-FREE / LEAK ANALYSIS (the mandatory deliverable after
// widening a "does this need boxing" predicate — the danger direction is
// storing a struct's bytes into a box that is then never freed, or freed
// twice). The DISPOSAL side of this exact widening was already fixed by
// #3770 (`dictValueBoxFreer`, routed through the same shared predicate) —
// the indexer-set's OWN overwrite path already calls `dictValueBoxFreer`
// (via `valDisp`) to free the DISPLACED value's box via
// `bz_dict_set_gen_disposing` / `bz_dict_set_str_disposing`
// BEFORE this fix landed; what was missing was only the matching ENCODE-side
// widening so a choice value could be boxed and stored in the first place.
// So the overwrite/displacement machinery below is NOT new — this fix only
// makes it reachable for a choice V. `overwrite_displaced_box_freed_no_
// double_free_4_8` below is the row `dict_choice_value_box_freer_3770_
// test.cpp` had to drop (its own comment names #3780 as the reason),
// looped 200x under the strict oracle: a fresh-key box is stored, then
// immediately displaced by an overwrite (its box freed once by the
// disposing setter), then the surviving box is freed once more at `using
// var` scope end — zero leak, zero double-free.
//
// A choice's payload can be a value (nothing to dispose — see the int-payload
// rows), a `string` (refcounted — see `string_payload_...`), or a
// borrow-bearing class field (see `class_payload_...`); an owning-classified
// choice cannot reach this signature at all (move-only, §15.3/§35.4 rule 1 —
// the pre-existing, separately filed #3775 sema gap, unrelated to this fix).
//
// `SortedDictionary` shares this exact arm (`isDictTypeName` admits it
// alongside `Dictionary`/`OwningDictionary`, and `lowerCollectionIndexerSet`
// has no per-name branch before the boxing decision) — see
// `sorted_dictionary_choice_value_indexer_set_same_arm_4_17` below, which is
// deliberately NOT run under the strict/LSan oracle: #3770's own commit
// measured and filed #3776, a SEPARATE, pre-existing gap where
// `dictValueBoxFreer`'s `headName() != "Dictionary"` guard means
// SortedDictionary never frees ANY value's box (record included, not
// choice-specific) on disposal OR on indexer-overwrite displacement. Before
// this fix a SortedDictionary<K, choice> indexer-set crashed the compiler
// outright (never reached #3776's leak at all); after this fix it compiles
// and leaks exactly like every other SortedDictionary V kind already does —
// this fix does not create or worsen #3776, it just makes choice V reach the
// same pre-existing gap every other V kind already sits in.
//
// FAIL-WITHOUT / PASS-WITH (measured against this lane's rebase, ef5b6f9 —
// a7fd8eb's #3770/#3768 fix cherry-picked onto 6d322f7): built a PARENT
// bezelc via `git archive ef5b6f9` into a separate scratch tree (this
// worktree's uncommitted fix untouched throughout). The issue's own repro
// (`var d := new Dictionary<int, Shape>(); d[1] := Shape.Circle(3);`) fails
// IR verification ("ZExt only operates on integer") and `bezelc -c` exits
// non-zero on the PARENT; compiles and runs correctly, printing the right
// value, on the FIX.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

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
// The issue's own repro: a FRESH key, no displacement at all. FAIL-WITHOUT:
// the compiler crashes IR verification (see file header). PASS-WITH: runs,
// prints the correct value. Looped 200x under the strict oracle as cheap
// insurance that the fresh-key box is freed at `using var` scope end.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_choice_value_indexer_set_3780,
           fresh_key_choice_value_indexer_set_4_8) {
    const std::string src =
        "module M; "
        "choice Shape Circle(r: int), Square(s: int); end Shape; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, Shape>(); "
        "    d[1] := Shape.Circle(3); "
        "    total := total + (case d[1] of Circle(var r) => r, Square(var s) => s); "
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
// Overwrite: the row `dict_choice_value_box_freer_3770_test.cpp` had to drop
// (its comment names this issue). Fresh key, then an OVERWRITE that displaces
// the just-stored box; the disposing setter (`dictValueBoxFreer`, fixed by
// #3770) frees the displaced box, the survivor is freed once more at scope
// end. 200x under the strict oracle: zero leak, zero double-free.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_choice_value_indexer_set_3780,
           overwrite_displaced_box_freed_no_double_free_4_8) {
    const std::string src =
        "module M; "
        "choice Shape Circle(r: int), Square(s: int); end Shape; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, Shape>(); "
        "    d[1] := Shape.Circle(3); "    // fresh key: boxed
        "    d[1] := Shape.Square(4); "    // overwrite: OLD box freed, NEW box stored
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// Self-overwrite `d[k] := d[k]` — issue #3780's own "Suggested fix" section
// explicitly asks for this: "confirm the newly-admitted encode path does not
// double-box or leak on a self-overwrite `d[k] := d[k]`". Unlike the #429
// string-V self-overwrite hazard (a raw refcounted pointer, where a
// release-before-retain ordering would dip the count to 0 before the new
// store retains it), a boxed struct V has no analogous risk: the RHS
// `d[1]` indexer-GET *decodes a fresh copy* of the struct's bytes out of the
// OLD box (a load, never a pointer alias — see `decodeDictValueAt`'s own
// "we only read its bytes" comment), the LHS indexer-SET then mallocs a
// BRAND-NEW box and stores that copy into it, and only THEN does the
// disposing setter free the OLD box (now displaced) — the old box is never
// touched again after its bytes were copied out, so there is no free-before-
// copy race and nothing to double-free. Looped 200x under the strict
// (ASan/LSan) oracle for the choice V case #3780 fixes; a record V sibling
// runs as a CONTROL confirming the pre-existing (#401) self-overwrite
// behaviour this fix does not touch is unchanged.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_choice_value_indexer_set_3780,
           self_overwrite_choice_value_no_double_free_4_8) {
    const std::string src =
        "module M; "
        "choice Shape Circle(r: int), Square(s: int); end Shape; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, Shape>(); "
        "    d[1] := Shape.Circle(3); "
        "    d[1] := d[1]; "   // self-overwrite: RHS copies out of the box the LHS displaces
        "    total := total + (case d[1] of Circle(var r) => r, Square(var s) => s); "
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

BEZEL_TEST(dict_choice_value_indexer_set_3780,
           self_overwrite_record_value_control_no_double_free_401) {
    const std::string src =
        "module M; "
        "record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, Pt>(); "
        "    d[1] := new Pt(3, 4); "
        "    d[1] := d[1]; "   // self-overwrite control — record V, unaffected by this fix
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

// ---------------------------------------------------------------------------
// Dictionary<string, choice> — the STRING-key branch of the SAME encodeValue
// lambda (`bz_dict_set_str[_disposing]` instead of `bz_dict_set_gen[_
// disposing]`), fresh key then overwrite. Confirms the fix is not
// gen-key-specific.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_choice_value_indexer_set_3780,
           string_key_choice_value_indexer_set_overwrite_4_8) {
    const std::string src =
        "module M; "
        "choice Shape Circle(r: int), Square(s: int); end Shape; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<string, Shape>(); "
        "    d[\"k\"] := Shape.Circle(3); "
        "    d[\"k\"] := Shape.Square(4); "   // overwrite via the string-key path
        "    total := total + (case d[\"k\"] of Circle(var r) => r, Square(var s) => s); "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=800"));  // 200 x 4
}

// ---------------------------------------------------------------------------
// Nullable-choice V (`Shape?`) — a SANITY regression, not a newly-admitted
// case: `isNullableChoiceTy` (fix #2447) already made `valNullableVal` true
// for a nullable choice, so this path was ALREADY boxed correctly under the
// OLD `(valRec || valNullableVal)` gate. Pinned here so a future refactor of
// `dictValueIsBoxedStruct` cannot silently narrow it back out. Fresh key,
// overwrite (displaces the tagged {i8, Shape} struct's box), and a null
// write, all through the indexer-set.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_choice_value_indexer_set_3780,
           nullable_choice_value_indexer_set_regression_2447) {
    const std::string src =
        "module M; "
        "choice Shape Circle(r: int), Square(s: int); end Shape; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, Shape?>(); "
        "    var a: Shape? := Shape.Circle(3); "
        "    d[1] := a; "
        "    var b: Shape? := Shape.Square(4); "
        "    d[1] := b; "              // overwrite: displaces the tagged-struct box
        "    var n: Shape? := null; "
        "    d[2] := n; "
        "    var got: Shape? := d[1]; "
        "    if got <> null then "
        "      total := total + (case got! of Circle(var r) => r, Square(var s) => s); "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=800"));  // 200 x 4
}

// ---------------------------------------------------------------------------
// Choice with a STRING-typed case payload — the payload is refcounted and
// travels inside the boxed struct's bytes. Fresh key, then an OVERWRITE that
// displaces a box holding an "ab" payload with one holding "cde". Loops 200x
// under the strict oracle: neither the new box nor the (unrelated) string
// refcounting logic double-frees or leaks.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_choice_value_indexer_set_3780,
           string_payload_choice_indexer_set_overwrite_no_uaf_28_9) {
    const std::string src =
        "module M; "
        "choice Msg Note(text: string), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, Msg>(); "
        "    d[1] := Msg.Note(\"ab\"); "
        "    d[1] := Msg.Note(\"cde\"); "   // overwrite: old box (holding \"ab\") freed
        "    total := total + (case d[1] of Note(var t) => t.Length, Empty => 0); "
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
// Choice with a borrow-bearing CLASS-typed case payload (§35.4 rule 2 — an
// owning-classified choice is move-only and cannot reach this signature at
// all, #3775). Two REAL `Connection` instances, each disposed separately via
// its own `using var`; the indexer-set OVERWRITE displaces the box holding
// the FIRST Connection's borrow with one holding the SECOND's. Proves the
// displaced box's free reclaims only the malloc'd struct storage (tag +
// pointer field) and never touches — let alone disposes — the Connection the
// pointer field borrows: both Connections are disposed on every iteration
// (200x), zero leak, zero double-free, alongside the boxes' own frees.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_choice_value_indexer_set_3780,
           class_payload_choice_indexer_set_overwrite_borrow_no_double_free_28_9) {
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
        "    using var conn1 := new Connection(7); "
        "    using var conn2 := new Connection(9); "
        "    using var d := new Dictionary<int, Wrap>(); "
        "    d[1] := Wrap.Item(conn1); "    // fresh key
        "    d[1] := Wrap.Item(conn2); "    // overwrite: old box (conn1 borrow) freed
        "    total := total + (case d[1] of Item(var c) => c.id, None => 0); "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1800"));  // 200 x 9
}

// ---------------------------------------------------------------------------
// SortedDictionary shares this exact arm (`isDictTypeName` admits it
// alongside Dictionary/OwningDictionary; `lowerCollectionIndexerSet` has no
// per-name branch before the boxing decision). Deliberately NOT run under the
// strict/LSan oracle: #3776 (filed by #3770's own commit, pre-existing,
// unrelated to this fix) means SortedDictionary never frees ANY value's box
// on disposal or on indexer-overwrite displacement, for ANY V kind. Before
// this fix this program crashed the compiler outright; after it, it compiles
// and round-trips the value correctly through a fresh-key write and an
// overwrite — the functional (non-leak) claim this fix is actually
// responsible for.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_choice_value_indexer_set_3780,
           sorted_dictionary_choice_value_indexer_set_same_arm_4_17) {
    const std::string src =
        "module M; "
        "choice Shape Circle(r: int), Square(s: int); end Shape; "
        "class C public static method Main(): int "
        "  using var d := new SortedDictionary<int, Shape>(); "
        "  d[1] := Shape.Circle(3); "
        "  d[1] := Shape.Square(4); "
        "  var total := (case d[1] of Circle(var r) => r, Square(var s) => s); "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=4"));
}
