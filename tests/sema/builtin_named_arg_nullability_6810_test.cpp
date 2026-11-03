// Spec-derived regression test for issue #6810.
//
// spec §7 (v0.350) "the parameter pass" and §7.3 (v0.350) "Assigning `null`
// to a non-nullable target is a compile error";
// spec §22.5 (v0.350) "Optional parameters and named arguments";
// stdlib §3.1 (v0.350) `StringBuilder`.
//
// THE DEFECT. The §7 nullability gate for a built-in member
// (`ExprTC::checkBuiltinNullableArgs`, src/sema/ExprTypecheck_Calls.cpp) read
// the parameter POSITION by the ARGUMENT'S INDEX, so it SKIPPED every named
// argument wherever the compiler HONOURS the name. The POSITIONAL spelling of
// a call was refused and the NAMED spelling of the SAME overload was
// accepted:
//
//   sb.Append(null)          -> "the argument to `StringBuilder.Append` is
//                                nullable but the target type is non-nullable
//                                (§7)"                        [REFUSED]
//   sb.Append(value := null) -> compiled clean, ran, exit 0  [ACCEPTED]
//
// stdlib §3.1 declares `Append(value: string): StringBuilder` and `value` is
// NOT nullable, so the two spellings of ONE overload disagreed in ADMISSION
// and the `null` reached the runtime unchecked. `AppendLine(value: string)`
// and `Replace(oldValue: string, newValue: string)` carried the SAME hole.
//
// The #7982 fix closed the hole for the `File` / `Directory` heads and the
// #7995 fix for the `Path` head, by listing those heads as "the heads whose
// NAMED argument is checked at the POSITION THE NAME BINDS TO". The list was
// the wrong SHAPE: the rule is a property of the BINDING, not of the head —
// every head whose names the compiler honours binds a named argument to the
// parameter it names, so every one of them must be checked at the BOUND
// position. The head list is gone.
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

// A rejection row: the program must be REJECTED at compile time, with the
// named diagnostic, at a real source location.
void expectRejected(const std::string& body, const std::string& needle) {
    auto r = runAot(body);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// A control row: the program must compile, run clean under the strict AOT
// oracle, and print exactly `out`.
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

// The §7 nullability refusal, for one member.
std::string nullableArg(const std::string& member) {
    return "the argument to `" + member + "` is nullable but the target type "
           "is non-nullable (§7)";
}

const char kTextImport[] = "import System.Text;\n";

}  // namespace

// ── the `StringBuilder` head: the NAMED spelling was ACCEPTED ──────────────

// stdlib §3.1 declares `Append(value: string): StringBuilder`. The named
// spelling of the `null` literal was ACCEPTED (the base compiled it clean
// and the program ran to completion) where the positional spelling is
// refused.
BEZEL_TEST(builtin_named_arg_nullability_6810,
           stringbuilder_append_named_null_is_refused) {
    expectRejected(progWithImports(kTextImport,
                        "var sb := new StringBuilder(); sb.Append(value := null);"),
                   nullableArg("StringBuilder.Append"));
}

// `AppendLine(value: string)` — the same hole, the same §3.1 declaration.
BEZEL_TEST(builtin_named_arg_nullability_6810,
           stringbuilder_append_line_named_null_is_refused) {
    expectRejected(progWithImports(kTextImport,
                        "var sb := new StringBuilder(); sb.AppendLine(value := null);"),
                   nullableArg("StringBuilder.AppendLine"));
}

// `Replace(oldValue: string, newValue: string)` — the hole at the FIRST
// position of a two-parameter member.
BEZEL_TEST(builtin_named_arg_nullability_6810,
           stringbuilder_replace_named_null_is_refused) {
    expectRejected(progWithImports(kTextImport,
                        "var sb := new StringBuilder(); "
                        "sb.Replace(oldValue := null, newValue := \"x\");"),
                   nullableArg("StringBuilder.Replace"));
}

// CONTROL (green on the base): the POSITIONAL spelling of the same call was
// already refused — the named spelling did not become a way around §7.
BEZEL_TEST(builtin_named_arg_nullability_6810,
           stringbuilder_append_positional_null_is_refused) {
    expectRejected(progWithImports(kTextImport,
                        "var sb := new StringBuilder(); sb.Append(null);"),
                   nullableArg("StringBuilder.Append"));
}

// CONTROL: the named spelling of a LEGAL call still compiles and runs.
BEZEL_TEST(builtin_named_arg_nullability_6810,
           stringbuilder_append_named_value_is_admitted) {
    expectStdout(progWithImports(kTextImport,
                      "var sb := new StringBuilder(); sb.Append(value := \"ab\"); "
                      "Console.WriteLine(sb.ToString());"),
                 "ab\n");
}

// ── CONTROLS: the heads the #7982 / #7995 fixes already closed ─────────────

// stdlib §7.3 declares `File.Exists(path: string)` and `path` is NOT
// nullable. GREEN on the base (#7982) — the regression guard for the
// head-list shape this fix replaces.
BEZEL_TEST(builtin_named_arg_nullability_6810,
           file_exists_named_null_is_refused) {
    expectRejected(prog("Console.WriteLine(File.Exists(path := null));"),
                   nullableArg("File.Exists"));
}

// stdlib §7.4 declares `Path.Combine(a: string, b: string)`. GREEN on the
// base (#7995).
BEZEL_TEST(builtin_named_arg_nullability_6810,
           path_combine_named_null_is_refused) {
    expectRejected(prog("var p := Path.Combine(a := null, b := \"x\");"),
                   nullableArg("Path.Combine"));
}

// stdlib §2.3 declares `Convert.ToInt32(value: string)`. The `Math` /
// `Convert` heads read the BOUND position already (`checkBuiltinMathConvert-
// CallArgs`), so this row is GREEN on the base — the regression guard.
BEZEL_TEST(builtin_named_arg_nullability_6810,
           convert_toint32_named_null_is_refused) {
    expectRejected(prog("var v := Convert.ToInt32(value := null);"),
                   nullableArg("Convert.ToInt32"));
}

// stdlib §2.2 declares `Math.Abs(x: int)`. GREEN on the base — the same
// regression guard on the `Math` head.
BEZEL_TEST(builtin_named_arg_nullability_6810,
           math_abs_named_null_is_refused) {
    expectRejected(prog("var v := Math.Abs(x := null);"),
                   nullableArg("Math.Abs"));
}

// CONTROL: the named spelling of a LEGAL `Math` / `Convert` call still
// compiles and runs.
BEZEL_TEST(builtin_named_arg_nullability_6810,
           math_abs_named_value_is_admitted) {
    expectStdout(prog("Console.WriteLine(Math.Abs(x := -42));"), "42\n");
}
