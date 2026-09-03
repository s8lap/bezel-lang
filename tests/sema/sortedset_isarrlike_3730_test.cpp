// Sema — stdlib §4.18 / §5 (v0.286), issue 3730: `isArrLike`
// (ExprTypecheck_Calls.cpp) omitted `SortedSet<T>` from the head list that
// types the whole stdlib §5 operator surface on a builtin-collection
// receiver, so a call like `s.ToArray()` on a DIRECT `SortedSet<T>` receiver
// never acquired a type — `ctx_.exprType(ce)` stayed null, `lowerLocalDecl`
// read the initializer as a UNIT initializer, and the local's slot degraded
// to zero (#2434) with no diagnostic on the line itself. This file pins the
// fix (SortedSet admitted, §5 surface now types) and the §6b exclusion diff
// the admission required: several isArrLike arms would otherwise type a
// SortedSet call that has no lowering — either because §4.18 never declares
// the member at all, or because it IS a real stdlib §5 member but this
// compiler's direct-receiver codegen dispatch has no SortedSet arm for it
// (issue 3735, filed while measuring this fix — the SAME gap reproduced
// today on a plain `Set<int>` receiver, so it was pre-existing and family-
// wide for the dict-backed collections, not something this admission
// introduced).
//
// UPDATED BY fix #3735 (stdlib §4.10 / §4.18 / §5, v0.286), per lane rules
// §7: that second exclusion category is GONE except for one member. The §5
// EAGER terminals (Any/All/Count(pred)/Sum/Min/Max/Average/Aggregate/
// FirstOrNull/LastOrNull/SingleOrNull/Single/ElementAt) now materialize a
// Set-family receiver through the same `lowerToLazyEnumerator` source the §5
// chains already use, so Category C's rows are VALUE rows now, not reject
// rows. `Reverse` alone still rejects, retargeted to issue 3763 — §5's LAZY
// `Reverse()` has no sema TYPING arm on a direct collection receiver, which
// no codegen change can remedy. Category B (members §4.18 never declares)
// is untouched.
//
// VALUE correctness (does the newly-typed §5 surface actually run right) is
// pinned in tests/codegen/sortedset_isarrlike_3730_test.cpp; this file is the
// TYPING and REJECTION half — every row here asserts diagnostic TEXT, never
// `hasErrors()` alone (§3 of the lane rules: "assert diagnostic TEXT, not
// hasErrors()").
//
// Citations verified verbatim against docs/ at v0.286:
//   stdlib §4.18 `SortedSet<T>`:
//     "public class SortedSet<T>: IReadOnlyCollection<T> where T:
//      IComparable<T>"
//     declared members: constructor(), Count, Add, TryAdd, Contains, Remove,
//     Clear, First, Last — no Get/Peek/Pop/Dequeue/PopFirst/PopLast/PopAt/
//     IndexOf/Length, no Try-extraction family beyond TryAdd.
//   stdlib line 1467: "public interface IReadOnlyCollection<out T>:
//     IEnumerable<T>" — so stdlib §5's `extension IEnumerable<T>` block
//     applies to a SortedSet receiver normatively.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& mainBody) {
    return "module M;\n"
           "import System;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    using var s := new SortedSet<int>();\n"
           "    s.Add(30); s.Add(10); s.Add(20); s.Add(40);\n" +
           mainBody +
           "\n    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

JitResult reject(const std::string& mainBody) { return runAot(prog(mainBody)); }

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// ===========================================================================
// Category A — the fix ITSELF: the whole §5 surface now types (and lowers)
// on a direct SortedSet<T> receiver. `runAotOut` proves it runs, not just
// compiles.
// ===========================================================================

BEZEL_TEST(sortedset_isarrlike_3730, toarray_on_direct_receiver_now_typed) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var st := s.ToArray();\n"
                        "    Console.WriteLine(st.Count.ToString());")),
        "4");
}

BEZEL_TEST(sortedset_isarrlike_3730, contains_still_types_no_hashable_reject) {
    // fix #3730 (stdlib §4.18): `SortedSet<T>.Contains` must NOT take the
    // isArrLike `where T: hashable` gate (#3331/C5) that `Array<T>.Contains`
    // does — §4.18 declares its OWN unconstrained `Contains(item: T): bool`,
    // identity by `CompareTo`-equality (its `where T: IComparable<T>`
    // constraint), not hashable. Admitting SortedSet to isArrLike without
    // excluding it from that gate would spuriously run the hashable check
    // (and, for a T that is IComparable but not hashable, wrongly reject a
    // legal program) even though the dedicated `colName == "SortedSet"` arm
    // (ExprTypecheck_Calls.cpp:~7100) retypes Contains correctly afterward —
    // the diagnostic, once reported, is not undone by a later correct type.
    auto r = reject("    Console.WriteLine(s.Contains(20).ToString());");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, "hashable"));
}

// ===========================================================================
// Category B — §6b exclusion: members §4.18 never declares at all. These
// were ALREADY silently untyped (degrading via #2434) before this fix since
// isArrLike excluded SortedSet outright; the fix keeps them REJECTED — now
// with a located, LOUD diagnostic — rather than letting the new isArrLike
// admission newly type them with no lowering arm to back the type.
// ===========================================================================

BEZEL_TEST(sortedset_isarrlike_3730, get_rejected_not_declared) {
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        reject("    var x := s.Get(0);"), "no member `Get` on `SortedSet<T>`"));
}

BEZEL_TEST(sortedset_isarrlike_3730, peek_rejected_not_declared) {
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        reject("    var x := s.Peek();"),
        "no member `Peek` on `SortedSet<T>`"));
}

BEZEL_TEST(sortedset_isarrlike_3730, pop_rejected_not_declared) {
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        reject("    var x := s.Pop();"), "no member `Pop` on `SortedSet<T>`"));
}

BEZEL_TEST(sortedset_isarrlike_3730, dequeue_rejected_not_declared) {
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        reject("    var x := s.Dequeue();"),
        "no member `Dequeue` on `SortedSet<T>`"));
}

BEZEL_TEST(sortedset_isarrlike_3730, popfirst_rejected_not_declared) {
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        reject("    var x := s.PopFirst();"),
        "no member `PopFirst` on `SortedSet<T>`"));
}

BEZEL_TEST(sortedset_isarrlike_3730, poplast_rejected_not_declared) {
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        reject("    var x := s.PopLast();"),
        "no member `PopLast` on `SortedSet<T>`"));
}

BEZEL_TEST(sortedset_isarrlike_3730, popat_rejected_not_declared) {
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        reject("    var x := s.PopAt(0);"),
        "no member `PopAt` on `SortedSet<T>`"));
}

BEZEL_TEST(sortedset_isarrlike_3730, indexof_rejected_not_declared) {
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        reject("    var x := s.IndexOf(10);"),
        "no member `IndexOf` on `SortedSet<T>`"));
}

BEZEL_TEST(sortedset_isarrlike_3730, length_call_rejected_not_declared) {
    // Bare-reference `s.Length` (no parens) is caught by a different,
    // pre-existing catch-all (~ExprTypecheck_Calls.cpp:18216) — this row is
    // the CALL form `s.Length()`, which that catch-all explicitly exempts
    // (`isCallForm`) and which reached codegen's #2434 degrade before this
    // fix's guard.
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        reject("    var x := s.Length();"),
        "no member `Length` on `SortedSet<T>`"));
}

BEZEL_TEST(sortedset_isarrlike_3730, trypop_rejected_not_declared) {
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        reject("    var out2: int?; var ok := s.TryPop(out out2);"),
        "no member `TryPop` on `SortedSet<T>`"));
}

BEZEL_TEST(sortedset_isarrlike_3730, trytake_rejected_not_declared) {
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        reject("    var out2: int?; var ok := s.TryTake(10, out out2);"),
        "no member `TryTake` on `SortedSet<T>`"));
}

// ===========================================================================
// Category C — WAS the §6b "declared by stdlib §5, but no direct-receiver
// codegen dispatch for a SortedSet source" exclusion (issue 3735). FLIPPED
// by fix #3735 (stdlib §4.10 / §4.18 / §5, v0.286): the §5 EAGER terminals
// now materialize a Set-family receiver through the SAME
// `lowerToLazyEnumerator` source the §5 chains already use
// (`LlvmBackend_Calls.cpp`'s `setRecvTerm` term in `lazyTerminal`), so every
// name below has a real lowering and #3444's "either a lowering or a loud
// reject" obligation is discharged by the LOWERING. Per lane rules §7 (a fix
// reversing a prior rejection must update the tests that pinned the old
// behaviour in the same commit), each row here is now a VALUE row rather
// than a reject row — and each asserts an ORDER-SENSITIVE value where §4.18's
// ascending guarantee is observable (`First`/`Last`/`ElementAt`/`Aggregate`),
// never a permutation-blind count.
//
// The one SURVIVOR is `Reverse` — for a reason that is not codegen's: §5's
// LAZY `Reverse(): IEnumerable<T>` has no sema TYPING arm at all on a direct
// collection receiver (`typeLinqOperator` never mentions it), so the call
// acquires no `exprType` and degrades at the NEXT line via #2434 whatever
// codegen does. Filed as issue 3763 with the measured repro; its row below
// pins the retargeted diagnostic.
//
// The source set for every row is the file's own `prog()` prelude:
// `SortedSet<int>` with 30, 10, 20, 40 added in that order, so §4.18's
// ascending iteration is 10, 20, 30, 40.
// ===========================================================================

BEZEL_TEST(sortedset_isarrlike_3730, any_now_dispatches) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine($\"{s.Any()}\");")), "True");
}

BEZEL_TEST(sortedset_isarrlike_3730, any_predicate_now_dispatches) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine($\"{s.Any(lambda(y) => y > 15)},"
                        "{s.Any(lambda(y) => y > 99)}\");")),
        "True,False");
}

BEZEL_TEST(sortedset_isarrlike_3730, all_now_dispatches) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine($\"{s.All(lambda(y) => y > 0)},"
                        "{s.All(lambda(y) => y > 15)}\");")),
        "True,False");
}

BEZEL_TEST(sortedset_isarrlike_3730, count_predicate_now_dispatches) {
    // The no-predicate `Count()` keeps its own O(1) `bz_dict_count` arm (the
    // row just below); only the predicate overload takes the new snapshot
    // route. 20, 30 and 40 are > 15.
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine($\"{s.Count(lambda(y) => y > 15)}\");")),
        "3");
}

BEZEL_TEST(sortedset_isarrlike_3730, count_noarg_still_types_and_runs) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(s.Count().ToString());")),
                    "4");
}

BEZEL_TEST(sortedset_isarrlike_3730, sum_min_max_now_dispatch) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine($\"{s.Sum()},{s.Min()},"
                        "{s.Max()}\");")),
        "100,10,40");
}

BEZEL_TEST(sortedset_isarrlike_3730, average_now_dispatches) {
    // stdlib §5.1 `Average(): double` over an int source; rendered through
    // `int(…)` so the row asserts the numeric value rather than `double`'s
    // exponent formatting.
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    var avg := s.Average(lambda(y) => y);\n"
                        "    Console.WriteLine($\"{int(avg)}\");")),
        "25");
}

BEZEL_TEST(sortedset_isarrlike_3730, aggregate_now_dispatches_in_ascending_order) {
    // ORDER-SENSITIVE: one decimal place per element, so this is 10203040 for
    // §4.18's ascending order and a different number for any permutation —
    // the property #3711's sorted snapshot exists to guarantee, now observed
    // through an EAGER terminal for the first time.
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine($\"{s.Aggregate(0, "
                        "lambda(acc, y) => acc * 100 + y)}\");")),
        "10203040");
}

BEZEL_TEST(sortedset_isarrlike_3730, ornull_family_now_dispatches_ascending) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine($\"{s.FirstOrNull()},"
                        "{s.LastOrNull()},"
                        "{s.SingleOrNull(lambda(y) => y = 10)}\");")),
        "10,40,10");
}

BEZEL_TEST(sortedset_isarrlike_3730, single_now_dispatches) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine($\"{s.Single(lambda(y) => y = 10)}\");")),
        "10");
}

BEZEL_TEST(sortedset_isarrlike_3730, elementat_now_dispatches_ascending) {
    // ORDER-SENSITIVE: ascending indices 0..3 are 10, 20, 30, 40.
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine($\"{s.ElementAt(0)},{s.ElementAt(1)},"
                        "{s.ElementAt(2)},{s.ElementAt(3)}\");")),
        "10,20,30,40");
}

BEZEL_TEST(sortedset_isarrlike_3730,
           first_last_dict_arm_agrees_with_snapshot_elementat) {
    // `First()`/`Last()` keep the dedicated `bz_dict_min_key_gen` /
    // `bz_dict_max_key_gen` arm (fix #3735 excludes them from the snapshot
    // route deliberately). The two surfaces must agree — the property
    // #2471/#2472 established.
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine($\"{s.First() = s.ElementAt(0)},"
                        "{s.Last() = s.ElementAt(3)}\");")),
        "True,True");
}

BEZEL_TEST(sortedset_isarrlike_3730, reverse_rejected_no_sema_typing_3763) {
    // The ONE survivor of the old Category C. Retargeted from issue 3735
    // (now fixed) to issue 3763: stdlib §5's LAZY `Reverse(): IEnumerable<T>`
    // has no typing arm on a direct collection receiver, so the reject keeps
    // the failure LOUD and located rather than letting it reach the #2434
    // degrade one line later (§6c).
    auto r = reject("    using var r := s.Reverse().ToArray();");
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "`Reverse` is declared on `SortedSet<T>` via stdlib \xC2\xA7" "5's"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "issue 3763"));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, "issue 3735"));
}

// ===========================================================================
// Category D — controls proving the exclusions are NARROW: the rest of the
// §5 surface (the LAZY operators, which route through
// `Emitter::lowerToLazyEnumerator`'s SortedSet arm — fixed by #3711 — and are
// unaffected by this fix's reject list) still types and runs.
// ===========================================================================

BEZEL_TEST(sortedset_isarrlike_3730, where_select_still_type_and_run) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var w := s.Where(lambda(y) => y > 15)."
            "Select(lambda(y) => y * 2).ToArray();\n"
            "    Console.WriteLine(w.Count.ToString());")),
        "3");
}

BEZEL_TEST(sortedset_isarrlike_3730, first_last_still_type_and_run) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    Console.WriteLine($\"{s.First()},{s.Last()}\");")),
        "10,40");
}

// ===========================================================================
// Category E — review finding (send-back after fa7352a): the Contains
// hashable-gate exclusion has TWO independent call sites in this file — the
// isArrLike Contains/TryAdd/Try* arm (Category A's
// `contains_still_types_no_hashable_reject` above covers THAT site, with an
// int element, which is always hashable and so cannot distinguish "the gate
// ran and passed" from "the gate did not run") and `typeLinqOperator`'s own
// applicability gate, reached via a SEPARATE if/else-if chain's unconditional
// fallback further down. The first commit excluded only the first site;
// `SortedSet<Bad>.Contains` — where `Bad` is IComparable<Bad> (legal per
// §4.18) but NOT hashable (a body record with a class-typed field, neither
// auto-hashable per §9.2 nor declaring custom equality) — reached the SECOND
// site and was wrongly rejected with "requires `T: hashable`" on fa7352a.
// `linqHashableGateApplies` is now the ONE predicate both sites consult.
//
// `Bad` is declared once as a receiver-parameter shape (`s: SortedSet<Bad>`
// / `s: Set<Bad>`) rather than via `new SortedSet<Bad>()` / `new Set<Bad>()`
// construction deliberately: constructing a `SortedSet<Bad>` hits a SEPARATE,
// pre-existing, orthogonal codegen defect (issue 3740, filed while measuring
// this) — reproduced identically on the tree BEFORE this fix, so it is not
// this fix's concern and must not gate these rows. These rows assert
// DIAGNOSTIC TEXT ONLY (never `r.ran`): sema either reports the hashable
// error or it does not, independent of whether codegen can fully lower the
// call afterward.
// ===========================================================================

// The `module M;` / `import System;` header MUST precede every declaration
// (a Bezel source file's module statement is first-or-nothing) — this helper
// includes it so neither call site can repeat (and potentially misorder) it.
std::string badRecordProg(const std::string& classCBody) {
    return "module M;\n"
           "import System;\n"
           "class R\n"
           "  public V: int := 0;\n"
           "  destructor end destructor;\n"
           "end R;\n"
           "record Bad: IComparable<Bad>\n"
           "  public Item: R;\n"
           "  public constructor(i: R) Item := i; end constructor;\n"
           "  public method CompareTo(other: Bad): int return 0; end "
           "CompareTo;\n"
           "end Bad;\n"
           "class C\n" +
           classCBody +
           "\n  public static method Main(): int\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

BEZEL_TEST(sortedset_isarrlike_3730,
           sortedset_bad_record_contains_accepted_second_site) {
    // FAILS on fa7352a (the first-site-only fix): reported the hashable
    // error via typeLinqOperator's independent gate. PASSES after the
    // linqHashableGateApplies dedup: `Bad` is IComparable (§4.18's own
    // requirement) but not hashable, and SortedSet<T>.Contains does not
    // require hashable at all (its own §4.18 signature is unconstrained).
    auto r = runAot(badRecordProg(
        "  public static method CheckSorted(s: SortedSet<Bad>): bool\n"
        "    return s.Contains(s.First());\n"
        "  end CheckSorted;"));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, "requires `T: hashable`"));
}

BEZEL_TEST(sortedset_isarrlike_3730,
           set_bad_record_contains_still_rejected_hashable_required) {
    // Control: `Set<T>` has NO native override of `Contains` the way
    // `SortedSet<T>` does — its `Contains` IS the §5 extension member, so the
    // hashable gate must still fire for a non-hashable element. Proves
    // `linqHashableGateApplies`'s exclusion is narrow (SortedSet + Contains
    // only), not a blanket "skip the hashable gate" that would silently
    // widen past this fix's intended scope.
    auto r = runAot(badRecordProg(
        "  public static method CheckSet(s: Set<Bad>): bool\n"
        "    return s.Contains(s.First());\n"
        "  end CheckSet;"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "requires `T: hashable`"));
}

}  // namespace
