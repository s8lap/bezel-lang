// Codegen — narrow #3576 / stdlib §5, §4.8 (v0.286): the §5 element-access
// terminals `First()`, `Last()`, `ElementAt(i)` over `Dictionary<K,V>` (any
// non-string K) lowered to a SILENT zero-filled `DictionaryEntry<K,V>`.
//
// Sema already types the call correctly as `DictionaryEntry<K,V>` for
// `Dictionary<K,V>` (ExprTypecheck_Calls.cpp's shared `colName ==
// "Dictionary" || colName == "SortedDictionary"` block). Codegen had NO arm
// for it: `isArr` is false for a dict receiver (`isDictTypeName` is disjoint
// from `isCollectionTypeName`), and the `isDict` block in
// LlvmBackend_Calls.cpp previously covered only
// Add/TryAdd/ContainsKey/Remove/TryGetValue/GetOrNull/GetOrAdd/Clear/Count —
// so the call fell through to `nullptr` and `lowerLocalDecl`'s #2434
// defense-in-depth zero-filled the local SILENTLY. Measured on the parent
// commit (a0f775f), FAIL-WITHOUT:
//
//   vfirst.bzl:6:15: internal: lowerLocalDecl's initializer expression failed
//     to lower to a value for this local's slot (degraded to a
//     zero-initialized value) - #2434
//   dictFirst=0 dictLast=0 dictElemAt1=0
//
// — the #2434 note is not an error, so the program compiled, linked, ran, and
// printed zeros. Silently wrong, not rejected: exactly the loud->quiet trade
// §6c forbids, which is why this is #3576's whole subject. PASS-WITH the fix,
// same program: dictFirst=330 dictLast=110 dictElemAt1=110 (both proven
// mechanically — commit, revert src/ to the parent in a scratch copy, rebuild,
// observe the pre-fix numbers, restore, relink, re-observe the post-fix ones).
//
// THE FIX. Route First()/Last()/ElementAt(i) through the SAME key/value
// snapshot the for-in path uses (`lowerDictEntryFor`, LlvmBackend_Stmts.cpp —
// mirrored rather than called into; see the commit body's §6b-style note on
// why a shared helper is a separate, larger unification, filed as #3717),
// then decode ONLY the one index the terminal's own guard selects (empty ->
// InvalidOperationException for First/Last; out of range ->
// IndexOutOfRangeException for ElementAt, stdlib §5 line 3594).
//
// NARROWED OFF: a STRING key. Measured (200x under ASan/LSan): the entry's
// VALUE is correct, but the rebuilt `bz_string` Key leaks on every call — a
// `DictionaryEntry<K,V>` is a SYNTHETIC TypeRef (no TypeSymbol, no field
// list), so the general per-local disposal pass cannot know to release it,
// and unlike the for-in loop / the LINQ materializer (which each thread that
// release through their OWN consumption shape — a loop body, a wrapped
// array), a plain local bound to `d.First()`'s result has no such hook. This
// fix rejects that shape LOUDLY (a diagnostic) instead of shipping the leak;
// #3720 is the sema-side fix (give `DictionaryEntry` a real field list).
//
// SORTEDDICTIONARY AND OWNINGDICTIONARY ARE NOT COVERED — measured, not
// assumed, and unrelated to the string-key narrowing above (both fail on
// PLAIN int keys too). `SortedDictionary<K,V>.First()` stays LOUD-rejected
// end to end (a PRE-EXISTING, deliberate sema containment:
// ExprTypecheck_Calls.cpp's `dictEntryElemTyped` hand-off excludes these four
// names specifically so a later SortedDictionary-only catch-all clobbers the
// type back to `V` — its own comment cites #3576 by number as the reason).
// `OwningDictionary<K,V>` has no typing rule for these four names at all, so
// `od.First()` itself now lowers (this fix's arm is reached by every
// `isDictTypeName` receiver) but a consuming `fe.Key` still silently
// degrades, one level up, via #2491 instead of #2434 — not a regression (see
// the commit body), but not a fix for that family either. Both are filed as
// the sema-side follow-up, #3716 — a `src/sema/**` change outside this
// lane's file ownership.
//
// ORACLE. Plain-return rows assert the ENTRY yielded (Key*100+Value, so a
// wrong key or a wrong value both move the number). `runAotChecked` proves
// the per-key snapshot free-loop this fix adds does not leak or double-free
// the malloc'd raw-key blobs, looped 200x under ASan/LSan per LANE_RULES —
// and that the string-key reject fires with NO allocation at all (so it
// cannot leak either).
//
// REVIEW AMENDMENT (c6-b12-review, NEEDS-FIX). Two findings, both fixed here:
//
// (1) The empty/out-of-range GUARD-THROW paths leaked. `emitGuardThrow`
// diverges to the throw block and never reaches the free calls below it, but
// the arm snapshotted the key/value tables (two unconditional
// `bz_dict_keys_gen`/`bz_dict_values_gen` mallocs) BEFORE running the guard —
// measured 200x under ASan/LSan, `new Dictionary<int,int>().First()` inside a
// try/catch leaked 3200 B in 400 allocations (8 B keys-array + 8 B
// values-array per throw). Fixed by reading the count via `bz_dict_count`
// (no allocation) and running the guard on THAT count, deferring the
// snapshot to after the guard passes — the same "no allocation before
// reject" discipline the string-key reject already had. The four
// throw-contract rows below are now `runAotChecked`, 200x-amplified, so they
// FAIL on the pre-amendment commit (3200 B / 400 allocations) and PASS here
// (clean).
//
// (2) Class-valued-V ownership proof, which the brief required and the first
// pass shipped only as a single-shot `run()` row covering none of the three
// sub-checks. Added: a Dispose-counting `Dictionary<int, R>` row, 200x under
// ASan, covering (a) reading `fe.Value.Tag` while the dictionary is still
// alive and (c) the dictionary going out of scope exactly once per
// iteration with no leak and no double-free (the destructor-count oracle:
// `R._n` must equal the iteration count, not less — a leak — or crash — a
// double-free); and a record-VALUE row, `Dictionary<int, Box>` where `Box`
// wraps an `R`, exercising the decode's struct-load (`isStructTy()`) arm
// rather than the raw-pointer arm. (b) "the entry outliving the
// dictionary's scope": spec §9.7 rules out a record ever OWNING a
// class-typed field ("record restrictions: no `owned` fields... a record's
// reference-typed fields are always borrows") — so "a record-with-owned-field
// V" the review literally asked for cannot exist in Bezel; `Box.Item` is
// necessarily a borrow, which the record-VALUE row exercises instead. For
// the outlive question itself: the underlying value's TRUE owner is
// whoever constructed it (`r` above, not the borrowing `Dictionary`), so a
// borrow read out through `fe.Value` is well-defined for as long as THAT
// owner lives, independent of the dictionary's own scope — confirmed
// (`using var r`; `d` and `fe` both scoped to a nested `do...end` block that
// closes before the extracted `int` is returned; ASan-clean). But moving
// the ENTRY ITSELF (not a value already read out of it) across a scope
// boundary — assigning `d.First()` to a PRE-DECLARED outer local, or
// returning it from a helper method — hits the SAME "return-value
// expression failed to lower ... #2491" degradation already measured for
// OwningDictionary above, both times: `var fe: DictionaryEntry<int, R>; ...
// fe := d.First();` and `public method Mk(r: R): DictionaryEntry<int, R>
// ... return d.First(); end;` each compile clean but return 0/degrade
// silently. That is `LlvmBackend_Assign.cpp` / cross-method return-type
// territory — outside this lane's file ownership and orthogonal to this
// fix's own arm (which only lowers the CALL `d.First()` itself, correctly,
// regardless of what happens to its result afterward) — noted on #3720
// rather than force a misleading passing test for it here.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& mainBody) {
    return "module M; class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

bool hasError(const std::string& program, const std::string& needle) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// stdlib §4.8 — Dictionary<int, int>, insertion order (3 then 1)
// ---------------------------------------------------------------------------

// The issue's own repro, verbatim (built out of key order — 3 then 1 — so an
// accidental ascending-sort would also fail this row). Pre-fix: dictFirst=0
// dictLast=0 dictElemAt1=0 (the #2434 degradation, FAIL-WITHOUT, measured
// above). Post-fix: 330 / 110 / 110.
BEZEL_TEST(dict_first_last_elementat_3576, dictionary_first_last_elementat_4_8) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new Dictionary<int, int>(); "
        "d.Add(3, 30); d.Add(1, 10); "
        "var fe := d.First(); "
        "var le := d.Last(); "
        "var ea := d.ElementAt(1); "
        "return int(fe.Key * 100 + fe.Value = 330) * 100 + "
        "       int(le.Key * 100 + le.Value = 110) * 10 + "
        "       int(ea.Key * 100 + ea.Value = 110); ")), 111);
}

// stdlib §4.8: a class-typed V — the decode's pointer/borrow arm, not the
// scalar ptrtoint arm. `fe.Value.Tag` reads the borrow directly.
BEZEL_TEST(dict_first_last_elementat_3576, dictionary_class_value_first_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R public Tag: int := 0; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end R; "
        "class C public static method Main(): int "
        "  var d := new Dictionary<int, R>(); "
        "  d.Add(1, new R(7)); "
        "  var fe := d.First(); "
        "  return fe.Key * 10 + fe.Value.Tag; "
        "end Main; end C;"), 17);
}

// ---------------------------------------------------------------------------
// stdlib §5 line 3594 — throwing contracts
// ---------------------------------------------------------------------------

// The empty/out-of-range guard runs BEFORE this fix's snapshot allocation
// (review c6-b12-review finding 1: it did not, pre-amendment — the guard ran
// AFTER `bz_dict_keys_gen`/`bz_dict_values_gen` had already malloc'd the
// spine arrays, and `emitGuardThrow`'s throw path diverges before the frees
// below it, so every throw leaked both arrays). 200x under ASan/LSan so a
// per-throw leak is visible; measured pre-amendment: 3200 B in 400
// allocations (8 B keys-array + 8 B values-array x 200 throws) for the
// empty-First row alone. FAILS on the pre-amendment commit, PASSES here.
BEZEL_TEST(dict_first_last_elementat_3576, dictionary_first_empty_throws_5) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  var caught := 0; var i := 0; "
        "  while i < 200 do "
        "    var d := new Dictionary<int, int>(); "
        "    try var fe := d.First(); return fe.Key; "
        "    catch InvalidOperationException => caught := caught + 1; end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"caught={caught}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "caught=200"));
}

BEZEL_TEST(dict_first_last_elementat_3576, dictionary_last_empty_throws_5) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  var caught := 0; var i := 0; "
        "  while i < 200 do "
        "    var d := new Dictionary<int, int>(); "
        "    try var le := d.Last(); return le.Key; "
        "    catch InvalidOperationException => caught := caught + 1; end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"caught={caught}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "caught=200"));
}

BEZEL_TEST(dict_first_last_elementat_3576, dictionary_element_at_oor_throws_5) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  var caught := 0; var i := 0; "
        "  while i < 200 do "
        "    var d := new Dictionary<int, int>(); "
        "    d.Add(1, 10); "
        "    try var ea := d.ElementAt(5); return ea.Key; "
        "    catch IndexOutOfRangeException => caught := caught + 1; end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"caught={caught}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "caught=200"));
}

BEZEL_TEST(dict_first_last_elementat_3576, dictionary_element_at_negative_throws_5) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  var caught := 0; var i := 0; "
        "  while i < 200 do "
        "    var d := new Dictionary<int, int>(); "
        "    d.Add(1, 10); "
        "    try var ea := d.ElementAt(-1); return ea.Key; "
        "    catch IndexOutOfRangeException => caught := caught + 1; end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"caught={caught}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "caught=200"));
}

// ---------------------------------------------------------------------------
// Class-valued V — ownership proof (review c6-b12-review, mandatory check 1)
// ---------------------------------------------------------------------------

// (a) read `fe.Value.Tag` through the borrow WHILE the dictionary (and the
// value's true owner, `r`) are both still alive; (c) the value is disposed
// EXACTLY once per iteration — the `R._n` destructor-count oracle: less than
// 200 would mean a leak, a crash under ASan would mean a double-free.
// `Dictionary<K,V>` BORROWS V (`Add(key, value)` — no `owned` marker, stdlib
// §4.8), so `r` (not `d`) is V's true owner and disposes it; this fix's
// decode never retains or disposes the class-typed Value (a plain
// `dtValDecoded = slotV` read, §5's borrow contract), so 200 iterations must
// show exactly 200 dispositions of `r`, matching GetOrNull's identical
// contract (dict_family_getornull_3510_test.cpp).
BEZEL_TEST(dict_first_last_elementat_3576,
           dictionary_class_value_read_while_alive_disposed_once_28_9) {
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
        "    var fe := d.First(); "
        "    total := total + fe.Value.Tag; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total} n={R._n}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    // 200 x 7 == 1400; disposed exactly once per iteration == 200.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1400 n=200"));
}

// A RECORD-valued V whose own field is class-typed. spec §9.7 rules out a
// record ever OWNING a class-typed field ("no `owned` fields... a record's
// reference-typed fields are always borrows") — the reviewer's literal
// "record-with-owned-field V" cannot exist in Bezel, so this is the closest
// admissible shape: `Box.Item` is necessarily a borrow of `r`. Exercises the
// decode's struct-load (`isStructTy()`) arm rather than the class-value
// arm's raw-pointer path, 200x under ASan for the same read-while-alive /
// disposed-once proof.
BEZEL_TEST(dict_first_last_elementat_3576,
           dictionary_record_value_with_borrowed_class_field_28_9) {
    const std::string src =
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  public Tag: int := 0; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "public record Box(Item: R); "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var r := new R(9); "
        "    var d := new Dictionary<int, Box>(); "
        "    d.Add(1, new Box(r)); "
        "    var fe := d.First(); "
        "    total := total + fe.Value.Item.Tag; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total} n={R._n}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    // 200 x 9 == 1800; disposed exactly once per iteration == 200.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1800 n=200"));
}

// (b) "the entry outliving the dictionary's scope" — the VALUE (not the
// whole entry) demonstrably does: `d` and `fe` are both scoped to the
// nested `do...end` block, which closes (disposing `d`) BEFORE the
// extracted plain `int` crosses to the outer `return`. `r` (the value's
// TRUE owner, per the borrow contract above) is declared OUTSIDE that
// block and stays alive throughout, which is exactly why this is sound —
// see the header comment's REVIEW AMENDMENT paragraph for why moving the
// ENTRY ITSELF (not an int already read out of it) across a scope boundary
// hits an unrelated, pre-existing #2491 gap this fix cannot close.
BEZEL_TEST(dict_first_last_elementat_3576,
           dictionary_class_value_read_survives_dict_scope_exit_28_9) {
    const std::string src =
        "module M; "
        "class R public Tag: int := 0; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end R; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var r := new R(7); "
        "    var t := 0; "
        "    do "
        "      var d := new Dictionary<int, R>(); "
        "      d.Add(1, r); "
        "      var fe := d.First(); "
        "      t := fe.Value.Tag; "
        "    end; "
        "    total := total + t; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1400"));  // 200 x 7
}

// ---------------------------------------------------------------------------
// Leak/UAF safety of the per-key snapshot free loop this fix adds
// ---------------------------------------------------------------------------

// The malloc'd raw-key blob `bz_dict_keys_gen` returns per entry must be
// freed for EVERY entry, whether or not it is the one chosen — 200x under
// LSan so a leaked blob per iteration is visible.
BEZEL_TEST(dict_first_last_elementat_3576, dictionary_generic_key_snapshot_no_leak_28_9) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var d := new Dictionary<int, int>(); "
        "    d.Add(3, 30); d.Add(1, 10); d.Add(2, 20); "
        "    var fe := d.First(); var le := d.Last(); "
        "    total := total + fe.Key + le.Key; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    // 200 x (3 + 2) == 1000 (insertion order: First key=3, Last key=2).
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1000"));
}

// A record key (hashable, per §3.6) — the generic-key decode's struct-load
// arm, exercised at 200x so a boxed/record key copy would show as a leak.
BEZEL_TEST(dict_first_last_elementat_3576, dictionary_record_key_snapshot_no_leak_28_9) {
    const std::string src =
        "module M; "
        "public record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var d := new Dictionary<Pt, int>(); "
        "    d.Add(new Pt(1, 1), 10); "
        "    var fe := d.First(); "
        "    total := total + fe.Key.X + fe.Value; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=2200"));  // 200 x (1 + 10)
}

// ---------------------------------------------------------------------------
// String key — narrowed off, rejected loudly (no leak, no allocation at all)
// ---------------------------------------------------------------------------

// A string key must not reach this arm's lowering at all: the reject fires
// BEFORE any snapshot/malloc happens, so this is 0 allocations, not a freed
// one — the strongest form of "no leak" for the shape this fix declines.
BEZEL_TEST(dict_first_last_elementat_3576, dictionary_string_key_first_rejected_loudly_3720) {
    BEZEL_EXPECT_TRUE(hasError(prog(
        "var d := new Dictionary<string, int>(); "
        "d.Add(\"a\", 1); "
        "var fe := d.First(); "
        "return fe.Key.Length; "),
        "over a string-keyed Dictionary/SortedDictionary is not yet supported"));
}

BEZEL_TEST(dict_first_last_elementat_3576, dictionary_string_key_last_rejected_loudly_3720) {
    BEZEL_EXPECT_TRUE(hasError(prog(
        "var d := new Dictionary<string, int>(); "
        "d.Add(\"a\", 1); "
        "var le := d.Last(); "
        "return le.Key.Length; "),
        "over a string-keyed Dictionary/SortedDictionary is not yet supported"));
}

BEZEL_TEST(dict_first_last_elementat_3576, dictionary_string_key_element_at_rejected_loudly_3720) {
    BEZEL_EXPECT_TRUE(hasError(prog(
        "var d := new Dictionary<string, int>(); "
        "d.Add(\"a\", 1); "
        "var ea := d.ElementAt(0); "
        "return ea.Key.Length; "),
        "over a string-keyed Dictionary/SortedDictionary is not yet supported"));
}

// The string-value (non-string-key) case is NOT narrowed off — the leak is
// specific to the Key field's representation, not the Value's. Confirms the
// reject is keyed on K, not on "any string in the entry."
BEZEL_TEST(dict_first_last_elementat_3576, dictionary_string_value_int_key_first_works_4_8) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var d := new Dictionary<int, string>(); "
        "d.Add(1, \"hi\"); "
        "var fe := d.First(); "
        "Console.WriteLine(fe.Value); "
        "return 0; ")),
        "hi");
}

// ---------------------------------------------------------------------------
// SortedDictionary / OwningDictionary — the surviving, sema-side half (#3716)
// ---------------------------------------------------------------------------

// Pin: `SortedDictionary<K,V>.First()` stays a LOUD sema rejection, unchanged
// by this fix, and correctly so (a deliberate pre-existing containment, not
// this fix's job to lift — #3716). If this row ever starts PASSING (no
// diagnostic), #3716's sema fix has landed and this whole block — plus the
// `SortedDictionary` coverage it should gain — needs revisiting.
BEZEL_TEST(dict_first_last_elementat_3576,
           sorted_dictionary_first_still_rejected_pending_3716) {
    BEZEL_EXPECT_TRUE(hasError(prog(
        "var sd := new SortedDictionary<int, int>(); "
        "sd.Add(1, 10); "
        "var fe := sd.First(); "
        "return fe.Key; "),
        "no member `Key` on primitive type `int`"));
}
