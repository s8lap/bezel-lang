// Regression test for issue #6340 -- a generic alias body reached by a
// QUALIFIED spelling resolved to the alias's own type parameter when that
// parameter happened to share its name with the built-in the qualifier
// names. Spec: docs/Bezel_Language_Spec.md, v0.338.
//
// THE GOVERNING RULE (§2.2, §2.7 rule 2, §27.8).
//   §2.2 (`sec-always-available-names`, the paragraph under the auto-import
//   table): "The imported name is reached by qualification (its module name
//   or an alias); the always-available one BY ITS MODULE'S QUALIFIED NAME
//   (`System.Exception`; `System.Threading.Task` under `import
//   System.Threading;`, §2.3 ...). No opt-out exists."
//   §2.7 rule 2: a dotted name is reached by "match[ing] the longest dotted
//   prefix of the chain against an imported module's full name" -- a bare
//   type PARAMETER is never a module, so it cannot claim a dotted prefix.
//   §27.8: "Each instantiation of the alias is interchangeable with the
//   corresponding instantiation of the aliased type."
// The spec states the same asymmetry concretely for the root type: "a
// nested type or a type parameter named `Object` shadows it, and
// `System.Object` still reaches it" (§2.2 table row 1, and §8.1). So the
// BARE spelling shadows (the parameter wins) and the QUALIFIED spelling
// still reaches the built-in -- which is exactly what this test pins.
//
// ROOT CAUSE (MEASURED). `Resolver::resolveNamed`'s generic-alias bare-body
// fast path (src/sema/TypeResolution_Names.cpp) fires when the alias body is
// a single-part Named with no generic arguments, and then looks the body's
// name up in the alias's own substitution map BY NAME ALONE:
//     auto it = aliasSubst.find(aliasDecl->aliasedType->name->parts.front());
// `#4698` strips the `System.` qualifier off `System.Exception` during
// `resolveNamed` and stamps `aliasQualifiedBuiltin` on the ast::Type -- at
// which point the body is indistinguishable from the bare-parameter shape
// `type Id<T> := T`. So for `type W<Exception> := System.Exception;` the
// lookup found the parameter named `Exception` and `W<int>` became `int`.
// The pre-fix rejection is a §5.1 MISATTRIBUTION whose own text proves the
// target was wrong: "the target is a value type", and a second diagnostic
// naming `int` explicitly ("no member `Message` on primitive type `int`")
// on a member the program reads off an exception.
//
// FIX. Exclude the `aliasQualifiedBuiltin`-stamped body from the fast path,
// sending it down the general `resolveNamed` path, which resolves the
// qualifier correctly. This is #4698's OWN exclusion, and the identical
// predicate `paramHead()` already applies on the substitution path
// (src/sema/SubstArgs.cpp). #6324 fixed a DIFFERENT trigger in the same
// guard (a `T?` body, whose `nullable` flag the fast path dropped); that
// fix is orthogonal and both guards are present.
//
// FAIL-WITHOUT. Every ACCEPT row below is rejected on the pre-fix compiler
// with the §5.1 misattribution above; the two REJECT rows (a
// self-reference and a genuine name collision that the spec DOES want
// resolved through the parameter) pass on it too, which is what shows the
// fix did not simply delete the substitution.
//
// This is a sibling of #6324, not a restatement: #6324's reviewer
// (`r31-rv-6324`) enumerated all nine `ast::Type` fields and found
// `nullable` was the only one the guard could drop. That enumeration was
// correct about which ATTRIBUTE is droppable, but it read
// `aliasQualifiedBuiltin` as making the substitution "miss" -- the flag
// makes the body a built-in NAME, and when a parameter shares that name the
// lookup HITS instead. That is this corner, a different defect with a
// different trigger.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {

// Compile under the checked (ASan) oracle, expect no diagnostic, and assert
// the program RUNS and prints `out`. Every ACCEPT row asserts a VALUE, so a
// row cannot pass by degrading to a discarded compile.
void expectRuns(const std::string& prog, const char* out) {
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

// Compile and expect a diagnostic naming `needle`.
void expectRejected(const std::string& prog, const char* needle) {
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

}  // namespace

// -- The issue's exact repro (§2.2, §2.7 rule 2, §27.8).
//
// `W<Exception> := System.Exception` declares a parameter named `Exception`
// that the body does NOT mention. `W<int>` is `System.Exception`, so
// `new Exception("x")` initializes it directly, and `m.Message` reads a
// real member. Pre-fix `W<int>` was `int`, so the initializer §5.1-rejected
// with "the target is a value type" and the member read named `int`.
BEZEL_TEST(sema_alias_qualified_body_param_collision_6340,
           issue_repro_qualified_builtin_body_reaches_builtin_spec_2_2) {
    expectRuns("module M; "
               "type W<Exception> := System.Exception; "
               "static class C "
               "  public static method Main(): int "
               "    var m: W<int> := new Exception(\"x\"); "
               "    Console.WriteLine(m.Message); "
               "    return 0; "
               "  end Main; "
               "end C;",
               "x\n");
}

// -- The boundary the fix must NOT cross: the BARE spelling still lets the
// parameter capture the name (§2.2 table row 1 / §8.1: "a nested type or a
// type parameter named `Object` shadows it, and `System.Object` still
// reaches it"). `type W<T> := T` is the canonical bare body and must keep
// substituting -- this is #4447's rule, and the fast path exists for it.
BEZEL_TEST(sema_alias_qualified_body_param_collision_6340,
           bare_body_still_substitutes_the_parameter_spec_27_8) {
    expectRuns("module M; "
               "type W<T> := T; "
               "static class C "
               "  public static method Main(): int "
               "    var m: W<int> := 5; "
               "    Console.WriteLine(m); "
               "    return 0; "
               "  end Main; "
               "end C;",
               "5\n");
}

// -- The same rule through a qualified body of a NON-exception built-in, so
// the row is not pinned to the §31.4 exception hierarchy.
BEZEL_TEST(sema_alias_qualified_body_param_collision_6340,
           qualified_builder_body_reaches_builtin_spec_2_3) {
    expectRuns("module M; "
               "type W<StringBuilder> := System.Text.StringBuilder; "
               "static class C "
               "  public static method Main(): int "
               "    var b: W<int> := new StringBuilder(); "
               "    b.Append(\"hi\"); "
               "    Console.WriteLine(b.ToString()); "
               "    return 0; "
               "  end Main; "
               "end C;",
               "hi\n");
}

// -- A CHAINED alias: `W2<Exception> := W<int>` is still a qualified-body
// alias one level down, and `W2<string>` must be `System.Exception`, not
// `string`. Pins that the fix composes through alias-of-alias (§27.8
// "Aliases of aliases are permitted") rather than fixing only the
// first-reference spelling.
BEZEL_TEST(sema_alias_qualified_body_param_collision_6340,
           chained_alias_composes_spec_27_8) {
    expectRuns("module M; "
               "type W<Exception> := System.Exception; "
               "type W2<Exception> := W<int>; "
               "static class C "
               "  public static method Main(): int "
               "    var m: W2<string> := new Exception(\"y\"); "
               "    Console.WriteLine(m.Message); "
               "    return 0; "
               "  end Main; "
               "end C;",
               "y\n");
}

// -- Control: an ordinary alias-of-alias with no collision keeps resolving
// (guards against the fix sending ALL alias bodies down the general path in
// a way that loses substitution).
BEZEL_TEST(sema_alias_qualified_body_param_collision_6340,
           control_nested_alias_without_collision_still_resolves) {
    expectRuns("module M; "
               "type Inner<T> := T; "
               "type Outer<T> := Inner<T>; "
               "static class C "
               "  public static method Main(): int "
               "    var m: Outer<int> := 7; "
               "    Console.WriteLine(m); "
               "    return 0; "
               "  end Main; "
               "end C;",
               "7\n");
}

// -- REJECT: the self-reference rule is unaffected (§27.8 "An alias may not
// reference itself, directly or through other aliases"). Passes both before
// and after the fix, which is what shows the guard was not simply widened
// into deleting checks.
BEZEL_TEST(sema_alias_qualified_body_param_collision_6340,
           control_self_reference_still_rejected_spec_27_8) {
    expectRejected("module M; "
                   "type A<T> := Array<T> where T: A<T>; "
                   "static class C "
                   "  public static method Main(): int "
                   "    return 0; "
                   "  end Main; "
                   "end C;",
                   "recursive");
}