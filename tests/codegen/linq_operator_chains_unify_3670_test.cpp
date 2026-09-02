// Sema — issue #3670: four hand-written LINQ operator name chains survived
// fix #3589's unification in ExprTypecheck_Calls.cpp:
//
//   1. `allowedOwningMethod`'s and `allowedNonOwningMethod`'s ~38-name LINQ
//      tails (near byte-twins, confirmed identical by extraction) now both
//      read the shared `common::isBuiltinCollectionLinqOperatorName` table
//      (plus an explicit `Reversed`/`Sorted` addition — real Array<T> §4.4
//      copy-forms, not in that table). This is the FIRST direct test of the
//      OWNING arm (`allowedOwningMethod`) against this table family — #3589's
//      own regression file only exercised the non-owning `Array` receiver.
//   2. `linqLambdaElementType`'s `elemLambda` (~20 names, a lambda-parameter-
//      SHAPE question, not a name-existence one) now reads a new narrow
//      table, `common::isLinqElementLambdaOperatorName` — extracted
//      unchanged, not merged with the two name-existence tables (a
//      deliberately different vocabulary; see BuiltinRegistry.hpp).
//   3. `typeLinqOperator`'s four byte-identical-bodied `if` arms (11 names:
//      Where/Skip/Distinct, OrderBy/OrderByDescending,
//      Concat/TakeWhile/SkipWhile, Union/Intersect/Except — each doing
//      exactly `ctx_.setExprType(&ce, makeLazyEnumerable(srcElem)); return
//      true;`) are now one arm gated on a third new narrow table,
//      `common::isLinqLazyPassthroughOperatorName`, plus an explicit
//      `Take`-with-`OwningSet`-exclusion conjunct kept at the call site (see
//      that table's header for why `Take` is not folded in).
//
// This is a PURE ROUTING fix: no name was added to or removed from any of
// the four chains (verified by extracting each chain's exact name set and
// diffing it against its new table/table-union before writing this fix; see
// the fix #3670 commit body for the full §6b exclusion diff). These tests
// therefore are not FAIL-WITHOUT/PASS-WITH pins for a behavior change (there
// is none to pin) — they are refactor-safety tests for the merge itself,
// covering exactly the places a transcription mistake in the merge would
// show up: the owning-receiver arm (previously untested directly), the
// `Take`/`OwningSet` exclusion the four-arms-to-one merge had to preserve by
// hand, and one `elemLambda` name (`Count`'s predicate form) not covered by
// nearby tests.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// Per tests/README.md: a genuine compile-time rejection must be
// distinguished by asserting on the diagnostic TEXT, never by exit code or
// `!r.ran` alone.
bool hasErrorContaining(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// ── allowedOwningMethod: the OWNING-receiver arm, now routed onto
//    common::isBuiltinCollectionLinqOperatorName ──────────────────────────

// A bogus name (never a real member of any receiver family this predicate
// governs — stdlib §5's own "There is no ToList()" text, per fix #3589)
// called on a DIRECT owning receiver must still hit the precise
// collection-specific diagnostic, not a generic dispatch-arm fallback. This
// is the owning-arm twin of fix #3589's `tolist_rejected_precise_stdlib_5`
// (which only exercised the non-owning `Array` receiver).
BEZEL_TEST(linq_operator_chains_unify_3670, owning_array_bogus_member_rejected_precise) {
    // OwningArray<T> requires T: class (§4.2/§14.4, #1327) — use a trivial
    // owned class receiver rather than a primitive.
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C public static method Main(): int "
        "  using var oa := new OwningArray<Res>(); "
        "  oa.Add(new Res(1)); "
        "  var r := oa.ToList(); "
        "  return 0; "
        "end Main; end C;",
        "no member `ToList` on `OwningArray<T>` (\xC2\xA7""22.5 / \xC2\xA7""4.x)"));
}

// A real §5 LINQ operator (`Where`, present in the routed table, not in the
// native-surface part of the same lambda) called DIRECTLY on an owning
// receiver must still be admitted end-to-end (a VALUE assertion, not merely
// "not rejected by the wrong diagnostic") — proves the routing did not
// accidentally narrow accept, either.
BEZEL_TEST(linq_operator_chains_unify_3670, owning_array_where_sum_admitted_stdlib_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C public static method Main(): int "
        "  using var oa := new OwningArray<Res>(); "
        "  oa.Add(new Res(5)); oa.Add(new Res(3)); oa.Add(new Res(8)); oa.Add(new Res(1)); "
        "  return oa.Where(lambda(p) => p.Tag > 4).Sum(lambda(p) => p.Tag); "  // 5+8=13
        "end Main; end C;"), 13);
}

// `Reversed`/`Sorted` stay admitted at THIS gate on both receiver arms (they
// are real Array<T> §4.4 copy-forms — never declared on OwningArray<T> per
// stdlib §4.4/§4.5's own text — kept in the routed union for the same
// "valid to call at all, diagnosed more precisely downstream if not"
// generosity #3589's commit already documented for the non-owning arm). This
// re-checks the non-owning `Array` receiver #3589's own file pinned
// (`reversed_still_admitted_stdlib_4_4`), now through the SHARED table this
// fix routes both arms onto, confirming the merge did not narrow it.
BEZEL_TEST(linq_operator_chains_unify_3670, array_reversed_still_admitted_via_shared_table) {
    BEZEL_EXPECT_FALSE(hasErrorContaining(
        "module M; "
        "class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  using var rev := a.Reversed(); "
        "  return rev.Length; "
        "end Main; end C;",
        "no member `Reversed`"));
}

// ── typeLinqOperator: the merged lazy-passthrough arm, spanning what were
//    four separately-spelled `if` blocks — stress one call chain touching a
//    name from each of the four merged arms plus the Take/OwningSet-excluded
//    conditional, so a transcription mistake in the merge (a dropped name,
//    a misplaced OR) shows up as a wrong VALUE or a spurious reject ────────

BEZEL_TEST(linq_operator_chains_unify_3670, lazy_passthrough_merge_where_orderby_take) {
    // arm1 (Where) + arm2 (OrderBy) + the Take conditional (non-OwningSet,
    // so still the ordinary LINQ Take(count)), chained: filter, order
    // ascending, keep the first two -> smallest two of {7,2,9,4} that are
    // even is {2,4} -> sum 6.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(7); a.Add(2); a.Add(9); a.Add(4); "
        "  using var r := a.Where(lambda(x) => x mod 2 = 0)"
        "                  .OrderBy(lambda(x) => x)"
        "                  .Take(2).ToArray(); "
        "  return r.Get(0) + r.Get(1); "  // 2 + 4 = 6
        "end Main; end C;"), 6);
}

BEZEL_TEST(linq_operator_chains_unify_3670, lazy_passthrough_merge_concat_skipwhile) {
    // arm3 (Concat, SkipWhile): concat two sources, then drop the leading
    // run below 5. {1,2,6,3} ++ {8,9} = {1,2,6,3,8,9}; SkipWhile(<5) drops
    // 1,2 (6 is not <5, stop) -> {6,3,8,9} -> sum 26.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(6); a.Add(3); "
        "  using var b := new Array<int>(); "
        "  b.Add(8); b.Add(9); "
        "  using var r := a.Concat(b).SkipWhile(lambda(x) => x < 5).ToArray(); "
        "  var sum := 0; "
        "  for var i in r do sum := sum + i; end; "
        "  return sum; "  // 6+3+8+9 = 26
        "end Main; end C;"), 26);
}

BEZEL_TEST(linq_operator_chains_unify_3670, lazy_passthrough_merge_union_distinct) {
    // arm4 (Union) + arm1 (Distinct): {1,2,2} union {2,3} = {1,2,3} in
    // encounter order (already distinct by Union's own contract); Distinct
    // on top is a no-op check that both still resolve as the SAME lazy
    // passthrough shape when chained. Sum = 6.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(2); "
        "  using var b := new Array<int>(); "
        "  b.Add(2); b.Add(3); "
        "  using var r := a.Union(b).Distinct().ToArray(); "
        "  var sum := 0; "
        "  for var i in r do sum := sum + i; end; "
        "  return sum; "  // 1+2+3 = 6
        "end Main; end C;"), 6);
}

// ── linqLambdaElementType: common::isLinqElementLambdaOperatorName ───────

// `Count(predicate)` is in the extracted table (a name that, unlike most of
// its neighbours, is ABSENT from both name-existence LINQ tables — see the
// table's header for why) and is not exercised by the handful of nearby
// destructuring/SelectMany tests already in the suite. An un-annotated
// lambda parameter must be seeded with the source element type so `.Field`
// resolves in the predicate body. `Array<T>` declares its OWN native
// no-argument `.Count` property, which shadows the `Count(predicate)`
// extension by name (spec §26.5 rule 1) on a DIRECT Array receiver — so this
// chains through a `Where` first to reach a lazy `IEnumerable<T>` receiver,
// which declares no such property.
BEZEL_TEST(linq_operator_chains_unify_3670, elemlambda_count_predicate_seeds_param) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Item(Tag: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Item>(); "
        "  a.Add(new Item(1)); a.Add(new Item(2)); a.Add(new Item(3)); "
        "  return a.Where(lambda(p) => true).Count(lambda(p) => p.Tag > 1); "  // 2
        "end Main; end C;"), 2);
}
