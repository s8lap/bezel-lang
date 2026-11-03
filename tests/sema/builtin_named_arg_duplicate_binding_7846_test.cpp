// Regression test for issue #7846 (spec §22.5, §5.1, stdlib §9.6, v0.350).
//
// THE DEFECT. §22.5 (v0.350) has two rules about a built-in member's
// argument list:
//
//   * a named argument binds the parameter it NAMES, in any order; and
//   * a parameter may not be bound TWICE, and a positional argument may not
//     FOLLOW a named one.
//
// stdlib §9.6 (v0.350) declares
//
//   public pure method PadLeft(totalWidth: int, paddingChar: char := ' '): string;
//   public pure method Insert(startIndex: int, value: string): string;
//
// so `"ab".PadLeft(totalWidth := 5, totalWidth := 6)` binds `totalWidth`
// twice -- a §22.5 error -- and `"ab".PadLeft(20, totalWidth := 20)` binds it
// once positionally and once by name, which is the same double binding.
// §22.5's OTHER rule is the positional-after-named one:
// `"ab".Insert(startIndex := 1, 2)` -- the named argument binds the first
// parameter, so the positional one would have to bind the second, and §22.5
// refuses the ORDER.
//
// The §9.6 STRING-CALL gate (`checkBuiltinStringCallArgs`) and the §9.6
// declared-parameter gate (`checkBuiltinStringDeclaredParams`) kept the
// POSITIONAL reading when the §22.5 binding resolved to no candidate, so
// they fell back to the IDENTITY mapping and asked §5.1's type question about
// the WRONG parameter. The correct §22.5 diagnostic fired, and a SECOND,
// SPURIOUS type error fired beside it:
//
//   call to built-in `string.PadLeft`: parameter `totalWidth` is bound more
//       than once (§22.5)
//   the argument to `string.PadLeft` has a type that is not implicitly
//       convertible to the target type (§5.1)      <-- SPURIOUS
//
// WHAT FIXED IT. Commit 8aaaebb5 ("fix #6810: the §7 nullability gate reads
// the §22.5 BOUND position of a named argument") removed the identity-mapping
// fallback from BOTH gates: "The identity-mapping fallback is GONE in both
// gates." A call the §22.5 binder has already refused now draws the §22.5
// diagnostic and NOTHING else.
//
// That commit landed its own issues in its SUBJECT -- #6810, #7622, #7535 and
// #8036 -- and carries no closing line for #7846: `scripts/pick_issues.sh
// --github 8aaaebb5` prints exactly those four, so #7846 was never closed
// even though its defect was already gone from the tree. This file pins it.
//
// THE ORACLE. `runAot` is the strict AOT oracle (fork-isolated
// Driver::compileFile + clang link + native exec). Every REFUSAL row asserts
// the POSITIVE: the exact §22.5 diagnostic text, at a REAL source location
// (failedAtCompileTimeWithRealLocation), AND the COUNT of the spurious §5.1
// type error, which is zero. The count is observable and not vacuous: the
// program is REFUSED, so the diagnostics list is non-empty by construction
// (the §22.5 diagnostic is asserted present in the same row), and what the
// count observes is that no SECOND diagnostic is there. Every CONTROL row
// asserts the exact stdout the standard library's member computes.
//
// ASan is OFF in lanes by default (CLAUDE.md): this is an argument's binding
// and a diagnostic, not allocation, layout, retain/release or disposal.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::diagnosticMessageCount;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// The program body is wrapped in a class with a `Main` method.
std::string prog(const std::string& body) {
    return "module M;\n"
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// A refusal row: the program must NOT run, the §22.5 diagnostic must carry
// `needle` at a REAL source location, and the spurious §5.1 type error must
// not be there beside it.
void expectOneDiagnostic(const std::string& body, const char* needle,
                         const char* spurious) {
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // The defect was a SECOND, spurious diagnostic beside the correct one.
    // The program is refused (the §22.5 diagnostic is asserted present
    // above), so the count observes a real list, not an empty one.
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, spurious), 0);
}

// A value row: the program must compile CLEAN, run, exit 0 and print exactly
// this stdout.
void expectStdout(const std::string& body, const std::string& expected) {
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') out.pop_back();
    BEZEL_EXPECT_EQ(out, expected);
}

}  // namespace

// ===========================================================================
// #7846 -- the duplicate parameter name is the §22.5 error, and NOTHING
// else.
// ===========================================================================

// stdlib §9.6's `PadLeft(totalWidth: int, paddingChar: char := ' ')` -- the
// name `totalWidth` is bound twice, so §22.5 refuses the call. The refusal
// is ONE diagnostic: the spurious "not implicitly convertible" type error
// that the identity-mapping fallback produced is gone.
BEZEL_TEST(builtin_named_arg_duplicate_binding_7846,
           a_duplicate_parameter_name_draws_only_the_22_5_error) {
    expectOneDiagnostic("    var s := \"hello\";\n"
                        "    Console.WriteLine(s.PadLeft(totalWidth := 20, totalWidth := 21));\n",
                        "call to built-in `string.PadLeft`: parameter `totalWidth` "
                        "is bound more than once (§22.5)",
                        "not implicitly convertible");
}

// The same double binding spelled POSITIONALLY first: the positional
// argument binds `totalWidth` and the named one binds it again.
BEZEL_TEST(builtin_named_arg_duplicate_binding_7846,
           a_positional_argument_that_rebinds_a_named_parameter_draws_only_the_22_5_error) {
    expectOneDiagnostic("    var s := \"hello\";\n"
                        "    Console.WriteLine(s.PadLeft(20, totalWidth := 20));\n",
                        "call to built-in `string.PadLeft`: parameter `totalWidth` "
                        "is bound more than once (§22.5)",
                        "not implicitly convertible");
}

// The same shape on another §9.6 member, so the row is about the BINDER and
// not about one member's spelling. stdlib §9.6 declares
// `Insert(startIndex: int, value: string): string`.
BEZEL_TEST(builtin_named_arg_duplicate_binding_7846,
           a_duplicate_parameter_name_on_another_string_member_draws_only_the_22_5_error) {
    expectOneDiagnostic("    var s := \"hello\";\n"
                        "    Console.WriteLine(s.Insert(startIndex := 1, startIndex := 2));\n",
                        "call to built-in `string.Insert`: parameter `startIndex` "
                        "is bound more than once (§22.5)",
                        "not implicitly convertible");
}

// The SECOND half of the issue's title: a POSITIONAL argument that FOLLOWS a
// named one. §22.5: "a positional argument may not follow a named argument".
// The named `startIndex := 1` binds the FIRST parameter, so the positional
// `2` would have to bind `value: string` -- and under the identity-mapping
// fallback the §5.1 gate asked about the WRONG parameter and drew a second,
// spurious type error beside the correct §22.5 one.
BEZEL_TEST(builtin_named_arg_duplicate_binding_7846,
           a_positional_argument_after_a_named_one_draws_only_the_22_5_error) {
    expectOneDiagnostic("    var s := \"hello\";\n"
                        "    Console.WriteLine(s.Insert(startIndex := 1, 2));\n",
                        "call to built-in `string.Insert`: a positional argument "
                        "may not follow a named argument (§22.5",
                        "not implicitly convertible");
}

// ===========================================================================
// THE CONTROL ROWS -- the conforming spellings still compute stdlib §9.6's
// value, and a legal NAMED argument is still admitted. This is what proves
// the gate refuses the DOUBLE BINDING and not the named argument.
// ===========================================================================

BEZEL_TEST(builtin_named_arg_duplicate_binding_7846,
           the_positional_and_the_named_spelling_still_pad_9_6) {
    expectStdout("    var s := \"hello\";\n"
                 "    Console.WriteLine(\"[\" + s.PadLeft(8) + \"]\");\n"
                 "    Console.WriteLine(\"[\" + s.PadLeft(totalWidth := 8) + \"]\");\n",
                 "[   hello]\n[   hello]");
}

// A legal REORDERED named argument -- §22.5's own rule ("a named argument
// binds the parameter it names, in any order") -- is still admitted and
// computes stdlib §9.6's value.
BEZEL_TEST(builtin_named_arg_duplicate_binding_7846,
           a_reordered_named_argument_is_still_admitted_22_5) {
    expectStdout("    var s := \"hello\";\n"
                 "    Console.WriteLine(\"[\" + s.PadLeft(paddingChar := '.', totalWidth := 8) + \"]\");\n",
                 "[...hello]");
}
