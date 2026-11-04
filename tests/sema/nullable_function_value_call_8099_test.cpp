// #8099 -- §7.4 (v0.355): "Member access, method calls, and indexing on a
// nullable value are compile errors unless the compiler has narrowed it to
// non-null". §7.6 (v0.355): "A function value of nullable function type may
// be invoked with `?(...)`" -- and §7.6 is the ONLY spelling that invokes a
// nullable function value.
//
// A call through a function VALUE (a local, a parameter, a field, a call
// result, an index result) whose static type is a NULLABLE function type was
// ACCEPTED: `var h: ((int) -> int)? := null; var r: int := h(1);` compiled
// with rc 0 and the program then died with SIGSEGV (exit 139) on the null
// call -- the same false accept the §7.4 member-access gate (#418 / #862)
// already refuses for `h.M`.
//
// Every REJECTION row below asserts the EXACT diagnostic text, at the
// offending call's own source line, and that it is the ONLY error (a fix
// that produced a cascade would fail here). Every CONTROL row asserts that
// the legal spellings stay accepted -- §7.6's `?(...)`, §7.4's narrowing
// check, §7.8's `!`, a non-nullable function value, and §7.7's `??`
// fallback -- and, where the program can run, its exact stdout.
//
// The `?.`-MEMBER spelling (`x?.Cb()`, §7.5) is deliberately NOT covered
// here: §7.5 types a suffix to the right of `?.` at the member's own
// DECLARED type, while the `?.` node carries that type LIFTED to nullable,
// so the node cannot answer "is the MEMBER's declared type nullable". See
// the lane report for the residual.
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

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
// `line` reads EXACTLY as expected, and it is the ONLY error.
void expectRejectedWith(const std::string& program, std::uint32_t line,
                        const std::string& expected) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_EQ(errorTextAtLine(r, line), expected);
    BEZEL_EXPECT_EQ(errorLineCount(r), std::size_t{1});
}

// The accept oracle: the program compiled AND ran, with this exact stdout.
void expectStdout(const std::string& program, const std::string& out) {
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The EXACT §7.4 diagnostic a call through a NULLABLE function value gets.
//
// A SIMPLE VARIABLE (a local or a parameter) is narrowed by a null test or a
// force-unwrap, so the diagnostic names those; every OTHER shape (a field, a
// property, a call result, an index result) is not a narrowing carrier at
// all (§7.4 "Narrowing applies to locals -- and `readonly` fields -- only"),
// so the copy-to-a-local remedy is the one that applies. The wording mirrors
// the §7.4 member-access gate's (ExprTypecheck_Calls.cpp, ~L33324).
std::string simpleVarMessage(const std::string& name) {
    return "call through a nullable function value `" + name +
           "` requires narrowing it to non-null first (§7.4): guard with `if " +
           name + " <> null`, force-unwrap with `" + name +
           "!`, or use the null-conditional call `" + name + "?(...)`";
}

const char* kOtherShapeMessage =
    "call through a nullable function value requires narrowing it to non-null "
    "first (§7.4): copy the nullable value to a local variable and guard with "
    "`<> null`, force-unwrap with `!`, or use the null-conditional call "
    "`?(...)`";

}  // namespace

// ---------------------------------------------------------------------------
// REJECTION rows -- one per callee shape. Each program is written so the
// offending call sits on the line the row asserts.
// ---------------------------------------------------------------------------

// A LOCAL of nullable function type, called directly. The issue's repro.
BEZEL_TEST(nullable_function_value_call_8099,
           local_call_is_refused_spec_7_4) {
    expectRejectedWith(
        "module M;\n"                                            // 1
        "import System;\n"                                       // 2
        "class C\n"                                              // 3
        "  public static method Main(): int\n"                   // 4
        "    var h: ((int) -> int)? := null;\n"                  // 5
        "    var r: int := h(1);\n"                              // 6
        "    return 0;\n"                                        // 7
        "  end Main;\n"                                          // 8
        "end C;\n",                                              // 9
        6, simpleVarMessage("h"));
}

// A PARAMETER of nullable function type, called directly.
BEZEL_TEST(nullable_function_value_call_8099,
           parameter_call_is_refused_spec_7_4) {
    expectRejectedWith(
        "module M;\n"                                            // 1
        "import System;\n"                                       // 2
        "class C\n"                                              // 3
        "  public static method Take(p: ((int) -> int)?): int\n" // 4
        "    return p(1);\n"                                     // 5
        "  end Take;\n"                                          // 6
        "  public static method Main(): int\n"                   // 7
        "    return Take(null);\n"                               // 8
        "  end Main;\n"                                          // 9
        "end C;\n",                                              // 10
        5, simpleVarMessage("p"));
}

// A FIELD of nullable function type, called directly. A field is NOT a
// narrowing carrier (§7.4 "Narrowing applies to locals -- and `readonly`
// fields -- only"), so the copy-to-a-local remedy is the one named.
BEZEL_TEST(nullable_function_value_call_8099,
           field_call_is_refused_spec_7_4) {
    expectRejectedWith(
        "module M;\n"                                            // 1
        "import System;\n"                                       // 2
        "class D public property F: ((int) -> int)? := null; end D;\n"  // 3
        "class C\n"                                              // 4
        "  public static method Main(): int\n"                   // 5
        "    var d := new D();\n"                                // 6
        "    var r: int := d.F(1);\n"                            // 7
        "    return 0;\n"                                        // 8
        "  end Main;\n"                                          // 9
        "end C;\n",                                              // 10
        7, kOtherShapeMessage);
}

// A METHOD's RESULT of nullable function type, called as `x.Nb()(...)` --
// the second spelling #8099 names. A call result is not a narrowing carrier.
BEZEL_TEST(nullable_function_value_call_8099,
           method_result_call_is_refused_spec_7_4) {
    expectRejectedWith(
        "module M;\n"                                            // 1
        "import System;\n"                                       // 2
        "class D\n"                                              // 3
        "  public method Nb(): (() -> int)?\n"                   // 4
        "    return null;\n"                                     // 5
        "  end Nb;\n"                                            // 6
        "end D;\n"                                               // 7
        "class C\n"                                              // 8
        "  public static method Main(): int\n"                   // 9
        "    var d := new D();\n"                                // 10
        "    var r: int := d.Nb()();\n"                          // 11
        "    return 0;\n"                                        // 12
        "  end Main;\n"                                          // 13
        "end C;\n",                                              // 14
        11, kOtherShapeMessage);
}

// `t.Result()` on a `Task<(() -> int)?>` -- the third spelling #8099 names.
// §30.3 declares `public readonly property Result: T;`, so the read is a
// `(() -> int)?` -- a nullable function type -- and the call through it is
// the §7.4 case.
//
// NOTE the spelling: the task's type argument is taken from the ARGUMENT's
// type (`Task.FromResult<T>(value: T): Task<T>`, stdlib §6.7), so the
// argument must itself be a nullable function value for the task to carry
// `(() -> int)?`. MEASURED: `Task.FromResult<(() -> int)?>(lambda() => 5)`
// yields `Task<() -> int>` -- the EXPLICIT type argument is dropped -- so
// that spelling is a legal call and is pinned as a control below.
BEZEL_TEST(nullable_function_value_call_8099,
           task_result_call_is_refused_spec_7_4) {
    expectRejectedWith(
        "module M;\n"                                            // 1
        "import System;\n"                                       // 2
        "import System.Threading.Tasks;\n"                       // 3
        "class C\n"                                              // 4
        "  public static method Main(): int\n"                   // 5
        "    var f: (() -> int)? := null;\n"                     // 6
        "    using var t := Task.FromResult(f);\n"               // 7
        "    var r: int := t.Result();\n"                        // 8
        "    return 0;\n"                                        // 9
        "  end Main;\n"                                          // 10
        "end C;\n",                                              // 11
        8, kOtherShapeMessage);
}

// A PROCEDURE-typed nullable function value (`((int) -> ())?`). §7.6 says
// the null-conditional call's "result type is nullable (or `()` for
// procedure-typed values)" -- so the procedure form is the same case.
BEZEL_TEST(nullable_function_value_call_8099,
           procedure_typed_call_is_refused_spec_7_4) {
    expectRejectedWith(
        "module M;\n"                                            // 1
        "import System;\n"                                       // 2
        "class C\n"                                              // 3
        "  public static method Main(): int\n"                   // 4
        "    var h: ((int) -> ())? := null;\n"                   // 5
        "    h(1);\n"                                            // 6
        "    return 0;\n"                                        // 7
        "  end Main;\n"                                          // 8
        "end C;\n",                                              // 9
        6, simpleVarMessage("h"));
}

// ---------------------------------------------------------------------------
// CONTROL rows -- the legal spellings, all still accepted.
// ---------------------------------------------------------------------------

// §7.6: the null-conditional call `?(...)` is the ONE spelling a nullable
// function value is invoked with. It runs on a null value (no-op) and on a
// non-null one (the call happens), with exact stdout for both.
BEZEL_TEST(nullable_function_value_call_8099,
           null_conditional_call_runs_on_null_and_non_null_spec_7_6) {
    expectStdout(
        "module M;\n"                                            // 1
        "import System;\n"                                       // 2
        "class C\n"                                              // 3
        "  public static method Main(): int\n"                   // 4
        "    var h: ((int) -> int)? := null;\n"                  // 5
        "    var a := h?(1);\n"                                  // 6
        "    Console.WriteLine((a ?? -1).ToString());\n"          // 7
        "    h := lambda(x: int) => x + 1;\n"                     // 8
        "    var b := h?(1);\n"                                  // 9
        "    Console.WriteLine((b ?? -1).ToString());\n"          // 10
        "    return 0;\n"                                        // 11
        "  end Main;\n"                                          // 12
        "end C;\n",                                              // 13
        "-1\n2\n");
}

// §7.4: a null test narrows the carrier, and the narrowed call is admitted.
// The carrier is a PARAMETER, so nothing but the `if` narrows it -- a local
// initialized with a non-null value would be narrowed by its declaration
// (§7.4 "from a declaration whose initializer is such an `expr`") and would
// not discriminate.
BEZEL_TEST(nullable_function_value_call_8099,
           call_after_a_null_check_is_accepted_spec_7_4) {
    expectStdout(
        "module M;\n"                                            // 1
        "import System;\n"                                       // 2
        "class C\n"                                              // 3
        "  public static method Take(h: ((int) -> int)?): int\n" // 4
        "    if h <> null then\n"                                // 5
        "      return h(1);\n"                                   // 6
        "    end;\n"                                             // 7
        "    return 0;\n"                                        // 8
        "  end Take;\n"                                          // 9
        "  public static method Main(): int\n"                   // 10
        "    Console.WriteLine(Take(lambda(x: int) => x + 1).ToString());\n"  // 11
        "    return 0;\n"                                        // 12
        "  end Main;\n"                                          // 13
        "end C;\n",                                              // 14
        "2\n");
}

// §7.8: the `!` force-unwrap asserts non-nullness at runtime and yields the
// non-nullable form of the expression, so the call through it is admitted.
// The carrier is a PARAMETER (never narrowed), so `!` is the only thing that
// admits this call.
BEZEL_TEST(nullable_function_value_call_8099,
           call_after_the_force_unwrap_is_accepted_spec_7_8) {
    expectStdout(
        "module M;\n"                                            // 1
        "import System;\n"                                       // 2
        "class C\n"                                              // 3
        "  public static method Take(p: ((int) -> int)?): int\n" // 4
        "    return p!(1);\n"                                    // 5
        "  end Take;\n"                                          // 6
        "  public static method Main(): int\n"                   // 7
        "    Console.WriteLine(Take(lambda(x: int) => x + 1).ToString());\n"  // 8
        "    return 0;\n"                                        // 9
        "  end Main;\n"                                          // 10
        "end C;\n",                                              // 11
        "2\n");
}

// A NON-NULLABLE function value is callable with no ceremony at all.
BEZEL_TEST(nullable_function_value_call_8099,
           non_nullable_function_value_call_is_accepted_spec_27_1) {
    expectStdout(
        "module M;\n"                                            // 1
        "import System;\n"                                       // 2
        "class C\n"                                              // 3
        "  public static method Main(): int\n"                   // 4
        "    var h: (int) -> int := lambda(x: int) => x + 1;\n" // 5
        "    Console.WriteLine(h(1).ToString());\n"               // 6
        "    return 0;\n"                                        // 7
        "  end Main;\n"                                          // 8
        "end C;\n",                                              // 9
        "2\n");
}

// §7.7: the `??` fallback yields a NON-nullable value, so the call through
// it is admitted.
BEZEL_TEST(nullable_function_value_call_8099,
           coalesce_fallback_call_is_accepted_spec_7_7) {
    expectStdout(
        "module M;\n"                                            // 1
        "import System;\n"                                       // 2
        "class C\n"                                              // 3
        "  public static method Main(): int\n"                   // 4
        "    var h: ((int) -> int)? := null;\n"                  // 5
        "    var r := (h ?? lambda(x: int) => 0)(1);\n"           // 6
        "    Console.WriteLine(r.ToString());\n"                 // 7
        "    return 0;\n"                                        // 8
        "  end Main;\n"                                          // 9
        "end C;\n",                                              // 10
        "0\n");
}

// The ADMITTING TWIN of the `t.Result()` rejection: the same call over a
// task whose result type is a NON-nullable function type is legal (§27.1).
// This is the spelling #7971's own test file uses -- `Task.FromResult<(() ->
// int)?>(lambda() => 5)` -- and MEASURED it yields `Task<() -> int>` (the
// EXPLICIT type argument is dropped in favour of the argument's type), so
// the call is a call through a NON-nullable function value.
BEZEL_TEST(nullable_function_value_call_8099,
           non_nullable_task_result_call_is_accepted_spec_27_1) {
    expectStdout(
        "module M;\n"                                            // 1
        "import System;\n"                                       // 2
        "import System.Threading.Tasks;\n"                       // 3
        "class C\n"                                              // 4
        "  public static method Main(): int\n"                   // 5
        "    using var t := Task.FromResult<() -> int>(lambda() => 5);\n"  // 6
        "    var r := t.Result();\n"                             // 7
        "    Console.WriteLine(r.ToString());\n"                 // 8
        "    return 0;\n"                                        // 9
        "  end Main;\n"                                          // 10
        "end C;\n",                                              // 11
        "5\n");
}
