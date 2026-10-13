// Spec-derived regression tests for issue #5862 -- overloads that differ only
// in WHICH built-in exception class they name (`Log(e: Exception)` beside
// `Log(e: ArgumentException)`, the §31.4 family) were rejected as
// indistinguishable, and once declared the call picked the first-declared
// overload whatever the argument was.
//
// Spec lines (docs/Bezel_Language_Spec.md, v0.338):
//   §22.6 (line 4503): "Overload resolution considers number, types, the
//     `ref`/`out` modifiers, and the number of type parameters a method
//     declares of its own -- overloads differing in any of these are
//     distinguishable".
//   §11.3 (line 1891): two methods clash only when they "do not differ in
//     number of parameters, number of type parameters of their own, parameter
//     types, or the `ref`/`out` modifier".
//   §22.6 *Implicit conversions and tie-breaking* (line 4564): a candidate is
//     better at a position only when it is an EXACT match there and the other
//     converts; "two candidates that both require an implicit conversion at the
//     same position are equally ranked"; "`M(a: Animal)` versus `M(o: Object)`
//     on a `Dog` argument is ambiguous -- neither candidate is exact".
//   §22.6 (line 4576): no candidate unambiguously better -> ambiguous, a compile
//     error; with no applicable candidate at all, the no-candidate error.
//   §12 / §5.1 reference conversions: a derived class converts to its base.
//   §31.4: `ArgumentException` and `InvalidOperationException` are siblings
//     under `Exception`.
//
// TWO-PART FIX, one commit: (A) `signaturesIndistinguishable` compared the
// declaration POINTER (null for every built-in class) so any two built-in
// exceptions were equal; (B) the call side -- `resolveOverload`'s `judgeable`
// rejected a built-in class argument, `classifyArg` compared by declaration
// pointer, and `collectHierarchyOverloads` dropped the second sibling as a
// "shadowed" override -- so with (A) alone `Log(a)` for `a: ArgumentException`
// still ran the Exception overload (a silent wrong overload). Every run row
// below prints which overload ran, so (A) without (B) fails them.
//
// Not covered here (still rejected / unchanged, see the commit body): built-in
// INTERFACES and non-exception built-in classes, and a built-in exception
// nested in a generic argument (`Array<Exception>` beside
// `Array<ArgumentException>`).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kMyEx = "class MyEx: Exception end MyEx; ";

std::string staticProg(const std::string& p1, const std::string& p2,
                       const std::string& argDecl, const std::string& arg) {
    return "module M; " + std::string(kMyEx) +
           "static class C "
           "  private static method Log(e: " + p1 + "): int => 11; "
           "  private static method Log(e: " + p2 + "): int => 22; "
           "  public static method Main(): int "
           "    " + argDecl + " "
           "    Console.WriteLine(Log(" + arg + ")); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// Compile, run under the checked (ASan) oracle, and expect `out` on stdout.
void expectRuns(const std::string& prog, const char* out) {
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

}  // namespace

// -- The issue's exact declarations compile (§22.6, §11.3).
BEZEL_TEST(sema_overload_builtin_class_distinct_5862,
           issue_repro_declarations_accepted_spec_22_6) {
    auto r = runAotChecked(
        "module M; "
        "type Show := (int) -> string; "
        "class Log "
        "  private static method Log(message: string) end Log; "
        "  private static method Log(e1: Exception) end Log; "
        "  private static method Log(e2: ArgumentException) end Log; "
        "end Log; "
        "class P public static method Main(): int return 0; end Main; end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// -- Exact match on the derived overload: part (B) -- (A) alone prints 11.
BEZEL_TEST(sema_overload_builtin_class_distinct_5862,
           derived_arg_picks_derived_overload_spec_22_6) {
    expectRuns(staticProg("Exception", "ArgumentException",
                          "using var a := new ArgumentException(\"x\");", "a"),
               "22\n");
}

// -- Exact match on the base overload (declared first).
BEZEL_TEST(sema_overload_builtin_class_distinct_5862,
           base_arg_picks_base_overload_spec_22_6) {
    expectRuns(staticProg("Exception", "ArgumentException",
                          "using var a := new Exception(\"x\");", "a"),
               "11\n");
}

// -- A binding whose STATIC type is Exception picks the Exception overload
// even though it holds an ArgumentException (§22.6 ranks static types).
BEZEL_TEST(sema_overload_builtin_class_distinct_5862,
           static_type_exception_holding_derived_picks_base_spec_22_6) {
    expectRuns(staticProg("Exception", "ArgumentException",
                          "using var a: Exception := new ArgumentException(\"x\");",
                          "a"),
               "11\n");
}

// -- The reverse declaration order does not change the winner.
BEZEL_TEST(sema_overload_builtin_class_distinct_5862,
           reverse_declaration_order_same_winner_spec_22_6) {
    expectRuns(staticProg("ArgumentException", "Exception",
                          "using var a := new ArgumentException(\"x\");", "a"),
               "11\n");
}

// -- A sibling class (InvalidOperationException) converts only to Exception.
BEZEL_TEST(sema_overload_builtin_class_distinct_5862,
           sibling_arg_converts_to_base_only_spec_31_4) {
    expectRuns(staticProg("Exception", "ArgumentException",
                          "using var a := new InvalidOperationException(\"x\");",
                          "a"),
               "11\n");
}

// -- A USER class deriving Exception converts to the Exception overload.
BEZEL_TEST(sema_overload_builtin_class_distinct_5862,
           user_derived_arg_converts_to_builtin_base_spec_12) {
    expectRuns(staticProg("Exception", "ArgumentException",
                          "using var a := new MyEx();", "a"),
               "11\n");
}

// -- Two sibling built-in classes: each argument picks its own overload.
BEZEL_TEST(sema_overload_builtin_class_distinct_5862,
           sibling_overloads_first_spec_22_6) {
    expectRuns(staticProg("ArgumentException", "InvalidOperationException",
                          "using var a := new ArgumentException(\"x\");", "a"),
               "11\n");
}
BEZEL_TEST(sema_overload_builtin_class_distinct_5862,
           sibling_overloads_second_spec_22_6) {
    expectRuns(staticProg("ArgumentException", "InvalidOperationException",
                          "using var a := new InvalidOperationException(\"x\");",
                          "a"),
               "22\n");
}

// -- Instance method.
BEZEL_TEST(sema_overload_builtin_class_distinct_5862,
           instance_method_overloads_spec_22_6) {
    expectRuns(
        "module M; class K "
        "  public method Log(e: Exception): int => 11; "
        "  public method Log(e: ArgumentException): int => 22; "
        "end K; "
        "class P public static method Main(): int "
        "  using var k := new K(); "
        "  using var a := new ArgumentException(\"x\"); "
        "  using var b := new InvalidOperationException(\"x\"); "
        "  Console.WriteLine(k.Log(a)); "
        "  Console.WriteLine(k.Log(b)); "
        "  return 0; "
        "end Main; end P;",
        "22\n11\n");
}

// -- Constructors (§22.6: "Methods and constructors may be overloaded").
BEZEL_TEST(sema_overload_builtin_class_distinct_5862,
           constructor_overloads_spec_22_6) {
    expectRuns(
        "module M; class K "
        "  public R: int; "
        "  public constructor(e: Exception) R := 11; end constructor; "
        "  public constructor(e: ArgumentException) R := 22; end constructor; "
        "end K; "
        "class P public static method Main(): int "
        "  using var a := new ArgumentException(\"x\"); "
        "  using var b := new InvalidOperationException(\"x\"); "
        "  using var k1 := new K(a); "
        "  using var k2 := new K(b); "
        "  Console.WriteLine(k1.R); "
        "  Console.WriteLine(k2.R); "
        "  return 0; "
        "end Main; end P;",
        "22\n11\n");
}

// -- A generic method with one built-in parameter beside a sibling overload.
BEZEL_TEST(sema_overload_builtin_class_distinct_5862,
           generic_method_overloads_spec_22_6_11_3) {
    expectRuns(
        "module M; static class C "
        "  private static method Log<T>(x: T, e: Exception): int => 11; "
        "  private static method Log<T>(x: T, e: ArgumentException): int => 22; "
        "  public static method Main(): int "
        "    using var a := new ArgumentException(\"x\"); "
        "    Console.WriteLine(Log<int>(1, a)); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "22\n");
}

// -- A built-in exception overload beside a `string` overload still resolves by
// the argument (the #5859 shape), now with the derived overload present too.
BEZEL_TEST(sema_overload_builtin_class_distinct_5862,
           string_overload_alongside_exception_pair_spec_22_6) {
    expectRuns(
        "module M; static class C "
        "  private static method Log(m: string): int => 33; "
        "  private static method Log(e: Exception): int => 11; "
        "  private static method Log(e: ArgumentException): int => 22; "
        "  public static method Main(): int "
        "    using var a := new ArgumentException(\"x\"); "
        "    Console.WriteLine(Log(a)); "
        "    Console.WriteLine(Log(\"s\")); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "22\n33\n");
}

// -- REJECT: truly identical signatures stay indistinguishable (§11.3).
BEZEL_TEST(sema_overload_builtin_class_distinct_5862,
           same_builtin_class_twice_still_rejected_spec_11_3) {
    auto r = runAot(
        "module M; static class C "
        "  private static method Log(e: Exception): int => 11; "
        "  private static method Log(e: Exception): int => 22; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "is indistinguishable from another overload"));
}

// -- REJECT: parameter NAMES never distinguish (§11.3).
BEZEL_TEST(sema_overload_builtin_class_distinct_5862,
           same_derived_class_different_param_names_still_rejected_spec_11_3) {
    auto r = runAot(
        "module M; static class C "
        "  private static method Log(a: ArgumentException): int => 11; "
        "  private static method Log(b: ArgumentException): int => 22; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "is indistinguishable from another overload"));
}

// -- REJECT: a bare type parameter pair is still a clash (§11.3 generic rule).
BEZEL_TEST(sema_overload_builtin_class_distinct_5862,
           bare_type_parameters_still_clash_spec_11_3) {
    auto r = runAot(
        "module M; static class C "
        "  private static method M1<T>(x: T): int => 11; "
        "  private static method M1<U>(x: U): int => 22; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "is indistinguishable from another overload"));
}

// -- REJECT (§22.6 line 4564): both candidates need a conversion from a derived
// argument -> equally ranked -> ambiguous, never a silent pick.
BEZEL_TEST(sema_overload_builtin_class_distinct_5862,
           derived_arg_object_vs_base_is_ambiguous_spec_22_6) {
    auto r = runAot(staticProg("Exception", "Object",
                               "using var a := new ArgumentException(\"x\");",
                               "a"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "ambiguous call to `Log`"));
}

// -- REJECT (§22.6 line 4576): the sibling overloads with the common BASE as the
// argument's static type -- neither converts -> no applicable candidate.
BEZEL_TEST(sema_overload_builtin_class_distinct_5862,
           common_base_arg_matches_no_sibling_overload_spec_22_6) {
    auto r = runAot(staticProg("ArgumentException", "InvalidOperationException",
                               "using var a: Exception := new Exception(\"x\");",
                               "a"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no overload of `Log` is applicable"));
}
