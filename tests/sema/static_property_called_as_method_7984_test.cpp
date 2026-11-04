// Spec-derived regression test for issue #7984 -- a STATIC property of a
// BUILT-IN head called as a METHOD (`Environment.NewLine(1)`) compiled, and
// codegen then degraded the un-lowerable callee (the internal #2434
// zero-initializer / the #3079 "failed to lower" warning) behind exit 0.
//
// THE SPEC.
//
// spec §26.5 rule 1 (v0.355): "If `x`'s declared type (or any base type)
// *declares* any instance member named `Foo` -- of any arity, applicability, or
// accessibility -- instance resolution is committed and extension blocks are
// **never consulted** for that name on that receiver. If the committed
// instance lookup then fails (no applicable overload, §22.5/§22.6, or the
// member is inaccessible, §13.1), the call is a compile error citing the
// instance member(s)."
//
// spec §27.1 (v0.355) admits only a FUNCTION-typed value as a callee -- "a
// parameter, local, field, or property of function type" -- and §27.7 (v0.355)
// is the compiler's "only function-typed values are callable" rule.
//
// A STATIC property is the same shape: stdlib §2.4 declares
// `public static readonly property NewLine: string;` on `Environment`, so
// `Environment.NewLine(1)` is a call through the property's VALUE -- a
// `string`, not a function -- and it has no candidate.
//
// THE DEFECT -- MEASURED on the base tip (r30-c1 e5ad8c2f, spec v0.355):
//
//   Console.WriteLine(Environment.NewLine(1));  -> COMPILED, exit 0, with the
//                                                  #3079 "failed to lower"
//                                                  warning
//   var d := DateTime.UtcNow();                 -> COMPILED, exit 0
//
// The rejection rows assert the EXACT diagnostic text, at the call's own
// source line, and assert that the program did not run and that the driver
// reported exactly ONE error (no cascade).

#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The body always sits on line 4.
std::string prog(const std::string& body) {
    return "module M; import System;\n"    // 1
           "class C\n"                       // 2
           "  public static method Main(): int\n"  // 3
           "    " + body + "\n"             // 4
           "    return 0;\n"                 // 5
           "  end Main;\n"                   // 6
           "end C;\n";                       // 7
}

// One `<path>:<line>:<col>: <text>` line of the driver's captured stderr.
struct DiagLine {
    std::uint32_t line = 0;
    std::string text;
};

// The AOT oracle packs the driver's ENTIRE captured stderr into ONE
// `Diagnostic` (a genuine compile reject), whose message is the driver's
// `<path>:<line>:<col>: <text>` diagnostic lines, one per line. This splits
// that blob back into its lines.
std::vector<DiagLine> diagnosticLines(const JitResult& r) {
    std::vector<DiagLine> out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::istringstream in(d.message);
        std::string raw;
        while (std::getline(in, raw)) {
            const std::size_t c1 = raw.find(':');
            if (c1 == std::string::npos) continue;
            const std::size_t c2 = raw.find(':', c1 + 1);
            if (c2 == std::string::npos) continue;
            const std::size_t c3 = raw.find(':', c2 + 1);
            if (c3 == std::string::npos) continue;
            DiagLine dl;
            dl.line = static_cast<std::uint32_t>(std::strtoul(
                raw.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 10));
            dl.text = raw.substr(c3 + 1);
            if (!dl.text.empty() && dl.text[0] == ' ') dl.text.erase(0, 1);
            out.push_back(dl);
        }
    }
    return out;
}

// A warning / note line carries its severity as a prefix; an error line does
// not.
bool isErrorLine(const DiagLine& dl) {
    return dl.text.rfind("warning: ", 0) != 0 && dl.text.rfind("note: ", 0) != 0;
}

// The EXACT text of the error diagnostic the driver reported at `line`.
std::string errorTextAtLine(const JitResult& r, std::uint32_t line) {
    for (const auto& dl : diagnosticLines(r))
        if (dl.line == line && isErrorLine(dl)) return dl.text;
    return std::string();
}

// How many ERROR diagnostics the driver reported in all.
std::size_t errorLineCount(const JitResult& r) {
    std::size_t n = 0;
    for (const auto& dl : diagnosticLines(r))
        if (isErrorLine(dl)) ++n;
    return n;
}

// The rejection oracle: the program did NOT run, the driver rejected it at a
// REAL source location (never a crash / signal / timeout), the diagnostic on
// `line` reads EXACTLY as expected, and it is the ONLY error (a fix that
// produced a cascade would fail here).
void expectRejectedWith(const std::string& program, std::uint32_t line,
                        const std::string& expected) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_EQ(errorTextAtLine(r, line), expected);
    BEZEL_EXPECT_EQ(errorLineCount(r), std::size_t{1});
}

// The EXACT diagnostic the fix introduces for a BUILT-IN property.
std::string builtinPropertyCallMessage(const std::string& member,
                                       const std::string& owner,
                                       const std::string& section,
                                       const std::string& type) {
    return "`" + member + "` is a property of `" + owner +
           "`, not a method (stdlib \xC2\xA7" + section + ": `" + member +
           ": " + type +
           "`), so this call has no candidate: the lookup commits to the "
           "instance member (\xC2\xA7""26.5 rule 1), and only a "
           "function-typed property can be called (\xC2\xA7""27.1, "
           "\xC2\xA7""27.7) \xE2\x80\x94"" read it without parentheses, `." +
           member + "`";
}

// A program that compiles and runs.
void expectRuns(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A program that compiles, runs, and prints exactly `out`.
void expectStdout(const std::string& program, const std::string& out) {
    auto r = runAot(program);
    if (!r.ran) {
        std::cout << "    #7984: program did not run\n";
        for (const auto& d : r.diagnostics.diagnostics())
            std::cout << "    " << d.message << "\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// ===========================================================================
// #7984 -- a BUILT-IN STATIC property called as a method.
// ===========================================================================

// The issue's own repro: `Environment.NewLine(1)`. Stdlib §2.4 declares
// `public static readonly property NewLine: string;`.
BEZEL_TEST(static_property_called_as_method_7984,
           environment_newline_call_with_an_argument_is_rejected_stdlib_2_4) {
    expectRejectedWith(
        prog("Console.WriteLine(Environment.NewLine(1));"), 4,
        builtinPropertyCallMessage("NewLine", "Environment", "2.4", "string"));
}

// The arity-0 spelling is the same shape.
BEZEL_TEST(static_property_called_as_method_7984,
           environment_newline_call_is_rejected_stdlib_2_4) {
    expectRejectedWith(
        prog("Console.WriteLine(Environment.NewLine());"), 4,
        builtinPropertyCallMessage("NewLine", "Environment", "2.4", "string"));
}

// `DateTime.UtcNow` -- stdlib §2.5 declares `public static readonly property
// UtcNow: DateTime;`.
BEZEL_TEST(static_property_called_as_method_7984,
           datetime_utcnow_call_is_rejected_stdlib_2_5) {
    expectRejectedWith(
        prog("var d := DateTime.UtcNow();"), 4,
        builtinPropertyCallMessage("UtcNow", "DateTime", "2.5", "DateTime"));
}

// ===========================================================================
// CONTROLS: what must still compile.
// ===========================================================================

// The property READ is the spelling stdlib §2.4 declares, and it still reads
// the value.
BEZEL_TEST(static_property_called_as_method_7984,
           environment_newline_read_still_compiles_stdlib_2_4) {
    expectRuns(prog("var s := Environment.NewLine; Console.WriteLine(s.Length.ToString());"));
}

// A USER class that reuses a built-in's name is not the built-in: its own
// declared surface answers, so a METHOD of that name is still callable.
BEZEL_TEST(static_property_called_as_method_7984,
           a_user_class_named_like_a_builtin_keeps_its_own_static_method) {
    expectStdout(
        "module M; import System;\n"          // 1
        "class Environment\n"                 // 2
        "  public static method NewLine(): string => \"mine\";\n"  // 3
        "end Environment;\n"                  // 4
        "class C\n"                           // 5
        "  public static method Main(): int\n"  // 6
        "    Console.WriteLine(Environment.NewLine());\n"  // 7
        "    return 0;\n"                      // 8
        "  end Main;\n"                        // 9
        "end C;\n",                            // 10
        "mine\n");
}
