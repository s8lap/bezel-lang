// Regression tests for issue #7671 — `Array<T>.LastIndexOf(item)` was refused
// as "no member `LastIndexOf` on `Array<T>`".
//
// Stdlib §4.4 (v0.350) declares on `Array<T>`:
//
//     public method LastIndexOf(item: T): int;   // last occurrence, or -1.
//                                                //   Equality is Equals for both, as IndexOf
//
// and §4's "Equality search" lists `Array<T>.IndexOf` and `LastIndexOf`
// together. `OwningArray<T>` (§4.5) declares `IndexOf` only, and no other
// collection declares either. The sema "collection member" allow-list and the
// `IndexOf` typing arm had no `LastIndexOf`, so a declared member was refused.
//
// The ORACLE is stdout: the scan direction (last, not first) is only visible
// in the printed index, so every value row has a duplicate element.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string prog(const std::string& decls, const std::string& body) {
    return "module M;\nimport System;\n" + decls +
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

}  // namespace

// The issue's literal shape: last occurrence, -1 when absent, and the first
// occurrence (`IndexOf`) differs from it.
BEZEL_TEST(array_lastindexof_7671, int_array_returns_the_last_occurrence_or_minus_one) {
    // per stdlib §4.4 (v0.350)
    expectStdout("",
                 "    var a: Array<int> := [1,2,3,2];\n"
                 "    Console.WriteLine(a.LastIndexOf(2));\n"
                 "    Console.WriteLine(a.LastIndexOf(9));\n"
                 "    Console.WriteLine(a.IndexOf(2));",
                 "3\n-1\n1\n");
}

BEZEL_TEST(array_lastindexof_7671, empty_array_and_single_element_boundaries) {
    // per stdlib §4.4 (v0.350): the scan runs Count-1 down to 0
    expectStdout("",
                 "    var e: Array<int> := [];\n"
                 "    var o: Array<int> := [5];\n"
                 "    Console.WriteLine(e.LastIndexOf(1));\n"
                 "    Console.WriteLine(o.LastIndexOf(5));\n"
                 "    Console.WriteLine(o.LastIndexOf(4));",
                 "-1\n0\n-1\n");
}

// The first element is a match too: the backward scan must reach index 0.
BEZEL_TEST(array_lastindexof_7671, match_only_at_index_zero) {
    // per stdlib §4.4 (v0.350)
    expectStdout("",
                 "    var a: Array<int> := [7,1,2,3];\n"
                 "    Console.WriteLine(a.LastIndexOf(7));",
                 "0\n");
}

// "Equality is Equals for both, as IndexOf": strings compare by value.
BEZEL_TEST(array_lastindexof_7671, string_elements_compare_by_equals) {
    // per stdlib §4.4 (v0.350) + §9.6
    expectStdout("",
                 "    var a: Array<string> := [\"a\",\"b\",\"a\"];\n"
                 "    var k := \"\" + \"a\";\n"
                 "    Console.WriteLine(a.LastIndexOf(k));\n"
                 "    Console.WriteLine(a.LastIndexOf(\"z\"));",
                 "2\n-1\n");
}

// A record element: structural equality, not identity.
BEZEL_TEST(array_lastindexof_7671, record_elements_compare_structurally) {
    // per stdlib §4.4 (v0.350) + spec §9.2
    expectStdout("record Pt(X: int, Y: int);\n",
                 "    var a: Array<Pt> := [new Pt(1, 2), new Pt(3, 4), new Pt(1, 2)];\n"
                 "    Console.WriteLine(a.LastIndexOf(new Pt(1, 2)));\n"
                 "    Console.WriteLine(a.IndexOf(new Pt(1, 2)));\n"
                 "    Console.WriteLine(a.LastIndexOf(new Pt(9, 9)));",
                 "2\n0\n-1\n");
}

// A nullable element type: `null` is searched as a value.
BEZEL_TEST(array_lastindexof_7671, nullable_elements_find_null_and_values) {
    // per stdlib §4.4 (v0.350): `Equals` for both, as IndexOf
    expectStdout("",
                 "    var a: Array<int?> := [1, null, 1, null];\n"
                 "    Console.WriteLine(a.LastIndexOf(null));\n"
                 "    Console.WriteLine(a.LastIndexOf(1));",
                 "3\n2\n");
}

// The `?.` spelling reaches the same member.
BEZEL_TEST(array_lastindexof_7671, null_conditional_spelling_and_null_receiver) {
    // per stdlib §4.4 (v0.350) + spec §7.5
    expectStdout("",
                 "    var a: Array<int>? := [1,2,1];\n"
                 "    var z: Array<int>? := null;\n"
                 "    Console.WriteLine(a?.LastIndexOf(1) ?? -9);\n"
                 "    Console.WriteLine(z?.LastIndexOf(1) ?? -9);",
                 "2\n-9\n");
}

// The result is an `int`: it composes in arithmetic and binds to an `int`.
BEZEL_TEST(array_lastindexof_7671, result_is_int) {
    // per stdlib §4.4 (v0.350)
    expectStdout("",
                 "    var a: Array<int> := [1,2,1];\n"
                 "    var i: int := a.LastIndexOf(a[0]) + a.Count;\n"
                 "    Console.WriteLine(i);",
                 "5\n");
}

// CONTROL: members that do NOT declare `LastIndexOf` still refuse it.
// `Stack<T>` (§4.11) has neither IndexOf nor LastIndexOf.
BEZEL_TEST(array_lastindexof_7671, control_stack_does_not_declare_it) {
    // per stdlib §4.11 (v0.350)
    auto r = runAot(prog("", "    var s := new Stack<int>();\n"
                             "    Console.WriteLine(s.LastIndexOf(1));"));
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("no member `LastIndexOf`") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// CONTROL: `OwningArray<T>` (§4.5) declares `IndexOf` and no `LastIndexOf`.
BEZEL_TEST(array_lastindexof_7671, control_owning_array_does_not_declare_it) {
    // per stdlib §4.5 (v0.350)
    auto r = runAot(prog("class Item end Item;\n",
                         "    var o := new OwningArray<Item>();\n"
                         "    Console.WriteLine(o.LastIndexOf(null));"));
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("no member `LastIndexOf`") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// CONTROL: `SortedDictionary<K, V>` (§4.17) declares no `LastIndexOf`. The
// shared non-owning allow-list must not admit the name for it: the call is a
// "no member" error, not a compile that emits nothing (#3079).
BEZEL_TEST(array_lastindexof_7671, control_sorted_dictionary_does_not_declare_it) {
    // per stdlib §4.17 (v0.350)
    auto r = runAot(prog("", "    var d := new SortedDictionary<int, int>();\n"
                             "    Console.WriteLine(d.LastIndexOf(1));"));
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("LastIndexOf") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// CONTROL: `FrozenArray<T>` (§4.19) declares `ToArray()`, `Length` and the
// indexer, and no `LastIndexOf`; the Array-only lowering must not take it.
BEZEL_TEST(array_lastindexof_7671, control_frozen_array_does_not_declare_it) {
    // per stdlib §4.19 (v0.350)
    auto r = runAot(prog("", "    var buf := new OwningBuffer<int>(2, lambda(i: int): int => i);\n"
                             "    using var fa := FrozenArray<int>.From(move buf);\n"
                             "    Console.WriteLine(fa.LastIndexOf(1));"));
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("LastIndexOf") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// Wrong arity: `Array<T>.LastIndexOf` takes exactly one parameter (stdlib
// §4.4, v0.350). Any other arity is a "no member" error, as it was before the
// name was admitted; it must not compile silently (#3079 degrade).
static void expectLastIndexOfArityRejected(const char* call) {
    auto r = runAot(prog("", std::string("    var a: Array<int> := [1, 2, 1];\n"
                                         "    Console.WriteLine(") + call + ");"));
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("LastIndexOf") != std::string::npos &&
            d.message.find("argument") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

BEZEL_TEST(array_lastindexof_7671, arity_zero_is_rejected) {
    expectLastIndexOfArityRejected("a.LastIndexOf()");
}
BEZEL_TEST(array_lastindexof_7671, arity_two_is_rejected) {
    expectLastIndexOfArityRejected("a.LastIndexOf(1, 2)");
}
BEZEL_TEST(array_lastindexof_7671, arity_three_is_rejected) {
    expectLastIndexOfArityRejected("a.LastIndexOf(1, 2, 3)");
}
