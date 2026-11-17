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
// fix #8633 (stdlib §1 "Block-by-name", §4.1, spec §7.5, §26.5 rule 1,
// §27.1, §27.7, v0.357): the CALL spelling of the inherited `Count`
// PROPERTY is refused outright — "`Count` is a property of `IList`, not a
// method (stdlib §4.1: `Count: int`), so this call has no candidate: the
// lookup commits to the instance member (§26.5 rule 1), and only a
// function-typed property can be called (§27.1, §27.7) — read it without
// parentheses, `.Count`". That is the #7543 refusal the concrete `Array<T>`
// head already gets; the extension's 77 is unreachable either way. §7.5 says
// the `?.` spelling performs the SAME access, so the `?.` spelling is
// refused with the SAME diagnostic. The PROPERTY READ `c.Count` / `c?.Count`
// — the spelling stdlib §4.1 declares — still answers, and that is pinned
// by the CONTROL rows.
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

// A rejection row over the NULLABLE receiver (the `?.` spelling).
void expectRejectedNullable(const std::string& ext, const std::string& body,
                             const std::string& needle) {
    auto r = runAot(progNullable(ext, body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// A rejection row that must carry EVERY named diagnostic (fix #8633: the
// property-call refusal, and the §5.1 refusal that follows it as the second
// report — the same two-report cascade the concrete `Array<T>` head has).
void expectRejectedWithAll(const std::string& ext, const std::string& body,
                           const std::vector<std::string>& needles) {
    auto r = runAot(prog(ext, body));
    BEZEL_EXPECT_FALSE(r.ran);
    for (const auto& needle : needles)
        BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// §5.1's refusal of an int-typed initializer for a `string`-typed local.
const char kNotConvertible[] =
    "the initializer for `s` has a type that is not implicitly convertible "
    "to the target type (§5.1)";

// fix #8633 (stdlib §1 "Block-by-name", §4.1, spec §26.5 rule 1, §27.1,
// §27.7, v0.357): the CALL spelling of the inherited `Count` PROPERTY is
// refused — "`Count` is a property of `IList`, not a method (stdlib §4.1:
// `Count: int`), so this call has no candidate: the lookup commits to the
// instance member (§26.5 rule 1), and only a function-typed property can be
// called (§27.1, §27.7) — read it without parentheses, `.Count`". The same
// refusal the concrete `Array<T>` head gets (#7543).
const char kCountIsAProperty[] =
    "`Count` is a property of `IList`, not a method (stdlib \xC2\xA7""4.1: "
    "`Count: int`)";

}  // namespace

// ── the DEFECT: the extension typed a member the receiver INHERITS ────────

// The `string` spelling. Before the fix it COMPILED — the built-in's `int`
// was stored in a `string`-typed local — which is the soundness defect.
//
// fix #8633 (stdlib §1 "Block-by-name", §4.1, spec §26.5 rule 1, §27.1,
// §27.7, v0.357): the CALL itself is refused now, with the #7543 diagnostic
// the concrete `Array<T>` head already gets — the committed member is the
// inherited `Count` PROPERTY, and a property of a non-function type is not
// callable. The `string` target's own §5.1 refusal follows it as the SECOND
// report (the same two-report cascade the `Array<int>` head has — MEASURED),
// so both diagnostics are pinned here.
BEZEL_TEST(sema_collection_interface_inherited_member_7446,
           ilist_count_string_target_rejected_property_call_8633) {
    // per stdlib §4.1 + §26.5 rule 1 + §27.1 + §27.7 (v0.357); §5.1
    expectRejectedWithAll("extension IList<int>\n"
                           "  public method Count(): string => \"USER\";\n"
                           "end;\n",
                           "var s: string := c.Count();\n"
                           "    Console.WriteLine(s);",
                           {kCountIsAProperty, kNotConvertible});
}

// The `int` spelling, with an extension whose OWN return type is `bool`.
// Before the fix the extension typed the call `bool`, so the `int` spelling
// was REFUSED by §5.1 while the `string` spelling was admitted — the two
// spellings disagreed. After the fix the call is the built-in's `int`.
//
// fix #8633 (stdlib §1 "Block-by-name", §4.1, spec §26.5 rule 1, §27.1,
// §27.7, v0.357): the CALL has no candidate at all now, so the §5.1 refusal
// of the `int` target is GONE — the call is refused by the property-call
// diagnostic alone, exactly as on the concrete `Array<T>` head.
BEZEL_TEST(sema_collection_interface_inherited_member_7446,
           ilist_count_bool_extension_int_target_is_refused_property_call_8633) {
    // per stdlib §4.1 + §26.5 rule 1 + §27.1 + §27.7 (v0.357)
    expectRejected("extension IList<int>\n"
                   "  public method Count(): bool => true;\n"
                   "end;\n",
                   "var i: int := c.Count();\n"
                   "    Console.WriteLine(i);",
                   kCountIsAProperty);
}

// ── CONTROLS ──────────────────────────────────────────────────────────────

// §26.5 rule 1 (v0.345) + stdlib §4.1: `IList<T>` INHERITS `Count`, so the
// extension is UNREACHABLE — the built-in would answer with the element count,
// not the extension's 77.
//
// fix #8633 (stdlib §1 "Block-by-name", §4.1, spec §26.5 rule 1, §27.1,
// §27.7, v0.357): the CALL spelling is refused altogether now (the committed
// member is the inherited `Count` PROPERTY, and a property of a non-function
// type is not callable), so the extension stays unreachable — and so does the
// built-in's answer. This is the #7543 refusal the concrete `Array<T>` head
// already gets.
BEZEL_TEST(sema_collection_interface_inherited_member_7446,
           ilist_count_extension_is_refused_property_call_8633) {
    // per stdlib §4.1 + §26.5 rule 1 + §27.1 + §27.7 (v0.357)
    expectRejected("extension IList<int>\n"
                   "  public method Count(): int => 77;\n"
                   "end;\n",
                   "var i: int := c.Count();\n"
                   "    Console.WriteLine(i);",
                   kCountIsAProperty);
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

// With NO extension at all the built-in used to answer, exactly as before the
// fix.
//
// fix #8633 (stdlib §1 "Block-by-name", §4.1, spec §26.5 rule 1, §27.1,
// §27.7, v0.357): the CALL is refused with the #7543 diagnostic — the
// inherited `Count` PROPERTY is committed by name, and a property of a
// non-function type is not callable. The PROPERTY READ `c.Count` is the
// spelling stdlib §4.1 declares, and it still answers; that control is a
// CORPUS candidate (it is green with and without the fix), not a gtest row —
// see `corpus-candidates/cc04_ilist_count_property_read_legal.bzl` and
// `cc05_ilist_nullcond_count_read_legal.bzl` in the lane's probe dir.
BEZEL_TEST(sema_collection_interface_inherited_member_7446,
           no_extension_builtin_call_is_refused_property_call_8633) {
    // per stdlib §4.1 + §26.5 rule 1 + §27.1 + §27.7 (v0.357)
    expectRejected("",
                   "var i: int := c.Count();\n"
                   "    Console.WriteLine(i);",
                   kCountIsAProperty);
}

// ── the `?.` twin ──────────────────────────────────────────────────────────

// The null-conditional spelling of the same shape. §26.5 rule 1 commits
// instance resolution by NAME on this receiver too, so the built-in used to
// answer (the element count) and the user's extension was unreachable.
//
// fix #8633 (stdlib §1 "Block-by-name", §4.1, spec §7.5, §26.5 rule 1,
// §27.1, §27.7, v0.357): §7.5 says the `?.` spelling performs the SAME
// access the `.` spelling does, so the two spellings of ONE member may not
// disagree — and the `.` spelling is refused. The `?.` spelling is refused
// with the SAME diagnostic.
BEZEL_TEST(sema_collection_interface_inherited_member_7446,
           ilist_null_cond_count_is_refused_property_call_8633) {
    // per stdlib §4.1 + §7.5 + §26.5 rule 1 + §27.1 + §27.7 (v0.357)
    expectRejectedNullable("extension IList<int>\n"
                            "  public method Count(): int => 77;\n"
                            "end;\n",
                            "var i: int? := c?.Count();\n"
                            "    Console.WriteLine(i ?? -1);",
                            kCountIsAProperty);
}

// The same `?.` spelling with NO extension: the built-in used to answer and
// the result was the element count. Before that fix the `?.` path found no
// member at all — the read was left untyped, the local degraded to a zero
// value (#2434) and the program printed -1.
//
// fix #8633 (stdlib §1 "Block-by-name", §4.1, spec §7.5, §26.5 rule 1,
// §27.1, §27.7, v0.357): the `?.` CALL is refused with the SAME diagnostic
// the `.` spelling gets.
BEZEL_TEST(sema_collection_interface_inherited_member_7446,
           ilist_null_cond_count_without_extension_is_refused_property_call_8633) {
    // per stdlib §4.1 + §7.5 + §26.5 rule 1 + §27.1 + §27.7 (v0.357)
    expectRejectedNullable("",
                            "var i: int? := c?.Count();\n"
                            "    Console.WriteLine(i ?? -1);",
                            kCountIsAProperty);
}

// CONTROL: the `?.` PROPERTY READ `c?.Count` is the spelling stdlib §4.1
// declares, and it still answers with the element count (3). fix #8633
// refuses the CALL spelling only. (A CORPUS candidate, not a gtest row — it
// is green with and without the fix. See
// `corpus-candidates/cc05_ilist_nullcond_count_read_legal.bzl` in the lane's
// probe dir.)
