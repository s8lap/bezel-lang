// Spec-derived regression test for issue #8045.
//
// spec §22.5 (v0.353) "Optional parameters and named arguments";
// spec §3.4 (v0.353) "Keyword aliases for built-in types";
// stdlib §2.4 (v0.353) `Environment`; stdlib §1 (v0.353) "Conventions"
// ("parameter names are API surface").
//
// THE DEFECT. The built-in named-argument binder (`ExprTC::checkBuiltinNamedArgs`)
// CLAIMS a built-in call through `common::isBuiltinNamedArgTableHead`, and
// where it claims, a parameter NAME written at the call is matched (or
// refused); where it does NOT claim, the name is neither matched nor refused
// and the arguments bind POSITIONALLY (codegen lowers a built-in call by
// position — §22.5, #3423).
//
// The claim set was a HAND-WRITTEN list of NINE heads, while the ONE built-in
// parameter table (`kBuiltinParams`, src/common/BuiltinRegistry.cpp) declares a
// NAMED parameter for SIXTY-EIGHT heads (measured on this tree: 384 rows over
// 69 distinct heads, 68 of them with at least one declared parameter name —
// `TextWriter` is the one head with none). So at the other heads a REORDERED
// name was SILENTLY IGNORED and the arguments bound POSITIONALLY:
//
//   Environment.Set(value := "two", name := "BEZEL_PROBE_VAR")
//     -> set the environment variable `two` to "BEZEL_PROBE_VAR"
//
// and an UNKNOWN name was silently ignored there too (#7622's rule reaches
// only the claimed heads).
//
// THE FIX. The claim set is now COMPUTED from the ONE table — a head is a
// named-argument table head exactly when the table carries a row for it whose
// declared parameter names are non-empty — so it cannot drift from the table,
// and the value built-ins (whose §22.5 surface the shared built-in resolver
// `ExprTC::checkBuiltinCallAdmission` already owns) stay with their owner.
//
// THE ORACLE is the program's OUTPUT — the exact stdout of the environment
// variable read back — plus the exact diagnostic text for the refusal rows,
// never the absence of a diagnostic.
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

// The §22.5 refusal for a name the member does NOT declare.
std::string unknownName(const std::string& callee, const std::string& name) {
    return "call to built-in `" + callee + "` has no parameter named `" + name +
           "` (§22.5)";
}

}  // namespace

// stdlib §2.4 declares `Set(name: string, value: string?)` — the two
// DECLARED names. The base compiled this call clean, bound the arguments
// POSITIONALLY, and set the environment variable `two` to "BEZEL_PROBE_VAR";
// the variable the call NAMES kept its earlier value.
//
// The oracle is the exact stdout of the read-back: "two" after the fix,
// "one" before it.
BEZEL_TEST(builtin_named_arg_table_heads_8045,
           environment_set_with_a_reordered_name_binds_by_name) {
    expectStdout(
        prog("Environment.Set(name := \"BEZEL_PROBE_VAR\", value := \"one\");\n"
             "    Environment.Set(value := \"two\", name := \"BEZEL_PROBE_VAR\");\n"
             "    Console.WriteLine(Environment.Get(\"BEZEL_PROBE_VAR\"));"),
        "two\n");
}

// THE ENV-VAR EFFECT of the same call: the variable the call NAMES carries
// the reordered value, and the variable the POSITIONAL reading would have
// written is NOT written at all. The base set `BEZEL_8045_MISBOUND` and left
// `BEZEL_8045_PROBE` unset.
BEZEL_TEST(builtin_named_arg_table_heads_8045,
           environment_set_with_a_reordered_name_writes_only_the_named_variable) {
    expectStdout(
        prog("Environment.Set(value := \"BEZEL_8045_MISBOUND\", "
             "name := \"BEZEL_8045_PROBE\");\n"
             "    Console.WriteLine(Environment.Get(\"BEZEL_8045_PROBE\") "
             "?? \"<unset>\");\n"
             "    Console.WriteLine(Environment.Get(\"BEZEL_8045_MISBOUND\") "
             "?? \"<unset>\");"),
        "BEZEL_8045_MISBOUND\n<unset>\n");
}

// #7622's rule reaches the newly claimed heads: an UNKNOWN name at
// `Environment.Set` is REFUSED, not silently ignored.
BEZEL_TEST(builtin_named_arg_table_heads_8045,
           environment_set_with_an_unknown_name_is_refused) {
    expectRejected(
        prog("Environment.Set(bogus := \"x\", value := \"y\");"),
        unknownName("Environment.Set", "bogus"));
}

// The same rule at a one-parameter member of the same head.
BEZEL_TEST(builtin_named_arg_table_heads_8045,
           environment_get_with_an_unknown_name_is_refused) {
    expectRejected(prog("var v := Environment.Get(bogus := \"X\");"),
                   unknownName("Environment.Get", "bogus"));
}

// CONTROL: the DECLARED name, in DECLARATION order, binds as it always did.
BEZEL_TEST(builtin_named_arg_table_heads_8045,
           environment_set_in_declaration_order_is_unchanged) {
    expectStdout(
        prog("Environment.Set(name := \"BEZEL_8045_ORDER\", value := \"one\");\n"
             "    Console.WriteLine(Environment.Get(\"BEZEL_8045_ORDER\"));"),
        "one\n");
}

// CONTROL: the POSITIONAL spelling is untouched.
BEZEL_TEST(builtin_named_arg_table_heads_8045,
           environment_set_positionally_is_unchanged) {
    expectStdout(
        prog("Environment.Set(\"BEZEL_8045_POS\", \"one\");\n"
             "    Console.WriteLine(Environment.Get(\"BEZEL_8045_POS\"));"),
        "one\n");
}

// CONTROL: a head the binder already claimed keeps its behaviour — the
// `string` head's §9.6 surface (`PadLeft(totalWidth: int, paddingChar: char
// := ' ')`), whose DECLARED name is still honoured.
BEZEL_TEST(builtin_named_arg_table_heads_8045,
           string_padleft_with_its_declared_name_is_unchanged) {
    expectStdout(prog("Console.WriteLine(\"ab\".PadLeft(totalWidth := 6));"),
                 "    ab\n");
}
