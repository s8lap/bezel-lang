// Codegen + sema — fix #3510 / stdlib §4.8, §4.9, §4.17 (v0.285):
// the dictionary family's `GetOrNull` — the §1 `*OrNull` convenience tier's
// keyed-lookup form (added v0.231), declared verbatim on all three families:
//
//   Dictionary<K, V>.GetOrNull(key: K): V?          (stdlib §4.8)
//   OwningDictionary<K, V>.GetOrNull(key: K): V?    (stdlib §4.9)
//   SortedDictionary<K, V>.GetOrNull(key: K): V?    (stdlib §4.17)
//
// The compiler implemented none of them. Measured on fca2f02 (v0.285), the CALL
// form was rejected outright:
//
//   no member `GetOrNull` on `SortedDictionary<T>` (§22.5 / §4.x) — it is not
//   part of this collection's method surface, a universal Object member, or a
//   LINQ operator
//
// — a diagnostic whose sibling text elsewhere in the same file ENUMERATES
// `GetOrNull` as part of the surface it was refusing. That is the shape the
// project calls a duplicated classification predicate: the member-access gate's
// `isDeclaredMethod` list already carried `GetOrNull`, while the CALL gate's
// `allowedNonOwningMethod` / `allowedOwningMethod` surface lists (the ones that
// actually fire) did not.
//
// The fix is three-layered, and the cases below pin all three:
//   1. sema surface (ExprTypecheck_Calls.cpp) — `GetOrNull` joins the two
//      §4.x call-surface lists, beside `GetOrAdd`.
//   2. sema typing — the result is V made NULLABLE. Without this the borrowing
//      families leave the call untyped and the sorted family is WORSE: its
//      block ends in a catch-all V-return arm that types the call as the
//      NON-nullable value type, so `var v: int := sd.GetOrNull(k)` would sail
//      past §7 (the identical clobber #3372 recorded for `TryAdd`).
//   3. codegen (LlvmBackend_Calls.cpp) — a `GetOrNull` arm in the dict-backed
//      handler: the `TryGetValue` probe + decode, with the out-parameter write
//      replaced by a `V?` value (the tagged `{i8, V}` struct for a value-typed
//      V, a nullable pointer for `string`/class V). The arm sits inside the
//      `isDict` block, whose predicate is the #1414 registry's
//      `common::isDictionaryName`, so one lowering serves all three families.
//
// Every case below FAILS on the pre-fix build (a hard compile reject, or a
// missing §7 diagnostic) and PASSES with the fix.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Plain program wrapper — no resource class.
std::string prog(const std::string& mainBody) {
    return "module M; class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

// Destructor-counting resource, for the OwningDictionary borrow oracle.
std::string progR(const std::string& mainBody) {
    return
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  public Tag: int := 0; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;";
}

bool hasError(const std::string& program, const std::string& needle) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// stdlib §4.8 (v0.285) — Dictionary<K, V>.GetOrNull(key): V?
// ---------------------------------------------------------------------------

// stdlib §4.8: "`null` when the key is absent, otherwise the value". BOTH
// answers are asserted — a test that only reads the present-key value would
// pass over a lowering that returned the value unconditionally.
// Encoded: (present ?? 0) * 10 + (absent ?? 7) -> 10 * 10 + 7 = 107.
BEZEL_TEST(dict_family_getornull_3510, dictionary_generic_key_present_and_absent_4_8) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new Dictionary<int, int>(); "
        "d.Add(1, 10); "
        "var v := d.GetOrNull(1); "
        "var w := d.GetOrNull(9); "
        "return (v ?? 0) * 10 + (w ?? 7); ")), 107);
}

// stdlib §4.8: the same contract on the STRING-key fast path
// (bz_dict_try_get_str), a separate codegen edge from the generic-key one.
// Encoded: 10 * 10 + 7 = 107.
BEZEL_TEST(dict_family_getornull_3510, dictionary_string_key_present_and_absent_4_8) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new Dictionary<string, int>(); "
        "d.Add(\"k\", 10); "
        "var v := d.GetOrNull(\"k\"); "
        "var w := d.GetOrNull(\"nope\"); "
        "return (v ?? 0) * 10 + (w ?? 7); ")), 107);
}

// stdlib §4.8's own worked idiom, verbatim from the prose: at a value-typed V
// the borrowed result composes with an owned fallback through `??`
// (`counts.GetOrNull(word) ?? 0`).
BEZEL_TEST(dict_family_getornull_3510, dictionary_counting_idiom_coalesce_4_8) {
    BEZEL_EXPECT_EQ(run(prog(
        "var counts := new Dictionary<string, int>(); "
        "counts[\"a\"] := 4; "
        "return (counts.GetOrNull(\"a\") ?? 0) + (counts.GetOrNull(\"b\") ?? 0); ")),
        4);
}

// stdlib §4.8: a `string`-typed V — the result is a nullable POINTER, not the
// tagged value struct, so it exercises the other half of the codegen decode.
// The §4.8 prose gives this exact composition: `config.GetOrNull(key) ??
// "default"`.
BEZEL_TEST(dict_family_getornull_3510, dictionary_string_value_present_and_absent_4_8) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var config := new Dictionary<string, string>(); "
        "config[\"greeting\"] := \"hi\"; "
        "var a := config.GetOrNull(\"greeting\") ?? \"default\"; "
        "var b := config.GetOrNull(\"missing\") ?? \"default\"; "
        "Console.WriteLine($\"{a}|{b}\"); "
        "return 0; ")),
        "hi|default");
}

// ---------------------------------------------------------------------------
// stdlib §4.17 (v0.285) — SortedDictionary<K, V>.GetOrNull(key): V?
// ---------------------------------------------------------------------------

// stdlib §4.17: "`GetOrNull` (v0.231) follows the same read semantics" and is
// listed among the O(log n) point operations. Same both-answers oracle.
// Encoded: 10 * 10 + 7 = 107.
BEZEL_TEST(dict_family_getornull_3510, sorted_dictionary_generic_key_present_and_absent_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "var sd := new SortedDictionary<int, int>(); "
        "sd.Add(1, 10); "
        "var v := sd.GetOrNull(1); "
        "var w := sd.GetOrNull(9); "
        "return (v ?? 0) * 10 + (w ?? 7); ")), 107);
}

// stdlib §4.17: string-key sorted map — the string fast path on the sorted
// family (which shares the bz_dict_t* representation per the #1414 registry).
BEZEL_TEST(dict_family_getornull_3510, sorted_dictionary_string_key_present_and_absent_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "var sd := new SortedDictionary<string, int>(); "
        "sd.Add(\"k\", 10); "
        "var v := sd.GetOrNull(\"k\"); "
        "var w := sd.GetOrNull(\"nope\"); "
        "return (v ?? 0) * 10 + (w ?? 7); ")), 107);
}

// stdlib §4.17: a point lookup must find a key regardless of its position in
// the ascending order — insert out of order and read the least, the middle and
// the greatest, plus one absent key.
// Encoded: 1*8 + 1*4 + 1*2 + 1 = 15.
BEZEL_TEST(dict_family_getornull_3510, sorted_dictionary_point_lookup_across_the_order_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "var sd := new SortedDictionary<int, int>(); "
        "sd.Add(30, 3); sd.Add(10, 1); sd.Add(20, 2); "
        "return int((sd.GetOrNull(10) ?? -1) = 1) * 8 + "
        "       int((sd.GetOrNull(20) ?? -1) = 2) * 4 + "
        "       int((sd.GetOrNull(30) ?? -1) = 3) * 2 + "
        "       int((sd.GetOrNull(40) ?? -1) = -1); ")), 15);
}

// ---------------------------------------------------------------------------
// The SEMA layer — the result is `V?`, not `V`
// ---------------------------------------------------------------------------

// spec §7 + stdlib §4.8: `GetOrNull(key: K): V?` returns a NULLABLE value, so
// binding it to a non-nullable `int` is a §7 reject. This is the case codegen
// alone does not deliver: with the lowering arm but no typing arm the sorted
// family's catch-all V-return would type the call as bare `int` and this
// program would compile — the silent hole #3372 measured for `TryAdd`.
BEZEL_TEST(dict_family_getornull_3510, dictionary_result_nullable_not_assignable_to_int_spec_7) {
    BEZEL_EXPECT_TRUE(hasError(prog(
        "var d := new Dictionary<int, int>(); "
        "d.Add(1, 10); "
        "var v: int := d.GetOrNull(1); "
        "return v; "),
        "is nullable but the target type is non-nullable"));
}

// The same §7 assertion on the SORTED family, which had the catch-all arm.
BEZEL_TEST(dict_family_getornull_3510, sorted_dictionary_result_nullable_not_assignable_to_int_spec_7) {
    BEZEL_EXPECT_TRUE(hasError(prog(
        "var sd := new SortedDictionary<int, int>(); "
        "sd.Add(1, 10); "
        "var v: int := sd.GetOrNull(1); "
        "return v; "),
        "is nullable but the target type is non-nullable"));
}

// The pre-fix diagnostic must be GONE — asserted by text, not by
// `hasErrors()`, so the row cannot pass on some unrelated error.
BEZEL_TEST(dict_family_getornull_3510, no_longer_rejected_as_outside_the_surface_4_17) {
    auto r = runAot(prog(
        "var sd := new SortedDictionary<int, int>(); "
        "sd.Add(1, 10); "
        "var v := sd.GetOrNull(1); "
        "return v ?? 0; "));
    for (const auto& d : r.diagnostics.diagnostics()) {
        BEZEL_EXPECT_TRUE(d.message.find("no member `GetOrNull`") ==
                          std::string::npos);
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ---------------------------------------------------------------------------
// stdlib §4.9 (v0.285) — OwningDictionary<K, V>.GetOrNull(key): V?
// ---------------------------------------------------------------------------

// stdlib §4.9: "present -> a borrow of the stored value, the indexer's get
// posture — ownership stays with the dictionary: a look, not a take". The
// oracle is BOTH the value read through the borrow AND the destructor count:
// exactly one dispose, at the dictionary's own scope end. A `GetOrNull` that
// transferred ownership would show 2.
// Encoded: (Tag read through the borrow = 7) * 10 + destructor count = 71.
BEZEL_TEST(dict_family_getornull_3510, owning_dictionary_result_is_a_borrow_not_a_take_4_9) {
    BEZEL_EXPECT_EQ(run(progR(
        "var t := 0; "
        "var n := 0; "
        "do "
        "  using var od := new OwningDictionary<string, R>(); "
        "  od.Add(\"a\", new R(7)); "
        "  var got := od.GetOrNull(\"a\"); "
        "  if got <> null then t := got!.Tag; end; "
        "  var miss := od.GetOrNull(\"b\"); "
        "  if miss = null then t := t + 0; end; "
        "end; "
        "n := R._n; "
        "return t * 10 + n; ")), 71);
}
