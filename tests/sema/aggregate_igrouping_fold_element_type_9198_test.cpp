// fix #9198 (stdlib §5 / spec §27.4, v0.359): an `Aggregate` fold over an
// `IGrouping<TKey, T>` seeded the fold lambda's ELEMENT parameter with the
// grouping KEY type.
//
// THE DEFECT. stdlib §5 declares
//
//     public extension IEnumerable<T>
//       public method Aggregate<TAccum>(seed: TAccum, fold: (TAccum, T) -> TAccum): TAccum;
//     end;
//
//     public interface IGrouping<TKey, T>: IEnumerable<T>
//       readonly property Key: TKey;
//     end IGrouping;
//
// so a grouping inherits §5's extension block with **T = the ELEMENT type**
// (the SECOND generic argument) — the first is the KEY. The fold lambda of
// `g.Aggregate(seed, lambda(acc, x) => …)` is therefore `(TAccum, T) -> TAccum`
// over the grouping's ELEMENT.
//
// `ExprTC::aggregateFoldLambdaParamTypes` (src/sema/ExprTypecheck_Calls.cpp)
// — the site that seeds an un-annotated fold lambda's two parameters — read
// the element as `recvTy->genericArgs[0]`, which is the KEY for an
// `IGrouping<TKey, T>`. The grouping TypeRef the GroupBy typing arm builds has
// `declaration == nullptr` and a synthetic `astNode`, so the "a user class is
// not a LINQ source" exclusion below the read did NOT fire and the KEY was
// seeded as the element.
//
// MEASURED at the base (r30-c1 @ c00f5a63f7): the issue's own program is
// REJECTED at compile time with
//
//     string member `.age` is not yet supported in codegen
//
// — the compiler looking for the record's `.age` member on a `string`. The
// symptom is the KEY type reaching the body, so the defect is observable as a
// compile REJECT (the program never runs), which is why every row below pins
// stdout: on the base the program does not print at all.
//
// THE FIX. The element read goes through the SHARED recognizer
// (`sema/GroupingElem.hpp`'s `iGroupingElemTypeRef`, which returns
// `genericArgs[1]`), the same one `linqSourceElementType` uses for the
// single-parameter LINQ seeding (#8542) and `ExprTypecheck.cpp` uses for the
// for-in loop-variable inference (#6627). Those two sites are the twin
// behaviours the issue leaves alone — they were already correct.
//
// ORACLE. `runAot` (the strict AOT path: fork-isolated `Driver::compileFile`
// + clang link + native exec), stdout pinned byte-for-byte after normalising
// the line ending. `runAot`, not `runAotChecked`: one matrix cell builds its
// grouping keys by concatenation, and a fresh string key leaks today
// (#6626 — `bz_grouping_free` frees the key DATA but never the string it
// points to), so LSan would fail that row for another reason. The fold's own
// accumulator discharge is #6395's subject and is not disturbed here.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// stdoutText is the program's raw captured stdout; the line ending is the
// platform's. Normalising "\r\n" to "\n" makes the pinned expectation
// platform-independent without weakening it — the whole output is compared,
// so a missing or extra line still fails the row.
std::string normalisedStdout(const JitResult& r) {
    std::string out = r.stdoutText;
    std::string out2;
    out2.reserve(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\r' && i + 1 < out.size() && out[i + 1] == '\n') continue;
        out2.push_back(out[i]);
    }
    return out2;
}

}  // namespace

// ===========================================================================
// THE ISSUE'S OWN REPRO — `record Person(name: string, age: int)` grouped by
// name, and an `Aggregate` fold over the group that reads the ELEMENT's
// member. §5's `fold: (TAccum, T) -> TAccum` makes `x` a `Person`.
//
// FAILS-WITHOUT: the program is REJECTED — "string member `.age` is not yet
//   supported in codegen" (the fold's element parameter seeded with the
//   `string` KEY), so it prints nothing at all.
// PASSES-WITH: "a=70" / "b=25".
// ===========================================================================
BEZEL_TEST(aggregate_igrouping_fold_element_type_9198,
           record_element_fold_reads_the_element_member) {
    auto r = runAot(
        "module M; "
        "import System.Collections.Linq; "
        "record Person(name: string, age: int); "
        "public class Program "
        "  public static method Main(): int "
        "    using var ps := new Array<Person>(); "
        "    ps.Add(new Person(\"ann\", 30)); "
        "    ps.Add(new Person(\"bob\", 25)); "
        "    ps.Add(new Person(\"ann\", 40)); "
        "    for var g in ps.GroupBy(lambda(p) => p.name) do "
        "      var total := g.Aggregate(0, lambda(acc, x) => acc + x.age); "
        "      Console.WriteLine(g.Key + \"=\" + total.ToString()); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors() == false);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(normalisedStdout(r) == "ann=70\nbob=25\n");
}

// ===========================================================================
// THE MATRIX CELL — the KEY type and the ELEMENT type are SWAPPED relative to
// the row above: the key is an `int` (the string's length) and the element is
// the `string` itself. §5's fold is `(TAccum, T) -> TAccum` over the ELEMENT,
// so `x` is the `string` and the accumulator is a `string`.
//
// This cell is what makes the defect a KEY/ELEMENT confusion rather than a
// `string`-vs-record confusion: on the base the fold's element parameter is
// the `int` KEY.
//
// FAILS-WITHOUT: the program is REJECTED — the fold's element parameter is
//   the `int` KEY, so `acc + x` is `string + int` (no such operator).
// PASSES-WITH: "2=aacc" / "1=b".
// ===========================================================================
BEZEL_TEST(aggregate_igrouping_fold_element_type_9198,
           string_element_fold_over_an_int_key_group) {
    auto r = runAot(
        "module M; "
        "import System.Collections.Linq; "
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new Array<string>(); "
        "    a.Add(\"aa\"); "
        "    a.Add(\"b\"); "
        "    a.Add(\"cc\"); "
        "    for var g in a.GroupBy(lambda(s) => s.Length) do "
        "      var joined := g.Aggregate(\"\", lambda(acc, x) => acc + x); "
        "      Console.WriteLine(g.Key.ToString() + \"=\" + joined); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors() == false);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(normalisedStdout(r) == "2=aacc\n1=b\n");
}

// ===========================================================================
// THE MATRIX CELL — the ELEMENT is an `int` and the KEY is a `string`: the
// opposite pairing again. The fold adds the element, so the element slot must
// be the `int`.
//
// The key is a LITERAL here on purpose: a key built by concatenation leaks
// today (#6626), and this row is about the fold's element type, not about the
// grouping's key buffer.
//
// FAILS-WITHOUT: the program is REJECTED — the fold's element parameter is
//   the `string` KEY, so `acc + x` is `int + string` (no such operator).
// PASSES-WITH: "k=6".
// ===========================================================================
BEZEL_TEST(aggregate_igrouping_fold_element_type_9198,
           int_element_fold_over_a_string_key_group) {
    auto r = runAot(
        "module M; "
        "import System.Collections.Linq; "
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(1); "
        "    a.Add(2); "
        "    a.Add(3); "
        "    for var g in a.GroupBy(lambda(i) => \"k\") do "
        "      var total := g.Aggregate(0, lambda(acc, x) => acc + x); "
        "      Console.WriteLine(g.Key + \"=\" + total.ToString()); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors() == false);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(normalisedStdout(r) == "k=6\n");
}

// ===========================================================================
// CONTROL 1 — THE ADMITTING TWIN: the same program with the fold's
// parameters ANNOTATED. The annotation supplies the parameter types the
// seeding now supplies, so this shape compiled and ran BEFORE the fix: it is
// green on both arms, and its value is that it shows the shape is genuinely
// ADMITTED rather than rejected for an unrelated reason (the rows above fail
// on the fold's element type, not on the call).
//
// FAILS-WITHOUT: no — green on the base too.
// PASSES-WITH: "ann=30" / "bob=40".
// ===========================================================================
BEZEL_TEST(aggregate_igrouping_fold_element_type_9198,
           annotated_fold_parameters_control) {
    auto r = runAot(
        "module M; "
        "import System.Collections.Linq; "
        "record Person(name: string, age: int); "
        "public class Program "
        "  public static method Main(): int "
        "    using var ps := new Array<Person>(); "
        "    ps.Add(new Person(\"ann\", 30)); "
        "    ps.Add(new Person(\"bob\", 40)); "
        "    for var g in ps.GroupBy(lambda(p) => p.name) do "
        "      var total := g.Aggregate(0, lambda(acc: int, x: Person) => acc + x.age); "
        "      Console.WriteLine(g.Key + \"=\" + total.ToString()); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors() == false);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(normalisedStdout(r) == "ann=30\nbob=40\n");
}

// ===========================================================================
// CONTROL 2 — THE ORDINARY SOURCE: the same `Aggregate` fold over a plain
// `Array<Person>` (not a grouping). §5's extension block is over
// `IEnumerable<T>` with T = the array's element, and that path was already
// correct — the fix must not disturb it.
//
// FAILS-WITHOUT: no — green on the base too.
// PASSES-WITH: "all=70".
// ===========================================================================
BEZEL_TEST(aggregate_igrouping_fold_element_type_9198,
           plain_array_aggregate_fold_control) {
    auto r = runAot(
        "module M; "
        "import System.Collections.Linq; "
        "record Person(name: string, age: int); "
        "public class Program "
        "  public static method Main(): int "
        "    using var ps := new Array<Person>(); "
        "    ps.Add(new Person(\"ann\", 30)); "
        "    ps.Add(new Person(\"bob\", 40)); "
        "    var total := ps.Aggregate(0, lambda(acc, x) => acc + x.age); "
        "    Console.WriteLine(\"all=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors() == false);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(normalisedStdout(r) == "all=70\n");
}
