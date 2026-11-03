// Spec-derived regression test for issue #7535.
//
// spec §22.5 (v0.350) "Optional parameters and named arguments";
// spec §5.1 (v0.350) "Implicit conversions";
// stdlib §9.6 (v0.350) `string.Repeat`.
//
// THE DEFECT. stdlib §9.6 declares TWO static overloads:
//
//   Repeat(c: char, count: int): string
//   Repeat(unit: string, count: int): string
//
// §22.5 (v0.350) resolves a named argument PER OVERLOAD: the argument binds
// the parameter it NAMES, so the name `c` selects the CHAR overload and the
// name `unit` selects the STRING overload. The §9.6 declared-parameter gate
// (`ExprTC::checkBuiltinStringDeclaredParams`) took the UNION of the declared
// types over every declared overload of the argument count, so the NAME and
// the TYPE could come from DIFFERENT overloads:
//
//   string.Repeat(c := "ab", count := 2)
//     -> compiled clean and printed "abab"
//
// The name `c` belongs to the CHAR overload (whose `c` is a `char`) and the
// `string` argument belongs to the STRING overload (whose first parameter is
// named `unit`). No declared overload matches both the name and the type, so
// the call must be refused.
//
// THE ORACLE is the diagnostic text plus a real source location
// (`failedAtCompileTimeWithRealLocation`), never the absence of a diagnostic.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

void expectRejected(const std::string& body, const std::string& needle) {
    auto r = runAot(body);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

void expectStdout(const std::string& body, const std::string& out) {
    auto r = runAotChecked(body);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

std::string prog(const std::string& body) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// The name `c` selects the CHAR overload; a `string` is not a `char` (§5.1).
// The base compiled this clean and printed "abab".
BEZEL_TEST(builtin_named_arg_overload_7535,
           string_repeat_named_char_parameter_with_a_string_argument_is_refused) {
    expectRejected(prog("Console.WriteLine(string.Repeat(c := \"ab\", count := 2));"),
                   "so the `c` argument must be a `char`");
}

// The same shape with the count named first — the ORDER does not change the
// overload the name selects.
BEZEL_TEST(builtin_named_arg_overload_7535,
           string_repeat_named_char_parameter_with_a_string_argument_is_refused_in_any_order) {
    expectRejected(prog("Console.WriteLine(string.Repeat(count := 2, c := \"ab\"));"),
                   "so the `c` argument must be a `char`");
}

// CONTROL: the STRING overload's own parameter name admits a `string`.
BEZEL_TEST(builtin_named_arg_overload_7535,
           string_repeat_named_unit_parameter_with_a_string_argument_is_admitted) {
    expectStdout(prog("Console.WriteLine(string.Repeat(unit := \"ab\", count := 2));"),
                 "abab\n");
}

// CONTROL: the CHAR overload's own parameter name admits a `char`.
BEZEL_TEST(builtin_named_arg_overload_7535,
           string_repeat_named_char_parameter_with_a_char_argument_is_admitted) {
    expectStdout(prog("Console.WriteLine(string.Repeat(c := 'a', count := 3));"),
                 "aaa\n");
}

// CONTROL: the POSITIONAL spelling is chosen by TYPE (§22.6's applicability),
// so a `string` first argument is the STRING overload.
BEZEL_TEST(builtin_named_arg_overload_7535,
           string_repeat_positional_string_argument_is_admitted) {
    expectStdout(prog("Console.WriteLine(string.Repeat(\"ab\", 2));"), "abab\n");
}

// CONTROL: the POSITIONAL spelling of a `char` first argument is the CHAR
// overload.
BEZEL_TEST(builtin_named_arg_overload_7535,
           string_repeat_positional_char_argument_is_admitted) {
    expectStdout(prog("Console.WriteLine(string.Repeat('a', 3));"), "aaa\n");
}
