// Ownership — fix #3638: §30.5 Pattern A await-confinement was not enforced for
// a task whose result is a plain BORROWING intrinsic collection.
//
// Spec (revision v0.286):
//   §30.5 "Result lifetime and task disposal", producer-side return-type rules —
//     "`Task<T>` for **class** T: the task may be awaited from multiple places.
//     Every awaiter receives a borrow of the same underlying result. The
//     result's lifetime is tied to the task — disposing the task disposes the
//     result. This is **Pattern A**."
//   §28.3 is the borrow/consume authority the §30.5 site rule cites; the
//     compiler's own Pattern A site message names both ("§30.5 / §28.3").
//
// THE DEFECT. Two passes classify a Pattern A result and only one of them could
// see a borrowing intrinsic collection:
//
//   * codegen — `patternAResultCollectionDisposer` (LlvmBackend_Disposal.cpp,
//     fix #2279, v0.236) recognizes `Array<T>`, `Dictionary<K,V>`, `Set<T>`,
//     `Stack<T>`, `Queue<T>`, `LinkedList<T>`, `OwningBuffer<T>` and the
//     `Owning*` forms, and frees the collection's `bz_array_t*`/`bz_dict_t*`/
//     `bz_llist_t*` backing at the task's K8 teardown boundary — exactly as it
//     would destruct a class-typed Pattern A result.
//
//   * ownership — `taskResultArgIsClass` (Ownership_Checks.cpp), the gate that
//     makes `awaitTaskKind` report `PatternA` and thereby makes
//     `checkAwaitSitesExpr` enforce the owning-location confinement, recognized
//     a user `class` declaration plus a hand-written allowlist of the SIX
//     `Owning*` collection heads and the sync-primitive result heads. The
//     BORROWING intrinsic collections were in NEITHER branch (they are builtins,
//     so `arg->declaration` is null and the class test cannot fire).
//
// So an UNCONFINED `var arr := await GetArr();` over a `Task<Array<int>>` was
// classified `ValueResult` — no site obligation at all — compiled with ZERO
// diagnostics, and then read memory codegen had already freed.
//
// MEASURED on the pre-fix compiler, one binary, thirty runs of the §30.5-
// unconfined program below: 15x exit=0 printing `0` where `6` is correct
// (silent miscompile) and 15x SIGSEGV — zero correct runs. Post-fix the program
// does not compile at all: the §30.5 Pattern A site diagnostic fires, which is
// what `unconfined_*` below pins. The `confined_*` row is the value oracle for
// the shape §30.5 admits: bound through a `using var` task local, the awaited
// borrow is bounded by the task's scope and the program prints `6` — amplified
// 500x inside the program so a reintroduced UAF cannot hide behind one lucky
// heap layout.
//
// THE FIX routes the ownership gate through
// `common::isPatternAResultCollectionHead` (src/common/PatternAResultCollection.hpp),
// a shared membership table whose header carries the exclusion diff against
// codegen's computation in both directions. `frozen_array_*` and
// `nested_owning_task_*` below pin the two deliberate EXCLUSIONS, so the
// widening stays a subset of what codegen actually disposes and cannot
// false-reject.

#include <string>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotOut;

namespace {

// The stable rule identifier for the §30.5 Pattern A site rejection — a
// §-reference plus the key term, per ownership_support.hpp's guidance, so a
// rewording of the sentence does not silently make these rows vacuous.
const char* kPatternASiteMessage = "syntactically-visible owning location";

// An async producer returning `Task<COLL>` plus a body that awaits it. `body`
// is spliced into a second async method, so `await` is legal.
std::string program(const std::string& coll, const std::string& producerBody,
                    const std::string& body) {
    return std::string("module M; ") +
           "class C " +
           "  public static async method P(): Task<" + coll + "> " +
           producerBody +
           "  end P; " +
           "  public static async method Run(): Task<int> " +
           body +
           "  end Run; " +
           "end C;";
}

// `Array<int>` — the issue's own shape. `new Array<int>()` + `Add` is the real
// stdlib surface (§4.4); the capacity ctor sets capacity, not length.
std::string arrayProgram(const std::string& body) {
    return program("Array<int>",
                   " var a := new Array<int>(); a.Add(1); a.Add(2); a.Add(3); "
                   " return a; ",
                   body);
}

// ---------------------------------------------------------------------------
// The defect: the UNCONFINED await over a borrowing intrinsic collection.
// Pre-fix these all compiled clean (awaitTaskKind reported ValueResult).
// ---------------------------------------------------------------------------

BEZEL_TEST(Ownership_PatternABorrowingCollectionResult3638,
           unconfined_await_of_task_array_rejected_30_5) {
    auto r = parseAndCheckOwnership(arrayProgram(
        " var arr := await P(); return arr.Length; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kPatternASiteMessage));
}

// The whole borrowing family, not just `Array`. Each head below is a member of
// `kPatternAResultCollectionHeads` AND takes a real free thunk in codegen's
// `patternAResultCollectionDisposer`, so each one leaked-then-dangled the same
// way. A grid rather than one row because the pre-fix allowlist's hole was
// per-head: adding only `Array` would have left five live UAFs.
BEZEL_TEST(Ownership_PatternABorrowingCollectionResult3638,
           unconfined_await_rejected_for_every_borrowing_collection_head_30_5) {
    struct Row {
        const char* coll;
        const char* producerBody;
    };
    const Row rows[] = {
        {"Array<int>",
         " var a := new Array<int>(); a.Add(1); return a; "},
        {"Set<int>",
         " var s := new Set<int>(); s.Add(1); return s; "},
        {"Stack<int>",
         " var s := new Stack<int>(); s.Push(1); return s; "},
        {"Queue<int>",
         " var q := new Queue<int>(); q.Enqueue(1); return q; "},
        {"LinkedList<int>",
         " var l := new LinkedList<int>(); l.AddLast(1); return l; "},
        {"Dictionary<int, int>",
         " var d := new Dictionary<int, int>(); d[1] := 1; return d; "},
    };
    int rejected = 0;
    for (const Row& row : rows) {
        auto r = parseAndCheckOwnership(program(
            row.coll, row.producerBody,
            " var c := await P(); return 0; "));
        const bool ok = r.diagnostics.hasErrors() &&
                        hasErrorMessage(r, kPatternASiteMessage);
        BEZEL_EXPECT_TRUE(ok);
        if (ok) ++rejected;
    }
    // A grid whose loop never ran, or whose rows all failed to parse for an
    // unrelated reason, would otherwise report success vacuously.
    BEZEL_EXPECT_EQ(rejected, 6);
}

// The owning collections were ALREADY admitted by the pre-fix allowlist. Pinned
// so the rewrite through the shared table cannot silently drop them (the fix is
// a widening — nothing previously rejected may become accepted).
BEZEL_TEST(Ownership_PatternABorrowingCollectionResult3638,
           unconfined_await_of_owning_collection_still_rejected_30_5) {
    auto r = parseAndCheckOwnership(program(
        "OwningArray<Box>",
        " var a := new OwningArray<Box>(); return a; ",
        " var c := await P(); return 0; "));
    // `Box` is undeclared in this minimal program, so sema will also complain;
    // what matters is that the §30.5 site rule is among the errors.
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kPatternASiteMessage));
}

// ---------------------------------------------------------------------------
// The admitted shape, with a VALUE oracle. §30.5 admits a `using var`-bound
// task local as the borrow's source; the task's scope end bounds the borrow, so
// the reads are all in-lifetime and the sum is exact.
// ---------------------------------------------------------------------------

BEZEL_TEST(Ownership_PatternABorrowingCollectionResult3638,
           confined_using_var_task_local_admitted_and_reads_correct_30_5) {
    auto r = parseAndCheckOwnership(arrayProgram(
        " using var t := P(); var arr := await t; "
        " var sum := 0; "
        " for var i in 0..<3 do sum := sum + arr[i]; end for; "
        " return sum; "));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kPatternASiteMessage));
}

// The end-to-end value. LOOP-AMPLIFIED: the produce/await/read cycle runs 500
// times in one process, so a reintroduced use-after-free cannot pass by landing
// in still-mapped memory on a single lucky iteration (LANE_RULES §3). Expected
// stdout is 500 * (1+2+3) = 3000; the pre-fix compiler could not even reach
// this program's confined shape reliably (its unconfined sibling produced `0`
// or SIGSEGV on every one of thirty runs).
BEZEL_TEST(Ownership_PatternABorrowingCollectionResult3638,
           confined_pattern_a_array_result_value_amplified_30_5) {
    const std::string src =
        "module M; "
        "class C "
        "  public static async method P(): Task<Array<int>> "
        "    var a := new Array<int>(); a.Add(1); a.Add(2); a.Add(3); "
        "    return a; "
        "  end P; "
        "  public static async method Run(): Task<int> "
        "    var total := 0; "
        "    for var k in 0..<500 do "
        "      using var t := P(); "
        "      var arr := await t; "
        "      for var i in 0..<3 do total := total + arr[i]; end for; "
        "    end for; "
        "    return total; "
        "  end Run; "
        "  public static method Main(): int "
        "    using var t := Run(); "
        "    t.Wait(); "
        "    Console.WriteLine(t.Result.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runAotOut(src), std::string("3000"));
}

// ---------------------------------------------------------------------------
// The two deliberate EXCLUSIONS from the shared table (§6b exclusion diff).
// Both exist so sema's admitted set stays a SUBSET of what codegen disposes;
// admitting either would be a false reject with no disposal to guard against.
// ---------------------------------------------------------------------------

// §4.19 `FrozenArray<T>` is in BuiltinRegistry's array-backed collection table
// but is NOT in codegen's `patternAResultCollectionDisposer` membership list, so
// codegen frees nothing for a `Task<FrozenArray<T>>`. It must therefore stay
// OUT of the ownership gate — this row is why the shared predicate is a closed
// table rather than a composition of the existing registry predicates.
BEZEL_TEST(Ownership_PatternABorrowingCollectionResult3638,
           frozen_array_result_not_pattern_a_no_false_reject_30_5) {
    auto r = parseAndCheckOwnership(program(
        "FrozenArray<int>",
        " var a := new Array<int>(); a.Add(1); "
        " return FrozenArray<int>.From(a); ",
        " var c := await P(); return 0; "));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kPatternASiteMessage));
}

// A nested Task-family result is BORROWED, not owned — codegen's own comment
// says disposing it at the outer teardown is "a double-free / UAF". `OwningTask`
// is excluded from the shared table so the ownership gate does not start
// treating `Task<OwningTask<U>>` as a Pattern A collection result.
BEZEL_TEST(Ownership_PatternABorrowingCollectionResult3638,
           nested_owning_task_result_not_a_collection_result_30_5) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") +
        "class Box public v: int; "
        "  public constructor(x: int) v := x; end constructor; "
        "end Box; "
        "class C "
        "  public static async method Inner(): OwningTask<Box> "
        "    return new Box(7); "
        "  end Inner; "
        "  public static async method Run(): Task<int> "
        "    var t := Inner(); "
        "    using var b := await t; "
        "    return b.v; "
        "  end Run; "
        "end C;");
    // Whatever else this program does or does not diagnose, the §30.5 Pattern A
    // *collection-result* site rule must not be what fires: `OwningTask<Box>`
    // is Pattern B and has its own rule.
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kPatternASiteMessage));
}

// A value-typed result carries no obligation at the site (§30.5 first bullet).
// The widening must not reach it — an unconfined `await` over `Task<int>` stays
// legal, which is the boundary of the whole change.
BEZEL_TEST(Ownership_PatternABorrowingCollectionResult3638,
           value_typed_task_result_still_unconfined_legal_30_5) {
    auto r = parseAndCheckOwnership(program(
        "int", " return 7; ", " var v := await P(); return v; "));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kPatternASiteMessage));
}

}  // namespace
