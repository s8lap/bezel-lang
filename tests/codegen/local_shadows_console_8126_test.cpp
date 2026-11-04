// Regression tests for #8126 -- a local, parameter, field or lambda parameter
// named `Console` must WIN over the built-in console (the #8019 sibling).
//
// THE DEFECT.  #8019 made a value that shadows a built-in's TYPE name win over
// the text-keyed static arms (`Path`, `Math`, ...).  The `Console` path keys on
// the identifier text in its own lowering (`tryLowerConsoleCall`) and was not
// moved, so `Console.WriteLine("x")` with a user value named `Console` ran the
// BUILT-IN and printed `x`; the user's `WriteLine` never ran.
//
// WHAT THE DOCUMENTS REQUIRE.
//   * Spec §2.3 (v0.355), the paragraph after the Always-available table:
//     "Only the first row's names cannot be shadowed (§3.4); every other
//     always-available name may be shadowed by local declarations under the
//     normal name-resolution rules, and stands in those rules where an
//     imported bare name does (§2.7 rule 1)".
//   * Spec §2.7 rule 1 (v0.355): "If it resolves to a *value* (a local, field,
//     or property), the chain is member access."
//
// THE ORACLE.  The user's `WriteLine` stores its argument in a field, and the
// printing is done by a helper method outside the shadow's scope, so a row
// passes only if the USER's member ran (the built-in prints at the call site
// and leaves the field empty).

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

const char* kSh =
    "public class Sh "
    "  public last: string := \"\"; "
    "  public method WriteLine(s: string) "
    "    this.last := \"USER:\" + s; "
    "  end WriteLine; "
    "  public method Write(s: string) "
    "    this.last := \"UW:\" + s; "
    "  end Write; "
    "end Sh; ";

std::string prog(const std::string& members, const std::string& body) {
    return std::string("module M; ") + kSh +
           " public class C " + members +
           " public static method Show(s: string) "
           "   Console.WriteLine(s); "
           " end Show; "
           " public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

void expectOut(const std::string& members, const std::string& body,
               const std::string& want) {
    auto r = runAot(prog(members, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

}  // namespace

BEZEL_TEST(local_shadows_console_8126, local_var_calls_the_user_method) {
    expectOut("",
              "var Console := new Sh(); Console.WriteLine(\"x\"); "
              "Show(Console.last);",
              "USER:x\n");
}

BEZEL_TEST(local_shadows_console_8126, local_write_calls_the_user_method) {
    expectOut("",
              "var Console := new Sh(); Console.Write(\"w\"); "
              "Show(Console.last);",
              "UW:w\n");
}

BEZEL_TEST(local_shadows_console_8126, parameter_calls_the_user_method) {
    expectOut("public static method F(Console: Sh): string "
              "  Console.WriteLine(\"p\"); return Console.last; "
              "end F; ",
              "Show(F(new Sh()));",
              "USER:p\n");
}

BEZEL_TEST(local_shadows_console_8126, lambda_parameter_calls_the_user_method) {
    expectOut("",
              "var f := lambda(Console: Sh): string => do "
              "  Console.WriteLine(\"l\"); return Console.last; end; "
              "Show(f(new Sh()));",
              "USER:l\n");
}

BEZEL_TEST(local_shadows_console_8126, lambda_captured_local_calls_the_user_method) {
    expectOut("",
              "var Console := new Sh(); "
              "var f := lambda(): string => do "
              "  Console.WriteLine(\"c\"); return Console.last; end; "
              "Show(f());",
              "USER:c\n");
}

BEZEL_TEST(local_shadows_console_8126, bare_field_calls_the_user_method) {
    expectOut("public owned Console: Sh := new Sh(); "
              "public method G(): string "
              "  Console.WriteLine(\"f\"); return Console.last; "
              "end G; ",
              "var c := new C(); Show(c.G());",
              "USER:f\n");
}

// A two-argument user `WriteLine` is not subject to the built-in console's
// one-argument arity rule.
BEZEL_TEST(local_shadows_console_8126, user_overload_arity_is_the_users) {
    auto r = runAot(
        "module M; "
        "public class Sh2 "
        "  public last: string := \"\"; "
        "  public method WriteLine(a: string, b: string) "
        "    this.last := a + \"|\" + b; "
        "  end WriteLine; "
        "end Sh2; "
        "public class C "
        " public static method Show(s: string) Console.WriteLine(s); end Show; "
        " public static method Main(): int "
        "  var Console := new Sh2(); Console.WriteLine(\"a\", \"b\"); "
        "  Show(Console.last); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "a|b\n");
}

// ---- controls: the built-in still works --------------------------------

BEZEL_TEST(local_shadows_console_8126, control_builtin_console_still_prints) {
    expectOut("", "Console.WriteLine(\"hi\"); Console.Write(\"w\"); "
                  "Console.WriteLine();",
              "hi\nw\n");
}

// A local declared AFTER the use does not shadow it (§2.7 rule 1: the local
// scope is searched at the point of use).
BEZEL_TEST(local_shadows_console_8126, control_later_local_does_not_shadow) {
    expectOut("",
              "Console.WriteLine(\"before\"); "
              "var Console := new Sh(); Console.WriteLine(\"after\"); "
              "Show(Console.last);",
              "before\nUSER:after\n");
}

// A local with a different name leaves the built-in alone.
BEZEL_TEST(local_shadows_console_8126, control_other_local_name_is_unaffected) {
    expectOut("",
              "var Logger := new Sh(); Logger.WriteLine(\"q\"); "
              "Console.WriteLine(Logger.last);",
              "USER:q\n");
}

// ---- the `Encoding` arm (same mechanism, second commit of #8126) --------

namespace {
const char* kEnc =
    "public class Enc "
    "  public property UTF8: Enc => this; "
    "  public property Name: string => \"USERNAME\"; "
    "  public method GetString(s: string): string "
    "    return \"USER:\" + s; "
    "  end GetString; "
    "end Enc; ";
}  // namespace

// §2.3 / §2.7 rule 1 (v0.355): a local `Encoding` is a value, so
// `Encoding.UTF8.GetString("x")` is member access on it.  Base: sema typed
// `Encoding.UTF8` as the built-in singleton and rejected the string argument.
BEZEL_TEST(local_shadows_console_8126, encoding_local_calls_the_user_method) {
    auto r = runAot(std::string("module M; ") + kEnc +
                    " public class C public static method Main(): int "
                    "  var Encoding := new Enc(); "
                    "  Console.WriteLine(Encoding.UTF8.GetString(\"x\")); "
                    "  Console.WriteLine(Encoding.UTF8.Name); "
                    "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "USER:x\nUSERNAME\n");
}

BEZEL_TEST(local_shadows_console_8126, control_builtin_encoding_still_works) {
    auto r = runAot("module M; public class C public static method Main(): int "
                    "  var b := Encoding.UTF8.GetBytes(\"ab\"); "
                    "  Console.WriteLine(Encoding.UTF8.GetString(b)); "
                    "  Console.WriteLine(Encoding.UTF8.Name); "
                    "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ab\nutf-8\n");
}
