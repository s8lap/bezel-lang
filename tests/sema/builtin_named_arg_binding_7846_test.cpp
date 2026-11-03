// Spec-derived regression test for issue #7846.
//
// spec §22.5 (v0.350) "Optional parameters and named arguments";
// spec §5.1 (v0.350) "Implicit conversions";
// stdlib §9.6 (v0.350) the `string` members.
//
// THE DEFECT. The §9.6 string-call gate
// (`ExprTC::checkBuiltinStringCallArgs`, src/sema/ExprTypecheck_Calls.cpp)
// types each argument against the parameter at its own INDEX when the §22.5
// binding does not resolve. A call that binds NO candidate — a parameter
// bound twice, a positional argument after a named one, a name no declared
// overload declares — therefore got its arguments typed against the WRONG
// parameters, and the type check reported a SECOND diagnostic on a call the
// §22.5 binder had already refused:
//
//   "ab".PadLeft(totalWidth := 5, totalWidth := 6)
//     -> "the argument to `string.PadLeft` has a type that is not implicitly
//         convertible to the target type (§5.1)"   [SPURIOUS]
//     -> "call to built-in `string.PadLeft`: parameter `totalWidth` is bound
//         more than once (§22.5)"                   [CORRECT]
//
// The spurious error blamed the `6` for a type mismatch it does not have —
// the identity mapping put it at position 1, the `char` parameter.
//
// THE ORACLE is the DIAGNOSTIC TEXT: a call the §22.5 binder refused must
// draw the §22.5 diagnostic and the spurious §5.1 type error must be
// ABSENT.
//
// The absence is OBSERVABLE, not vacuous: the program was REJECTED, so the
// diagnostics carry the reject text, and the §5.1 sentence is exactly what
// the identity mapping used to add to it. (The row asserts `r.ran` is
// FALSE, so the diagnostics are never empty by construction.)
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

std::string prog(const std::string& body) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The SPURIOUS diagnostic the identity mapping used to add — the sentence
// the §5.1 gate speaks when an argument's type does not fit the parameter it
// was put at.
const std::string kSpuriousTypeError =
    "has a type that is not implicitly convertible to the target type";

// The §22.5 refusal ALONE: the named-argument violation is reported, and
// NO spurious type error rides along with it.
void expectOnlyTheSection225Error(const std::string& body,
                                  const std::string& needle) {
    auto r = runAot(body);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    // The §22.5 diagnostic is the ONE that describes the mistake.
    BEZEL_EXPECT_FALSE(hasError(r, kSpuriousTypeError));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

}  // namespace

// A parameter bound TWICE: the second binding is a §22.5 violation, and the
// type check must not type the arguments against the identity mapping.
BEZEL_TEST(builtin_named_arg_binding_7846,
           string_call_with_a_duplicated_parameter_name_reports_one_error) {
    expectOnlyTheSection225Error(
        prog("Console.WriteLine(\"ab\".PadLeft(totalWidth := 5, totalWidth := 6));"),
        "parameter `totalWidth` is bound more than once (§22.5)");
}

// A POSITIONAL argument after a NAMED one — the other §22.5 violation the
// identity mapping used to answer with a type error.
BEZEL_TEST(builtin_named_arg_binding_7846,
           string_call_with_a_positional_after_a_named_reports_one_error) {
    expectOnlyTheSection225Error(
        prog("Console.WriteLine(\"ab\".PadLeft(totalWidth := 5, 6));"),
        "a positional argument may not follow a named argument (§22.5");
}

// The same shape on a member whose SECOND parameter is a `string` — the
// identity mapping put the `5` at the `string` parameter.
BEZEL_TEST(builtin_named_arg_binding_7846,
           string_insert_with_a_duplicated_parameter_name_reports_one_error) {
    expectOnlyTheSection225Error(
        prog("Console.WriteLine(\"ab\".Insert(value := \"X\", value := \"Y\"));"),
        "parameter `value` is bound more than once (§22.5)");
}

// CONTROL: a LEGAL reordered named call is still admitted and still typed at
// the parameter the name binds — the identity fallback is gone, but the
// BOUND-position typing the #7674 fix established is not.
BEZEL_TEST(builtin_named_arg_binding_7846,
           string_call_with_a_reordered_named_argument_is_admitted) {
    auto r = runAotChecked(prog(
        "Console.WriteLine(\"ab\".PadLeft(paddingChar := 'x', totalWidth := 5));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "xxxab\n");
}
