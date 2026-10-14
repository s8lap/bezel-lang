// Spec-derived regression tests for issue #6002 -- a §7.4 flow-narrowed
// nullable argument (`n: Animal?` inside `if n <> null`) was "no overload of
// `L` is applicable" among `L(Animal)` / `L(string)`, while the same call
// against `L(Animal)` alone compiles.
//
// Spec lines (docs/Bezel_Language_Spec.md, v0.338):
//   §7.4: a nullable local is narrowed non-null by a null test, an early
//     return, and by assignment "from a declaration whose initializer is such
//     an `expr`".
//   §7.11: "The rule reads the operand's **static** type, so an operand that
//     §7.4 has narrowed non-null is a non-nullable operand".
//   §22.6: a candidate is applicable when each argument is its parameter's
//     type or implicitly converts to it; "two candidates that both require an
//     implicit conversion at the same position are **equally ranked** there".
//
// CAUSE: overload ranking graded the argument at its DECLARED nullable type,
// which §7 forbids at a non-nullable parameter, so every candidate was
// inapplicable. When ordinary grading finds no candidate, a narrowed read is
// now also graded at its non-null type.
//
// INTERPRETATION RECORDED (the open half is a filed spec question): such a
// read is graded as a CONVERSION, even to its own non-null type. So it
// selects the one candidate that admits it and is ambiguous between two; it
// never silently prefers. Calls that already resolve (a `T?` overload is
// present) are not regraded and select what they selected before.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// `S.L(first)` returns 1, `S.L(second)` returns 2; `Main` runs `body`.
std::string prog(const std::string& first, const std::string& second,
                 const std::string& body) {
    return "module M; "
           "class Animal end Animal; class Dog: Animal end Dog; "
           "static class S "
           "  public static method L(e: " + first + "): int => 1; "
           "  public static method L(e: " + second + "): int => 2; "
           "  public static method MkA(): owned Animal? => new Animal(); "
           "  public static method MkD(): owned Dog? => new Dog(); "
           "  public static method MkS(): string? => \"s\"; "
           "  public static method MkI(): int? => 3; "
           "end S; "
           "class P public static method Main(): int " + body +
           "  return 0; end Main; end P;";
}

void expectRuns(const std::string& p, const char* out) {
    auto r = runAotChecked(p);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

void expectRejects(const std::string& p, const char* needle) {
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

}  // namespace

// -- The issue's repro. Its `F` is spelled `owned Animal?` and its local
//    `using var` here: the issue's own text returns a class from a static
//    method without `owned`, which is rejected before overloading is reached.
BEZEL_TEST(sema_overload_narrowed_nullable_arg_6002,
           issue_repro_if_narrowed_class_local_spec_7_4) {
    expectRuns(
        "module M; class Animal end Animal;  static class C\n"
        " private static method L(e: Animal): int => 1;\n"
        " private static method L(s: string): int => 2;\n"
        " private static method F(): owned Animal? => new Animal();\n"
        " public static method Main(): int \n"
        "  using var n := F();\n"
        "  if n <> null then Console.WriteLine(L(n)); end if;\n"
        "  return 0; end Main; end C;\n",
        "1\n");
}

// -- Both declaration orders, `if`-narrowed.
BEZEL_TEST(sema_overload_narrowed_nullable_arg_6002,
           if_narrowed_class_local_both_orders_spec_7_4) {
    const char* body =
        "using var n := S.MkA(); "
        "if n <> null then Console.WriteLine(S.L(n)); end if; ";
    expectRuns(prog("Animal", "string", body), "1\n");
    expectRuns(prog("string", "Animal", body), "2\n");
}

// -- Early-return narrowing (§7.4).
BEZEL_TEST(sema_overload_narrowed_nullable_arg_6002,
           early_return_narrowed_class_local_spec_7_4) {
    expectRuns(prog("Animal", "string",
                    "using var n := S.MkA(); "
                    "if n = null then return 1; end if; "
                    "Console.WriteLine(S.L(n)); "),
               "1\n");
}

// -- A narrowed `Dog?` reaches a BASE-typed parameter: the one applicable
//    candidate of `L(Animal)` / `L(string)`.
BEZEL_TEST(sema_overload_narrowed_nullable_arg_6002,
           if_narrowed_derived_local_reaches_base_param_spec_7_4) {
    expectRuns(prog("string", "Animal",
                    "using var d := S.MkD(); "
                    "if d <> null then Console.WriteLine(S.L(d)); end if; "),
               "2\n");
}

// -- Value and string carriers: `int?` and `string?`, `if`-narrowed.
BEZEL_TEST(sema_overload_narrowed_nullable_arg_6002,
           if_narrowed_value_and_string_locals_spec_7_4) {
    expectRuns(prog("int", "string",
                    "var i := S.MkI(); var s := S.MkS(); "
                    "if i <> null then Console.WriteLine(S.L(i)); end if; "
                    "if s <> null then Console.WriteLine(S.L(s)); end if; "),
               "1\n2\n");
}

// -- A narrowed PARAMETER is a §7.4 carrier too.
BEZEL_TEST(sema_overload_narrowed_nullable_arg_6002,
           if_narrowed_parameter_spec_7_4) {
    expectRuns(
        "module M; class Animal end Animal; "
        "static class S "
        "  public static method L(e: Animal): int => 1; "
        "  public static method L(e: string): int => 2; "
        "  public static method Via(n: Animal?): int "
        "    if n <> null then return L(n); end if; "
        "    return 0; "
        "  end Via; "
        "end S; "
        "class P public static method Main(): int "
        "  using var a := new Animal(); "
        "  Console.WriteLine(S.Via(a)); Console.WriteLine(S.Via(null)); "
        "  return 0; end Main; end P;",
        "1\n0\n");
}

// -- Constructors rank through the same grid.
BEZEL_TEST(sema_overload_narrowed_nullable_arg_6002,
           if_narrowed_local_constructor_overloads_spec_7_4) {
    expectRuns(
        "module M; class Animal end Animal; "
        "class Box "
        "  public Tag: int; "
        "  public constructor(x: Animal) Tag := 1; end constructor; "
        "  public constructor(x: string) Tag := 2; end constructor; "
        "end Box; "
        "static class S "
        "  public static method MkA(): owned Animal? => new Animal(); "
        "end S; "
        "class P public static method Main(): int "
        "  using var n := S.MkA(); "
        "  if n <> null then "
        "    using var b := new Box(n); Console.WriteLine(b.Tag); "
        "  end if; "
        "  return 0; end Main; end P;",
        "1\n");
}

// -- REJECT: the same call WITHOUT the narrowing stays the no-candidate
//    error -- an un-narrowed `Animal?` reaches neither parameter (§7).
BEZEL_TEST(sema_overload_narrowed_nullable_arg_6002,
           un_narrowed_nullable_still_rejected_spec_7) {
    expectRejects(prog("Animal", "string",
                       "using var n := S.MkA(); Console.WriteLine(S.L(n)); "),
                  "no overload of `L` is applicable");
}

// -- REJECT: narrowed, but neither parameter admits an `Animal`.
BEZEL_TEST(sema_overload_narrowed_nullable_arg_6002,
           narrowed_but_no_admitting_candidate_still_rejected_spec_22_6) {
    expectRejects(prog("int", "string",
                       "using var n := S.MkA(); "
                       "if n <> null then Console.WriteLine(S.L(n)); end if; "),
                  "no overload of `L` is applicable");
}

// -- REJECT: a narrowed `Dog?` beside `L(Dog)` / `L(Animal)` is graded a
//    conversion to each, so the call is ambiguous (the recorded
//    interpretation; pre-fix it was the no-candidate error, so this stays a
//    rejection either way). Pending #6220(a), the narrowed argument's rank.
BEZEL_TEST(sema_overload_narrowed_nullable_arg_6002,
           narrowed_derived_between_two_admitting_candidates_pending_6220) {
    expectRejects(prog("Dog", "Animal",
                       "using var d := S.MkD(); "
                       "if d <> null then Console.WriteLine(S.L(d)); end if; "),
                  "ambiguous call to `L`");
}

// -- CONTROL: with a `T?` overload present the call already resolves and is
//    not regraded: the nullable overload runs, narrowed or not, as before.
BEZEL_TEST(sema_overload_narrowed_nullable_arg_6002,
           nullable_overload_present_selection_unchanged_spec_22_6) {
    expectRuns(prog("Animal", "Animal?",
                    "using var n := S.MkA(); "
                    "Console.WriteLine(S.L(n)); "
                    "if n <> null then Console.WriteLine(S.L(n)); end if; "),
               "2\n2\n");
}
