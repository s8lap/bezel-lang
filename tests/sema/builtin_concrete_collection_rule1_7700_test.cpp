// Spec-derived regression test for issue #7700.
//
// Spec §26.5 rule 1 (v0.350): "If `x`'s declared type (or any base type)
// *declares* any instance member named `Foo` — of any arity, applicability,
// or accessibility — instance resolution is committed and extension blocks
// are **never consulted** for that name on that receiver. If the committed
// instance lookup then fails ..., the call is a compile error citing the
// instance member(s); it does not fall back to an extension."
//
// Stdlib §4.4 (v0.350) declares `Array<T>.Contains(item: T): bool` and
// `ToArray(): owned Array<T>`; §4.10 `Set<T>` conforms to `ICollection<T>`
// (`Contains`); §4.8 `Dictionary<K, V>.ContainsKey(key: K): bool`; §4.11
// `Stack<T>.Peek(): T`; §4.18 `SortedSet<T>` declares `Contains`.
//
// THE DEFECT. Sema consulted the user's extension blocks for a name the
// CONCRETE built-in collection head DECLARES (the #7446 gate covered only
// the eight interface heads), and typed the call by the extension's own
// signature while codegen dispatched the built-in:
//
//   extension Array<int> Contains(x: int): string => "EXT"
//   var s: string := a.Contains(2);   // COMPILED, and printed True
//
// The ORACLE is the specific diagnostic on the rejection rows and the exact
// stdout on the value rows. The CONTROL rows are load-bearing: a name the
// head does NOT declare (a stdlib §5-only operator, or an invented name) must
// still bind the user's extension (§26.5 rule 2).
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& ext, const std::string& body) {
    return "module M;\n" + ext +
           "class C\n"
           "  public static method Main(): int\n"
           "    var a: Array<int> := [1,2,3];\n"
           "    var st: Set<int> := [1,2,3];\n"
           "    var ss: SortedSet<int> := new SortedSet<int>();\n"
           "    var stk: Stack<int> := [1,2,3];\n"
           "    var d: Dictionary<string, int> := new Dictionary<string, int>();\n"
           "    d.Add(\"k\", 5);\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

void expectRejected(const std::string& ext, const std::string& body,
                    const std::string& needle) {
    auto r = runAot(prog(ext, body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

void expectStdout(const std::string& ext, const std::string& body,
                  const std::string& out) {
    auto r = runAot(prog(ext, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// §5.1's refusal of a built-in-typed initializer for a `string` local.
const char kNotConvertible[] = "not implicitly convertible to the target type";

}  // namespace

// ── the DEFECT: the extension typed a member the head DECLARES ────────────

// The issue's repro (probe s03): `Array<T>.Contains(item: T): bool` is
// declared, so rule 1 commits and the call is the built-in's `bool`.
BEZEL_TEST(sema_builtin_concrete_collection_rule1_7700,
           array_contains_string_extension_rejected) {
    // per §26.5 rule 1 (v0.350) + stdlib §4.4 + §5.1
    expectRejected("extension Array<int>\n"
                   "  public method Contains(x: int): string => \"EXT\";\n"
                   "end;\n",
                   "var s: string := a.Contains(2);\n"
                   "    Console.WriteLine(s);",
                   kNotConvertible);
}

// The issue's second repro (probe t03): `ToArray(): owned Array<T>` is
// declared, so `a.ToArray()` is an array, not the extension's `int`.
BEZEL_TEST(sema_builtin_concrete_collection_rule1_7700,
           array_toarray_int_extension_types_the_builtin) {
    // per §26.5 rule 1 (v0.350) + stdlib §4.4
    expectStdout("extension Array<int>\n"
                 "  public method ToArray(): int => 77;\n"
                 "end;\n",
                 "var x := a.ToArray();\n"
                 "    Console.WriteLine(x.Count);",
                 "3\n");
}

// "Of any arity": the extension's `IndexOf` takes two arguments, the
// declared `IndexOf(item, startIndex := 0)` one. Rule 1 commits by NAME, so
// the extension is never consulted and the declared member answers.
BEZEL_TEST(sema_builtin_concrete_collection_rule1_7700,
           array_indexof_other_arity_extension_unreachable) {
    // per §26.5 rule 1 (v0.350) + stdlib §4.4
    expectStdout("extension Array<int>\n"
                 "  public method IndexOf(x: int, y: int): string => \"EXT\";\n"
                 "end;\n",
                 "var i: int := a.IndexOf(2);\n"
                 "    Console.WriteLine(i);",
                 "1\n");
}

// "Then fails": a call the declared member cannot take is an error, never a
// fallback to the extension of that arity.
BEZEL_TEST(sema_builtin_concrete_collection_rule1_7700,
           array_contains_other_arity_does_not_fall_back) {
    // per §26.5 rule 1 (v0.350) + stdlib §4.4
    auto r = runAot(prog("extension Array<int>\n"
                         "  public method Contains(x: int, y: int): string "
                         "=> \"EXT\";\n"
                         "end;\n",
                         "var s: string := a.Contains(2, 3);\n"
                         "    Console.WriteLine(s);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// §4.10: `Set<T>` conforms to `ICollection<T>`, which declares `Contains`.
BEZEL_TEST(sema_builtin_concrete_collection_rule1_7700,
           set_contains_string_extension_rejected) {
    // per §26.5 rule 1 (v0.350) + stdlib §4.1, §4.10 + §5.1
    expectRejected("extension Set<int>\n"
                   "  public method Contains(x: int): string => \"EXT\";\n"
                   "end;\n",
                   "var s: string := st.Contains(2);\n"
                   "    Console.WriteLine(s);",
                   kNotConvertible);
}

// §4.8: `Dictionary<K, V>.ContainsKey(key: K): bool`.
BEZEL_TEST(sema_builtin_concrete_collection_rule1_7700,
           dictionary_containskey_string_extension_rejected) {
    // per §26.5 rule 1 (v0.350) + stdlib §4.8 + §5.1
    expectRejected("extension Dictionary<string, int>\n"
                   "  public method ContainsKey(k: string): string => "
                   "\"EXT\";\n"
                   "end;\n",
                   "var s: string := d.ContainsKey(\"k\");\n"
                   "    Console.WriteLine(s);",
                   kNotConvertible);
}

// §4.11: `Stack<T>.Peek(): T`.
BEZEL_TEST(sema_builtin_concrete_collection_rule1_7700,
           stack_peek_string_extension_rejected) {
    // per §26.5 rule 1 (v0.350) + stdlib §4.11 + §5.1
    expectRejected("extension Stack<int>\n"
                   "  public method Peek(): string => \"EXT\";\n"
                   "end;\n",
                   "var s: string := stk.Peek();\n"
                   "    Console.WriteLine(s);",
                   kNotConvertible);
}

// §4.18: `SortedSet<T>` declares `Contains` (a generic block on the head).
BEZEL_TEST(sema_builtin_concrete_collection_rule1_7700,
           sortedset_contains_generic_extension_rejected) {
    // per §26.5 rule 1 (v0.350) + stdlib §4.18 + §5.1
    expectRejected("extension SortedSet<T>\n"
                   "  public method Contains(x: T): string => \"EXT\";\n"
                   "end;\n",
                   "var s: string := ss.Contains(2);\n"
                   "    Console.WriteLine(s);",
                   kNotConvertible);
}

// The value-typed spelling: the declared `Contains` answers, the
// extension's `Contains` never runs.
BEZEL_TEST(sema_builtin_concrete_collection_rule1_7700,
           array_contains_bool_extension_prints_the_builtin) {
    // per §26.5 rule 1 (v0.350) + stdlib §4.4
    expectStdout("extension Array<int>\n"
                 "  public method Contains(x: int): bool => false;\n"
                 "end;\n",
                 "Console.WriteLine(a.Contains(2));",
                 "True\n");
}

// ── CONTROLS: a name the head does NOT declare still binds the extension ──

// Stdlib §5 `Sum` is declared by `extension IEnumerable<T>`, not by
// `Array<T>`; §26.5 rule 2 admits the user's more specific block (#7448).
BEZEL_TEST(sema_builtin_concrete_collection_rule1_7700,
           control_array_sum_user_extension_wins) {
    // per §26.5 rules 2-3 + stdlib §4.4, §5
    expectStdout("extension Array<int>\n"
                 "  public method Sum(): string => \"EXT\";\n"
                 "end;\n",
                 "var s: string := a.Sum();\n"
                 "    Console.WriteLine(s);",
                 "EXT\n");
}

// An invented name on another head.
BEZEL_TEST(sema_builtin_concrete_collection_rule1_7700,
           control_set_invented_name_user_extension_wins) {
    // per §26.5 rule 2 + stdlib §4.10
    expectStdout("extension Set<int>\n"
                 "  public method Describe(): string => \"EXT\";\n"
                 "end;\n",
                 "var s: string := st.Describe();\n"
                 "    Console.WriteLine(s);",
                 "EXT\n");
}

// `Peek` is declared by `Stack<T>` (§4.11) but NOT by `Set<T>`: the same
// name on a head that does not declare it still binds the extension.
BEZEL_TEST(sema_builtin_concrete_collection_rule1_7700,
           control_set_peek_is_not_declared_user_extension_wins) {
    // per §26.5 rule 2 + stdlib §4.10 (no `Peek`), §4.11
    expectStdout("extension Set<int>\n"
                 "  public method Peek(): string => \"EXT\";\n"
                 "end;\n",
                 "var s: string := st.Peek();\n"
                 "    Console.WriteLine(s);",
                 "EXT\n");
}
