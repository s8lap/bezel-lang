// Sema — three LINQ shapes over an `IGrouping<TKey, T>` read the grouping's
// KEY type (`genericArgs[0]`) where stdlib §5 puts the ELEMENT type
// (`genericArgs[1]`). Issue #8542.
//
// THE DEFECT. `IGrouping<TKey, T> : IEnumerable<T>` (stdlib §5): a grouping's
// element is T, the SECOND generic argument; the FIRST is the key. Three
// sema sites read the KEY where the stdlib declares the element:
//
//   (a) `ExprTC::typeLinqOperator`'s `SelectMany` RESULT-ELEMENT computation
//       (`src/sema/ExprTypecheck_Calls.cpp`) — the flattened element of
//       `GroupBy(k).SelectMany(lambda(g) => g)`;
//   (b)/(c) `ExprTC::linqSourceElementType` (`src/sema/ExprTypecheck_Calls.cpp`)
//       — the element type a LINQ receiver contributes, from which
//       `ExprTC::linqLambdaElementType` seeds the selector's parameter.
//
// The LINQ RESULT type over a grouping was already right (`typeLinqOperator`'s
// `isGroupingLinqRecv` arm reads `genericArgs[1]`), and so was the `for`-in
// loop-variable inference (#6627) — which is why `g.ToArray()` yields
// `Array<T>` while `g.Where(lambda(x) => x.age > 35)` typed `x` as the KEY.
//
// SPEC. stdlib §5 (`public interface IGrouping<TKey, T>: IEnumerable<T>`),
// spec §27.4 (a `Where`/`Select`/`SelectMany` selector over an
// `IEnumerable<T>` takes `(T) -> …`, so over a grouping T is the grouping's
// ELEMENT). Spec revision v0.359.
//
// MEASURED ON THE BASE (`r30-c1` @ 6c7cbb002) — every row below is RED there:
// the compiler REJECTED each program with
//   "string member `.age` is not yet supported in codegen"
// (the selector's parameter was the key's `string`). (The ISSUE's own base
// measurement, at `r30-c1` tip `e41796df3` — before #6627 landed — reached
// codegen with a malformed IR instead: "Function return type does not match
// operand type of return inst!".) With the fix each program compiles, runs,
// and prints the expected values.
//
// WHY A 200-ITERATION LOOP. Each row runs its shape 200 times so the
// grouping MATERIALIZATION (stdlib §5's `GroupBy` semantics paragraph: the
// operator buffers the source, materializes every group, and each group owns
// its element buffer) is exercised 200 times under the suite's ASan/LSan leg
// (`BEZEL_AOT_FORCE_SANITIZE=address`) — a wrong element type would also be a
// wrong DISPOSAL (the element buffer's owner is the group, not the flattened
// sequence), and the accumulated values pin the enumeration.
//
// THE KEY IS NOT THE ELEMENT. The grouping key is the person's `dept` and the
// element is the person itself, so a row that read the KEY where the ELEMENT
// belongs could not even compile the body (`x.age` on a `string`), and a row
// that confused the key's VALUE with the element's would print a different
// string — the expectations below distinguish the two.

#include <string>

#include "support/aot_support.hpp"  // runAot — AOT oracle (the strict one)
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// (a) `SelectMany` over a grouping — the FLATTENED element must be the
// grouping's ELEMENT (the person), not its KEY (the department).
//
// `ps.GroupBy(lambda(q) => q.dept)` yields `IEnumerable<IGrouping<string,
// Person>>`; `.SelectMany(lambda(g) => g)` flattens each group, so the
// flattened sequence's element is `Person` (stdlib §5: the group's element is
// T = genericArgs[1]).
//
// Per iteration: the "eng" group holds ann(40) and ann2(50), the "ops" group
// holds bob(20) — 3 flattened elements summing to 110. Over 200 iterations:
// 600 elements, 22000 of age.
const char* kSelectManyOverGrouping =
    "module M; "
    "import System.Collections.Linq; "
    "public record Person(name: string, dept: string, age: int); "
    "class C "
    "  public static method Main(): int "
    "    var sum := 0; "
    "    var n := 0; "
    "    var i := 0; "
    "    while i < 200 do "
    "      using var ps := new Array<Person>(); "
    "      ps.Add(new Person(\"ann\", \"eng\", 40)); "
    "      ps.Add(new Person(\"bob\", \"ops\", 20)); "
    "      ps.Add(new Person(\"ann2\", \"eng\", 50)); "
    "      for var p in ps.GroupBy(lambda(q) => q.dept).SelectMany(lambda(g) => g) do "
    "        sum := sum + p.age; "
    "        n := n + 1; "
    "      end; "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(\"sum=\" + sum.ToString() + \" n=\" + n.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// (b) A `Where` selector OVER a grouping — the selector's parameter must be
// the grouping's ELEMENT.
//
// The key selector is FULLY TYPED (`lambda(q: Person) => q.dept`): #6627
// already taught the `for`-in path and the GroupBy key selector to read the
// element, so a defect that survived a TYPED selector is a defect of its own
// and not a consequence of an un-annotated selector.
//
// Per iteration: the "eng" group holds ann(40) and ann2(50) — both over 35;
// the "ops" group holds bob(20) — not. 2 hits, ages summing to 90. Over 200
// iterations: 400 hits, 18000 of age.
const char* kWhereOverGrouping =
    "module M; "
    "import System.Collections.Linq; "
    "public record Person(name: string, dept: string, age: int); "
    "class C "
    "  public static method Main(): int "
    "    var hits := 0; "
    "    var sum := 0; "
    "    var i := 0; "
    "    while i < 200 do "
    "      using var ps := new Array<Person>(); "
    "      ps.Add(new Person(\"ann\", \"eng\", 40)); "
    "      ps.Add(new Person(\"bob\", \"ops\", 20)); "
    "      ps.Add(new Person(\"ann2\", \"eng\", 50)); "
    "      for var g in ps.GroupBy(lambda(q: Person) => q.dept) do "
    "        for var q in g.Where(lambda(x) => x.age > 35) do "
    "          hits := hits + 1; "
    "          sum := sum + q.age; "
    "        end; "
    "      end; "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(\"hits=\" + hits.ToString() + \" sum=\" + sum.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// (c) A `Select` selector OVER a grouping — the selector's parameter must be
// the grouping's ELEMENT, and the RESULT carries the selector's return type.
//
// `last` is reset each iteration, so the printed value is the LAST
// iteration's names — the groups appear in first-occurrence-of-key order
// (stdlib §5's `GroupBy` semantics paragraph), so "eng" then "ops": ann,
// ann2, bob.
const char* kSelectOverGrouping =
    "module M; "
    "import System.Collections.Linq; "
    "public record Person(name: string, dept: string, age: int); "
    "class C "
    "  public static method Main(): int "
    "    var n := 0; "
    "    var last := \"\"; "
    "    var i := 0; "
    "    while i < 200 do "
    "      using var ps := new Array<Person>(); "
    "      ps.Add(new Person(\"ann\", \"eng\", 40)); "
    "      ps.Add(new Person(\"bob\", \"ops\", 20)); "
    "      ps.Add(new Person(\"ann2\", \"eng\", 50)); "
    "      last := \"\"; "
    "      for var g in ps.GroupBy(lambda(q: Person) => q.dept) do "
    "        for var q in g.Select(lambda(x) => x.name) do "
    "          n := n + 1; "
    "          last := last + q + \";\"; "
    "        end; "
    "      end; "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(\"n=\" + n.ToString() + \" last=\" + last); "
    "    return 0; "
    "  end Main; "
    "end C;";

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

// ---------------------------------------------------------------------------
// (a) `SelectMany` over a grouping flattens to the grouping's ELEMENT type
// (stdlib §5), not its KEY type.
// ---------------------------------------------------------------------------

BEZEL_TEST(igrouping_linq_element_type_8542,
           selectmany_over_grouping_flattens_the_element_type_spec_5) {
    JitResult r = runAot(kSelectManyOverGrouping);
    BEZEL_EXPECT_TRUE(r.ran);  // RED on the base: the compile was rejected
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(normalisedStdout(r), std::string("sum=22000 n=600\n"));
}

// ---------------------------------------------------------------------------
// (b) A `Where` selector over a grouping takes the grouping's ELEMENT as its
// parameter type (spec §27.4 / stdlib §5).
// ---------------------------------------------------------------------------

BEZEL_TEST(igrouping_linq_element_type_8542,
           where_over_grouping_seeds_the_element_type_spec_27_4) {
    JitResult r = runAot(kWhereOverGrouping);
    BEZEL_EXPECT_TRUE(r.ran);  // RED on the base: the compile was rejected
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(normalisedStdout(r), std::string("hits=400 sum=18000\n"));
}

// ---------------------------------------------------------------------------
// (c) A `Select` selector over a grouping takes the grouping's ELEMENT as its
// parameter type (spec §27.4 / stdlib §5).
// ---------------------------------------------------------------------------

BEZEL_TEST(igrouping_linq_element_type_8542,
           select_over_grouping_seeds_the_element_type_spec_27_4) {
    JitResult r = runAot(kSelectOverGrouping);
    BEZEL_EXPECT_TRUE(r.ran);  // RED on the base: the compile was rejected
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(normalisedStdout(r),
                    std::string("n=600 last=ann;ann2;bob;\n"));
}
