// Spec-derived regression test for issue #7446.
//
// Spec §26.5 rule 1 (v0.345, docs/Bezel_Language_Spec.md:5714): "If `x`'s
// declared type (or any base type) *declares* any instance member named
// `Foo` — of any arity, applicability, or accessibility — instance
// resolution is committed and extension blocks are **never consulted** for
// that name on that receiver."
//
// Spec §26.5 rule 2 (v0.345, docs/Bezel_Language_Spec.md:5715): "If the
// receiver's type declares no instance member of that name, the compiler
// searches the current module and all imported modules ... for extension
// blocks on `x`'s type, base types, or implemented interfaces."
//
// Stdlib §4.1 (v0.345) declares `pure readonly property Count: int` on
// `IReadOnlyCollection<out T>`, and `IList<T>` INHERITS it through
// `IReadOnlyList<T>`. The stdlib's own "Block-by-name" convention says the
// same: "extension resolution commits to instance members by name, inherited
// ones included".
//
// THE DEFECT — a TYPE CONFUSION. Sema consulted the user's extension for
// the INHERITED name `Count` and TYPED the call by the extension's own
// signature, while codegen dispatched the built-in. The program therefore
// stored the built-in's `int` in a `string`-typed local:
//
//   extension IList<int> public method Count(): string => "USER"; end;
//   var c: IList<int> := a;
//   var s: string := c.Count();   // COMPILED CLEAN, and printed 3
//   var i: int := c.Count();      // REFUSED: "... not implicitly
//                                 //  convertible to the target type (§5.1)"
//
// THE TWO SPELLINGS DISAGREED, and the disagreement was the soundness
// defect. After the fix they agree: the call is the built-in's `int`, the
// `int` spelling is admitted, and the `string` spelling is refused by §5.1.
//
// THE ROWS. The value rows assert the program's exact STDOUT (the oracle is
// the RENDERED VALUE). The rejection row asserts the SPECIFIC diagnostic at
// a real source location. The CONTROL rows are load-bearing: where §26.5
// rule 1 does NOT commit — a name the receiver's surface does not declare,
// at all — the user's extension must still WIN.
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// A module carrying the extension block, and a `Main` that runs `body` over
// a three-element `Array<int>` bound to an `IList<int>`.
std::string prog(const std::string& ext, const std::string& body) {
    return "module M;\n" + ext +
           "class C\n"
           "  public static method Main(): int\n"
           "    var a: Array<int> := [1,2,3];\n"
           "    var c: IList<int> := a;\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The same program with a NULLABLE receiver, for the `?.` spelling.
std::string progNullable(const std::string& ext, const std::string& body) {
    return "module M;\n" + ext +
           "class C\n"
           "  public static method Main(): int\n"
           "    var a: Array<int> := [1,2,3];\n"
           "    var c: IList<int>? := a;\n"
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

// A rejection row: the program must be REJECTED at compile time, with the
// named diagnostic, at a real source location.
void expectRejected(const std::string& ext, const std::string& body,
                     const std::string& needle) {
    auto r = runAot(prog(ext, body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// A value row: the program must compile, run, and print exactly `out`.
void expectStdout(const std::string& ext, const std::string& body,
                  const std::string& out) {
    auto r = runAot(prog(ext, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// A value row over the NULLABLE receiver (the `?.` spelling).
void expectStdoutNullable(const std::string& ext, const std::string& body,
                          const std::string& out) {
    auto r = runAot(progNullable(ext, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// §5.1's refusal of an int-typed initializer for a `string`-typed local.
const char kNotConvertible[] =
    "the initializer for `s` has a type that is not implicitly convertible "
    "to the target type (§5.1)";

}  // namespace

// ── the DEFECT: the extension typed a member the receiver INHERITS ────────

// The `string` spelling. Before the fix it COMPILED — the built-in's `int`
// was stored in a `string`-typed local — which is the soundness defect.
BEZEL_TEST(sema_collection_interface_inherited_member_7446,
           ilist_count_string_target_rejected) {
    // per §26.5 rule 1 (v0.345) + stdlib §4.1 + §5.1
    expectRejected("extension IList<int>\n"
                   "  public method Count(): string => \"USER\";\n"
                   "end;\n",
                   "var s: string := c.Count();\n"
                   "    Console.WriteLine(s);",
                   kNotConvertible);
}

// The `int` spelling, with an extension whose OWN return type is `bool`.
// Before the fix the extension typed the call `bool`, so the `int` spelling
// was REFUSED by §5.1 while the `string` spelling was admitted — the two
// spellings disagreed. After the fix the call is the built-in's `int`.
BEZEL_TEST(sema_collection_interface_inherited_member_7446,
           ilist_count_bool_extension_int_target_prints_the_builtin_count) {
    // per §26.5 rule 1 (v0.345) + stdlib §4.1 + §4.4
    expectStdout("extension IList<int>\n"
                 "  public method Count(): bool => true;\n"
                 "end;\n",
                 "var i: int := c.Count();\n"
                 "    Console.WriteLine(i);",
                 "3\n");
}

// ── CONTROLS ──────────────────────────────────────────────────────────────

// §26.5 rule 1 (v0.345) + stdlib §4.1: `IList<T>` INHERITS `Count`, so the
// extension is UNREACHABLE and the built-in answers with the element count
// — not the extension's 77. This is the value the L1 lane's #6159 fix
// already pinned at the CODEGEN end; it is pinned here at the SEMA end too.
BEZEL_TEST(sema_collection_interface_inherited_member_7446,
           ilist_count_extension_is_unreachable) {
    // per §26.5 rule 1 (v0.345) + stdlib §4.1
    expectStdout("extension IList<int>\n"
                 "  public method Count(): int => 77;\n"
                 "end;\n",
                 "var i: int := c.Count();\n"
                 "    Console.WriteLine(i);",
                 "3\n");
}

// §26.5 rule 2 (v0.345) + stdlib §4.1: `IList<T>` declares NO `Length`, so
// rule 1 does NOT commit, rule 2 admits the extension blocks, and the user's
// extension is the selected candidate. This is the mirror of the rows above
// and the fix must not change it.
BEZEL_TEST(sema_collection_interface_inherited_member_7446,
           ilist_length_user_extension_wins) {
    // per §26.5 rule 2 (v0.345) + stdlib §4.1
    expectStdout("extension IList<int>\n"
                 "  public method Length(): int => 77;\n"
                 "end;\n",
                 "var i: int := c.Length();\n"
                 "    Console.WriteLine(i);",
                 "77\n");
}

// The same, with an INTERFACE extension block: `IEnumerable<T>` is an
// implemented interface of `IList<T>`, which rule 2 admits.
BEZEL_TEST(sema_collection_interface_inherited_member_7446,
           ilist_length_ienumerable_extension_wins) {
    // per §26.5 rule 2 (v0.345) + stdlib §4.1 / §4.12
    expectStdout("extension IEnumerable<int>\n"
                 "  public method Length(): int => 77;\n"
                 "end;\n",
                 "var i: int := c.Length();\n"
                 "    Console.WriteLine(i);",
                 "77\n");
}

// With NO extension at all the built-in answers, exactly as before the fix.
BEZEL_TEST(sema_collection_interface_inherited_member_7446,
           no_extension_builtin_still_answers) {
    // per stdlib §4.1 / §4.4
    expectStdout("",
                 "var i: int := c.Count();\n"
                 "    Console.WriteLine(i);",
                 "3\n");
}

// ── the `?.` twin ──────────────────────────────────────────────────────────

// The null-conditional spelling of the same shape. §26.5 rule 1 commits
// instance resolution by NAME on this receiver too, so the built-in answers
// (the element count) and the user's extension is unreachable. Before the
// fix the `?.` path consulted the extension and dispatched it, so the
// program printed the extension's 77.
BEZEL_TEST(sema_collection_interface_inherited_member_7446,
           ilist_null_cond_count_answers_the_builtin) {
    // per §26.5 rule 1 (v0.345) + stdlib §4.1 + §7.5
    expectStdoutNullable("extension IList<int>\n"
                         "  public method Count(): int => 77;\n"
                         "end;\n",
                         "var i: int? := c?.Count();\n"
                         "    Console.WriteLine(i ?? -1);",
                         "3\n");
}

// The same `?.` spelling with NO extension: the built-in answers and the
// result is the element count. Before the fix the `?.` path found no member
// at all — the read was left untyped, the local degraded to a zero value
// (#2434) and the program printed -1.
BEZEL_TEST(sema_collection_interface_inherited_member_7446,
           ilist_null_cond_count_without_extension_answers_the_builtin) {
    // per §26.5 rule 1 (v0.345) + stdlib §4.1 + §7.5
    expectStdoutNullable("",
                         "var i: int? := c?.Count();\n"
                         "    Console.WriteLine(i ?? -1);",
                         "3\n");
}
