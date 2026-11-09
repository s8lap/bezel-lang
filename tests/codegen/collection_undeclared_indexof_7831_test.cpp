// Regression tests for issue #7831 — `SortedDictionary<K, V>.IndexOf` and
// `FrozenArray<T>.IndexOf` COMPILED although no stdlib section declares them.
//
// THE ISSUE. Stdlib §1's Block-by-name rule (v0.350) makes an undeclared
// member an ERROR: "an instance member blocks a same-named extension" — and a
// member a type does NOT declare is not a member of that type at all.
//
//   * stdlib §4.17 (v0.355) declares on `SortedDictionary<K, V>`: `Count`,
//     the indexer `[k]`, `Add`, `TryAdd`, `ContainsKey`, `Contains`,
//     `TryGetValue`, `GetOrNull`, `Remove`, `Clear`, `FirstKey`, `LastKey`.
//     It declares NO `IndexOf` — and `IndexOf` is meaningless for a
//     KEY-VALUE head (the stdlib's own §4.17 surface is the authority).
//   * stdlib §4.19 (v0.355) declares on `FrozenArray<T>`: the static `From`
//     (two forms), `Length`, the indexer `[i]`, `ToArray` (and the
//     destructor). It declares NO `IndexOf`.
//   * stdlib §5's `extension IEnumerable<T>` block declares NO `IndexOf`
//     either (its `Contains` is a different member), so no extension can
//     supply the name.
//
// WHAT THE BASE DID (measured on `a939f913`):
//   * `new SortedDictionary<int,int>().IndexOf(1)` COMPILED — sema's shared
//     non-owning allow-list admits `IndexOf` for every non-owning container
//     head, and codegen's dictionary arms have no `IndexOf`, so the call
//     degraded to the #3079 "the argument of this `Console.WriteLine` failed
//     to lower to a value" warning and the program emitted NOTHING.
//   * `FrozenArray.From(a).IndexOf(1)` COMPILED SILENTLY — `FrozenArray` is
//     in NEITHER sema container table (so no call-surface gate applies to
//     it), and codegen's array `IndexOf` arm had no head gate, so the call
//     took the `Array<T>` lowering.
//
// THE FIX. Codegen's collection-head member surface refuses the two
// POSITIONAL-INDEX names on a concrete collection head whose stdlib section
// declares neither, with the stdlib's own documented-surface diagnostic —
// the same sentence the `IEnumerable` row uses (with the concrete
// collection's own phrasing, "this collection's documented surface"; the
// §4.12 `IEnumerable` row says "this interface's" because that head IS an
// interface).
//
// The ORACLE is the compile REJECTION and its diagnostic text: the issue's
// symptom is a program that COMPILES, so a row that only checked the
// program's output would pass on the base too. Every control row reads the
// value the member returns, so a fix that broke a DECLARED member could not
// pass.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

std::string prog(const std::string& decls, const std::string& body) {
    return "module M;\nimport System;\nimport System.Linq;\n" + decls +
           "class P\n"
           "  public static method Main(): int\n" + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end P;\n";
}

void expectStdout(const std::string& decls, const std::string& body,
                  const std::string& out) {
    auto r = runAot(prog(decls, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// A reject row: the program must NOT compile, and the diagnostic must name
// the member, the head and the stdlib section that declares the surface.
void expectRejected(const std::string& decls, const std::string& body,
                    const char* needle) {
    auto r = runAot(prog(decls, body));
    BEZEL_EXPECT_FALSE(r.ran);
    // fix #3895 (issue #3892): a genuine compile-time rejection, not a crash
    // or a signal — `!ran` alone cannot tell them apart.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

}  // namespace

// -----------------------------------------------------------------------
// The issue's two reproducing programs — REJECTED
// -----------------------------------------------------------------------

// stdlib §4.17 (v0.355): `SortedDictionary<K, V>` declares no `IndexOf`.
// The issue's literal repro. Pre-fix this COMPILED (with only the #3079
// "failed to lower to a value" warning) and the program emitted nothing.
BEZEL_TEST(collection_undeclared_indexof_7831,
           sorted_dictionary_indexof_is_rejected) {
    // per stdlib §4.17 (v0.355) and stdlib §1's Block-by-name rule
    expectRejected("",
                   "    var s := new SortedDictionary<int,int>();\n"
                   "    Console.WriteLine(s.IndexOf(1));",
                   "no member `IndexOf` on `SortedDictionary`");
}

// The stdlib section that DECLARES `SortedDictionary`'s surface must be
// named, and the diagnostic must be the stdlib's own documented-surface one
// — not an "internal: ..." warning and not a generic "no dispatch arm"
// message.
BEZEL_TEST(collection_undeclared_indexof_7831,
           sorted_dictionary_indexof_names_the_stdlib_section_and_surface) {
    // per stdlib §4.17 (v0.355)
    auto r = runAot(prog("", "    var s := new SortedDictionary<int,int>();\n"
                             "    Console.WriteLine(s.IndexOf(1));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "(stdlib \xC2\xA7""4.17)"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not part of this collection's documented surface"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "language spec \xC2\xA7""8.1"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "stdlib \xC2\xA7""5 LINQ extension operator"));
}

// stdlib §4.19 (v0.355): `FrozenArray<T>` declares `Length`, the indexer,
// `ToArray` and the static `From` — no `IndexOf`. The issue's second
// literal repro (the `using var` form, which compiled with NO diagnostic at
// all).
BEZEL_TEST(collection_undeclared_indexof_7831,
           frozen_array_indexof_is_rejected) {
    // per stdlib §4.19 (v0.355)
    expectRejected("",
                   "    var a: Array<int> := [1,2,1];\n"
                   "    using var f := FrozenArray.From(a);\n"
                   "    Console.WriteLine(f.IndexOf(1));",
                   "no member `IndexOf` on `FrozenArray`");
}

// The same, with the §4.19 citation and the documented-surface sentence.
BEZEL_TEST(collection_undeclared_indexof_7831,
           frozen_array_indexof_names_the_stdlib_section_and_surface) {
    // per stdlib §4.19 (v0.355)
    auto r = runAot(prog("", "    var a: Array<int> := [1,2,1];\n"
                             "    using var f := FrozenArray.From(a);\n"
                             "    Console.WriteLine(f.IndexOf(1));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "(stdlib \xC2\xA7""4.19)"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not part of this collection's documented surface"));
}

// The `FrozenArray<T>` form the #7671 row uses (the O(1) freeze over an
// `OwningBuffer<T>`) must be refused the same way — the head is the same.
BEZEL_TEST(collection_undeclared_indexof_7831,
           frozen_array_from_buffer_indexof_is_rejected) {
    // per stdlib §4.19 (v0.355)
    expectRejected("",
                   "    var buf := new OwningBuffer<int>(2, lambda(i: int): int => i);\n"
                   "    using var f := FrozenArray<int>.From(move buf);\n"
                   "    Console.WriteLine(f.IndexOf(1));",
                   "no member `IndexOf` on `FrozenArray`");
}

// stdlib §4.8 (v0.355): `Dictionary<K, V>` declares no `IndexOf` either.
// It was already REJECTED on the base, but with the generic #2457 "no
// dispatch arm" message; it now gets the stdlib's documented-surface
// diagnostic, which names the member, the head and §4.8. This row pins the
// MESSAGE, so the diagnostic improvement cannot silently regress.
BEZEL_TEST(collection_undeclared_indexof_7831,
           dictionary_indexof_is_rejected_with_the_documented_surface_diagnostic) {
    // per stdlib §4.8 (v0.355)
    expectRejected("",
                   "    var d := new Dictionary<int,int>();\n"
                   "    Console.WriteLine(d.IndexOf(1));",
                   "no member `IndexOf` on `Dictionary`");
}

// stdlib §4.4 (v0.355): `LastIndexOf` is declared by `Array<T>` alone, so
// `FrozenArray<T>` must refuse it too — with the same documented-surface
// diagnostic (the #7671 row pinned only the member name).
BEZEL_TEST(collection_undeclared_indexof_7831,
           frozen_array_lastindexof_is_rejected) {
    // per stdlib §4.4 / §4.19 (v0.355)
    expectRejected("",
                   "    var a: Array<int> := [1,2,1];\n"
                   "    using var f := FrozenArray.From(a);\n"
                   "    Console.WriteLine(f.LastIndexOf(1));",
                   "no member `LastIndexOf` on `FrozenArray`");
}

// -----------------------------------------------------------------------
// CONTROLS — the members that ARE declared keep working
// -----------------------------------------------------------------------

// stdlib §4.4 (v0.355): `Array<T>` declares `IndexOf(item: T, startIndex:
// int := 0): int` and `LastIndexOf(item: T): int`. Both must keep working,
// and they must keep returning the FIRST and the LAST occurrence.
BEZEL_TEST(collection_undeclared_indexof_7831,
           array_indexof_and_lastindexof_still_work) {
    // per stdlib §4.4 (v0.355)
    expectStdout("",
                 "    var a: Array<int> := [1,2,1];\n"
                 "    Console.WriteLine(a.IndexOf(1));\n"
                 "    Console.WriteLine(a.IndexOf(1, 1));\n"
                 "    Console.WriteLine(a.LastIndexOf(1));",
                 "0\n2\n2\n");
}

// stdlib §4.5 (v0.355): `OwningArray<T>` declares `IndexOf(item: T): int`
// (and NO `LastIndexOf`). The declared member must keep working.
BEZEL_TEST(collection_undeclared_indexof_7831,
           owning_array_indexof_still_works) {
    // per stdlib §4.5 (v0.355)
    expectStdout("class Item end Item;\n",
                 "    using var o := new OwningArray<Item>();\n"
                 "    o.Add(new Item());\n"
                 "    Console.WriteLine(o.IndexOf(new Item()));",
                 "-1\n");
}

// stdlib §4.19 (v0.355): `FrozenArray<T>`'s DECLARED surface — `Length`,
// the indexer, `ToArray` — must keep working.
BEZEL_TEST(collection_undeclared_indexof_7831,
           frozen_array_declared_surface_still_works) {
    // per stdlib §4.19 (v0.355)
    expectStdout("",
                 "    var a: Array<int> := [1,2,1];\n"
                 "    using var f := FrozenArray.From(a);\n"
                 "    Console.WriteLine(f.Length);\n"
                 "    Console.WriteLine(f[1]);\n"
                 "    using var b := f.ToArray();\n"
                 "    Console.WriteLine(b.Count);",
                 "3\n2\n3\n");
}

// stdlib §4.17 (v0.355): `SortedDictionary<K, V>`'s DECLARED surface must
// keep working — `Count`, `ContainsKey`, `Contains`, the indexer,
// `TryGetValue`, `GetOrNull`, `Remove`, `FirstKey`, `LastKey`.
BEZEL_TEST(collection_undeclared_indexof_7831,
           sorted_dictionary_declared_surface_still_works) {
    // per stdlib §4.17 (v0.355)
    expectStdout("",
                 "    var d := new SortedDictionary<int,int>();\n"
                 "    d.Add(1, 10);\n"
                 "    Console.WriteLine(d.Count);\n"
                 "    Console.WriteLine(d.ContainsKey(1));\n"
                 "    Console.WriteLine(d.Contains(1));\n"
                 "    Console.WriteLine(d[1]);\n"
                 "    Console.WriteLine(d.FirstKey);\n"
                 "    Console.WriteLine(d.LastKey);\n"
                 "    Console.WriteLine(d.Remove(1));\n"
                 "    Console.WriteLine(d.Count);",
                 "1\nTrue\nTrue\n10\n1\n1\nTrue\n0\n");
}

// stdlib §4.17 (v0.355): `TryGetValue` / `GetOrNull` — the two nullable
// companions of the keyed read — must keep working.
BEZEL_TEST(collection_undeclared_indexof_7831,
           sorted_dictionary_try_get_value_still_works) {
    // per stdlib §4.17 (v0.355)
    expectStdout("",
                 "    var d := new SortedDictionary<int,int>();\n"
                 "    d.Add(1, 10);\n"
                 "    var v: int? := null;\n"
                 "    Console.WriteLine(d.TryGetValue(1, out v));\n"
                 "    Console.WriteLine(d.GetOrNull(1) ?? -1);\n"
                 "    Console.WriteLine(d.GetOrNull(2) ?? -1);",
                 "True\n10\n-1\n");
}

// stdlib §4.8 (v0.355): `Dictionary<K, V>`'s declared keyed surface must
// keep working.
BEZEL_TEST(collection_undeclared_indexof_7831,
           dictionary_declared_surface_still_works) {
    // per stdlib §4.8 (v0.355)
    expectStdout("",
                 "    var d := new Dictionary<int,int>();\n"
                 "    d.Add(1, 10);\n"
                 "    Console.WriteLine(d.Count);\n"
                 "    Console.WriteLine(d.ContainsKey(1));\n"
                 "    Console.WriteLine(d.Contains(1));\n"
                 "    var v: int? := null;\n"
                 "    Console.WriteLine(d.TryGetValue(1, out v));",
                 "1\nTrue\nTrue\nTrue\n");
}

// stdlib §5 (v0.355): a §5 operator on a head that declares no `IndexOf` is
// still reachable — §1's Block-by-name rule blocks an extension only by the
// NAME the head declares. `FrozenArray<T>` declares `Length`/`ToArray`, not
// `Count`, so §5's `Count()` / `Contains(value)` / `ToArray()` are reachable
// on it.
BEZEL_TEST(collection_undeclared_indexof_7831,
           frozen_array_linq_operators_still_work) {
    // per stdlib §5 (v0.355) and stdlib §1's Block-by-name rule
    expectStdout("",
                 "    var a: Array<int> := [1,2,1];\n"
                 "    using var f := FrozenArray.From(a);\n"
                 "    Console.WriteLine(f.Count());\n"
                 "    Console.WriteLine(f.Contains(1));",
                 "3\nTrue\n");
}
