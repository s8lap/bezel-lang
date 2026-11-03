// Issue #7165 (spec §22.5, v0.343; stdlib §9.6, v0.343) -- a REORDERED
// named-argument call to a built-in `string` member binds each argument to
// the PARAMETER it names, and produces the RIGHT VALUE.
//
// THE GOVERNING TEXT. spec §22.5 (v0.343): "named arguments may appear in
// any order after positional arguments. Each supplied argument binds exactly
// one parameter", and "naming an argument changes which parameter it binds
// to, never when it runs". stdlib §9.6 (v0.343) declares the string members
// with NAMED parameters -- for example
//   public pure method Substring(startIndex: int, length: int): string;
//   public pure method Replace(oldValue: string, newValue: string): string;
//   public static method Join(separator: string, items: IEnumerable<string>): string;
// so `startIndex` is the FIRST parameter and `length` the SECOND, and a
// reordered call must bind by NAME, not by position.
//
// THE DEFECT. The built-in `string` arms did not read the binding sema
// PUBLISHES (the `paramToArg` vector, PARAMETER index -> SOURCE argument
// index). They took their operands POSITIONALLY -- `call.args[0]`,
// `call.args[1]` -- so a reordered named call handed each parameter another
// parameter's value. Where the two parameters have the SAME type the call
// still compiled and RAN, and computed the WRONG VALUE, silently: the issue's
// own repro `s.Substring(length := 2, startIndex := 1)` printed `c` where it
// must print `bc`, and `s.Replace(newValue := "X", oldValue := "b")` printed
// `abcdef` where it must print `aXcdef`. Where the two parameters have
// DIFFERENT types the front end refused the call, and where the arm lowered
// the mismatched operands the compiler aborted with "IR verification
// failed".
//
// THE ISSUE'S OWN REPROS. The `string` INSTANCE arm (`Substring`) and the
// `string` STATIC arm (`string.Join`), each written with its named arguments
// in the REVERSED order.
//
// THE FIX LANDED ELSEWHERE (before this test existed). The defect was fixed
// by TWO earlier commits, and this commit names both, because the issue's
// two halves are two arms:
//
//   * commit e19de7506 ("narrow #7165: the static string.Join arm binds a
//     named argument to the PARAMETER it names", 2026-10-25) -- the STATIC
//     `string.Join` arm of `Emitter::tryLowerBuiltinClassCall`;
//   * commit 40d17acd ("narrow #7180: the `string` instance arms bind a named
//     argument to the PARAMETER it names", 2026-10-25, LATER) -- the
//     INSTANCE arms.
//
// `40d17acd` is the later of the two, and its PARENT is the tree on which
// BOTH halves of the issue are still broken -- so that parent is the RED arm
// this commit's fail-without is measured on. Nothing closed the issue, so it
// stayed open on a tree that already behaves correctly.
//
// ORACLE. `runAot` (the AOT path). Every row asserts the EXACT STDOUT -- a
// VALUE oracle, never "no diagnostic was produced". The control row is the
// DECLARATION-ORDER spelling of the same two members, which publishes the
// identity permutation and was always correct.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;
using bezel::test::runAot;

std::string prog(const std::string& body) {
    return "module M;\n"
           "import System;\n"
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

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

// =====================================================================
// ROW 1 -- the issue's own repro, the INSTANCE arm: `Substring/2`, whose
// declared parameters are `{startIndex: int, length: int}`. The two
// parameters have the SAME type, so the positional reading compiled and RAN
// and computed the WRONG VALUE.
//
// Pre-fix: printed "c" (the length and the start index swapped).
// Post-fix: prints "bc" -- the 2 code points starting at position 1.
// =====================================================================
BEZEL_TEST(string_reordered_named_argument_binds_7165,
           substring_binds_each_named_argument_to_its_parameter_spec_22_5) {
    expectStdout("    var s: string := \"abcdef\";\n"
                 "    Console.WriteLine(s.Substring(length := 2, "
                 "startIndex := 1));\n",
                 "bc");
}

// =====================================================================
// ROW 2 -- the second same-typed instance arm: `Replace/2`, whose declared
// parameters are `{oldValue: string, newValue: string}`. A wrong binding
// here is a wrong VALUE, not a refusal.
//
// Pre-fix: printed "abcdef" (nothing was replaced -- the search value was
//           the replacement and the replacement was the search value).
// Post-fix: prints "aXcdef".
// =====================================================================
BEZEL_TEST(string_reordered_named_argument_binds_7165,
           replace_binds_each_named_argument_to_its_parameter_spec_22_5) {
    expectStdout("    var s: string := \"abcdef\";\n"
                 "    Console.WriteLine(s.Replace(newValue := \"X\", "
                 "oldValue := \"b\"));\n",
                 "aXcdef");
}

// =====================================================================
// ROW 3 -- the STATIC arm: `string.Join(items := parts, separator := "-")`,
// whose declared parameters are `{separator: string, items:
// IEnumerable<string>}`.
//
// Pre-fix: the separator parameter received the ITEMS argument and vice
//           versa.
// Post-fix: prints "a-b".
// =====================================================================
BEZEL_TEST(string_reordered_named_argument_binds_7165,
           the_static_join_arm_binds_each_named_argument_to_its_parameter) {
    expectStdout("    var parts: Array<string> := new Array<string>();\n"
                 "    parts.Add(\"a\");\n"
                 "    parts.Add(\"b\");\n"
                 "    Console.WriteLine(string.Join(items := parts, "
                 "separator := \"-\"));\n",
                 "a-b");
}

// =====================================================================
// CONTROL -- the DECLARATION-ORDER spellings of the same two instance
// members, which publish the IDENTITY permutation and were always correct.
// A VALUE oracle, green on both arms: it pins that the reordered spellings
// changed nothing about the positional ones.
//
// Pre-fix and post-fix: "bc" and "aXcdef".
// =====================================================================
BEZEL_TEST(string_reordered_named_argument_binds_7165,
           the_declaration_order_spellings_are_unchanged_spec_22_5) {
    expectStdout("    var s: string := \"abcdef\";\n"
                 "    Console.WriteLine(s.Substring(1, 2));\n"
                 "    Console.WriteLine(s.Replace(\"b\", \"X\"));\n",
                 "bc\naXcdef");
}
