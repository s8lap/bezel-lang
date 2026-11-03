// Spec-derived regression test for issue #8046.
//
// spec §3.4 (v0.353) "Keyword aliases for built-in types";
// spec §22.5 (v0.353) "Optional parameters and named arguments";
// spec §5.1 (v0.353) "Implicit conversions";
// stdlib §9.6 (v0.353) `string`.
//
// THE DEFECT. The §9.6 declared-parameter gate
// (`ExprTC::checkBuiltinStringDeclaredParams`) claims the §9.6 `String` head
// in TWO spellings — the primitive keyword `string` and its §3.4 PascalCase
// alias `String` — and its own header says so. But the ALIAS branch claimed
// the head only when the target's symbol was a `TypeSymbol` with
// `decl == nullptr` (the compiler-intrinsic `builtinClassSym` shape), and the
// §3.4 primitive alias is NOT such a symbol. So the whole gate — the §5.1
// type check, the §7.3 nullability check and #7535's candidate narrowing —
// STOOD DOWN at the alias spelling:
//
//   String.Repeat(c := "ab", count := 2)   -> compiled; printed "abab"
//   string.Repeat(c := "ab", count := 2)   -> COMPILE-REFUSED
//   String.Repeat(unit := 5, count := 2)   -> compiled CLEAN
//   string.Repeat(unit := 5, count := 2)   -> REFUSED
//
// The first pair is #7535's own defect at the alias (stdlib §9.6 declares
// `Repeat(c: char, count: int): string` and `Repeat(unit: string, count:
// int): string`, so the name `c` selects the CHAR overload and a `string` is
// not a `char` — §5.1); the second pair is a §5.1 hole beside it (an `int` at
// a `string` parameter).
//
// THE ORACLE is the exact diagnostic text (plus a real source location) for
// the refusal rows and the exact stdout for the accept rows — never the
// absence of a diagnostic.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

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
    auto r = runAot(body);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

std::string prog(const std::string& body) {
    return "module M;\n"
           "import System;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The §5.1 refusal of the §9.6 declared-parameter gate: the gate names the
// DECLARED FORMS, the PARAMETER (§22.5's declared name), the DECLARED TYPE
// at that position and the argument's own type.
std::string mustBe(const std::string& param, const std::string& type,
                   const std::string& argType) {
    return "no matching overload of `string.Repeat` — stdlib §9.6 declares "
           "`Repeat(c: char, count: int): string` / `Repeat(unit: string, "
           "count: int): string`, so the `" + param + "` argument must be a " +
           type + " — the argument has type `" + argType +
           "`, which is never implicitly convertible to it (§5.1)";
}

}  // namespace

// The ALIAS hole: `String.Repeat(c := "ab", count := 2)` — the name `c`
// selects stdlib §9.6's CHAR overload, and a `string` is not a `char` (§5.1).
// The base compiled this clean and printed "abab".
BEZEL_TEST(builtin_string_declared_params_alias_8046,
           string_alias_repeat_with_a_string_at_the_char_parameter_is_refused) {
    expectRejected(
        prog("Console.WriteLine(String.Repeat(c := \"ab\", count := 2));"),
        mustBe("c", "`char`", "string"));
}

// The §5.1 hole beside it: an `int` at the `string` parameter of the STRING
// overload. The base compiled this CLEAN.
BEZEL_TEST(builtin_string_declared_params_alias_8046,
           string_alias_repeat_with_an_int_at_the_string_parameter_is_refused) {
    expectRejected(
        prog("Console.WriteLine(String.Repeat(unit := 5, count := 2));"),
        mustBe("unit", "`string`", "int"));
}

// CONTROL: the admitting twin — the LOWERCASE spelling keeps refusing the
// same two calls, with the same words.
BEZEL_TEST(builtin_string_declared_params_alias_8046,
           lowercase_repeat_with_a_string_at_the_char_parameter_is_still_refused) {
    expectRejected(
        prog("Console.WriteLine(string.Repeat(c := \"ab\", count := 2));"),
        mustBe("c", "`char`", "string"));
}

// CONTROL: the lowercase spelling keeps refusing the §5.1 hole too.
BEZEL_TEST(builtin_string_declared_params_alias_8046,
           lowercase_repeat_with_an_int_at_the_string_parameter_is_still_refused) {
    expectRejected(
        prog("Console.WriteLine(string.Repeat(unit := 5, count := 2));"),
        mustBe("unit", "`string`", "int"));
}

// CONTROL: the GOOD call still works through the ALIAS — the STRING
// overload's own parameter name admits a `string`.
BEZEL_TEST(builtin_string_declared_params_alias_8046,
           string_alias_repeat_with_the_declared_string_parameter_is_admitted) {
    expectStdout(
        prog("Console.WriteLine(String.Repeat(unit := \"ab\", count := 2));"),
        "abab\n");
}

// CONTROL: the GOOD call still works through the lowercase spelling.
BEZEL_TEST(builtin_string_declared_params_alias_8046,
           lowercase_repeat_with_the_declared_string_parameter_is_admitted) {
    expectStdout(
        prog("Console.WriteLine(string.Repeat(unit := \"ab\", count := 2));"),
        "abab\n");
}

// CONTROL: the CHAR overload's own parameter name admits a `char`, through the
// alias as well.
BEZEL_TEST(builtin_string_declared_params_alias_8046,
           string_alias_repeat_with_the_declared_char_parameter_is_admitted) {
    expectStdout(
        prog("Console.WriteLine(String.Repeat(c := 'a', count := 3));"),
        "aaa\n");
}
