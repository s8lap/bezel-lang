// Spec-derived regression test for issue #7622.
//
// spec §22.5 (v0.350) "Optional parameters and named arguments";
// stdlib §9.1 (v0.350) `int.Parse`, §9.2 (v0.350) `Float.Parse` /
// `Float.IeeeEquals`, §2.2 (v0.350) `Math`, §8.2 (v0.350) `Debug`.
//
// THE DEFECT. A named argument must name a parameter of the callee
// (§22.5, v0.350). The §22.5 binder (`ExprTC::checkBuiltinNamedArgs`)
// claims only the heads whose names the compiler honours — the `string` /
// `StringBuilder` / `Math` / `Convert` / `Encoding` / `Path` / `File` /
// `Directory` family — and the VALUE built-ins (`int`, `Float`, `Guid`,
// `DateTime`, `TimeSpan`, `Debug`, …) are NOT among them, so nothing
// checked the name written there and the call was ADMITTED with the name
// silently ignored:
//
//   int.Parse(s := "ff", bogus := 1)
//     -> compiled clean, ran, and threw ArgumentException at RUNTIME
//
// The shared built-in resolver (`ExprTC::checkBuiltinCallAdmission`) is the
// gate that owns these heads, so the refusal belongs there: a name no
// declared form of the member declares is a compile error.
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

std::string progWithImports(const std::string& imports, const std::string& body) {
    return "module M;\n" + imports +
           "class C\n"
           "  public static method Main(): int\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

std::string prog(const std::string& body) {
    return progWithImports("", body);
}

// The §22.5 refusal for an unknown parameter name.
std::string unknownName(const std::string& callee, const std::string& name) {
    return "call to built-in `" + callee + "` has no parameter named `" + name +
           "` (§22.5)";
}

}  // namespace

// stdlib §9.1 declares `Parse(s: string, radix: int): int` — `bogus` is no
// parameter of it. The base compiled the call clean and threw
// ArgumentException at RUNTIME.
BEZEL_TEST(builtin_named_arg_unknown_name_7622,
           int_parse_with_an_unknown_parameter_name_is_refused) {
    expectRejected(prog("var v := int.Parse(s := \"ff\", bogus := 1);"),
                   unknownName("int.Parse", "bogus"));
}

// stdlib §9.2 declares `Parse(s: string): float` — `x` is no parameter of
// it.
BEZEL_TEST(builtin_named_arg_unknown_name_7622,
           float_parse_with_an_unknown_parameter_name_is_refused) {
    expectRejected(prog("var v := float.Parse(x := \"3.0\");"),
                   unknownName("float.Parse", "x"));
}

// stdlib §9.2 declares `IsNaN(value: float): bool` — `x` is no parameter of
// it.
BEZEL_TEST(builtin_named_arg_unknown_name_7622,
           float_isnan_with_an_unknown_parameter_name_is_refused) {
    expectRejected(prog("var b := Float.IsNaN(x := 3.0f);"),
                   unknownName("Float.IsNaN", "x"));
}

// stdlib §9.2 declares `IeeeEquals(a: float, b: float): bool` — `c` is no
// parameter of it (the issue's own example).
BEZEL_TEST(builtin_named_arg_unknown_name_7622,
           float_ieeeequals_with_an_unknown_parameter_name_is_refused) {
    expectRejected(prog("var b := Float.IeeeEquals(a := 1.0f, c := 1.0f);"),
                   unknownName("Float.IeeeEquals", "c"));
}

// stdlib §8.2 declares `WriteLine(message: string)` — `text` is no parameter
// of it.
BEZEL_TEST(builtin_named_arg_unknown_name_7622,
           debug_writeline_with_an_unknown_parameter_name_is_refused) {
    expectRejected(
        progWithImports("import System.Diagnostics;\n",
                        "Debug.WriteLine(text := \"x\");"),
        unknownName("Debug.WriteLine", "text"));
}

// CONTROL: the DECLARED name is accepted, and the call runs.
BEZEL_TEST(builtin_named_arg_unknown_name_7622,
           int_parse_with_the_declared_parameter_name_is_admitted) {
    expectStdout(prog("Console.WriteLine(int.Parse(s := \"255\", radix := 16));"),
                 "597\n");
}

// CONTROL: the declared name of another member is accepted too.
BEZEL_TEST(builtin_named_arg_unknown_name_7622,
           float_isnan_with_the_declared_parameter_name_is_admitted) {
    expectStdout(prog("Console.WriteLine(Float.IsNaN(value := 3.0f));"),
                 "False\n");
}
