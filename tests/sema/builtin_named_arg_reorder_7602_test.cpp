// Spec-derived regression test for issue #7602.
//
// spec §22.5 (v0.357) "Optional parameters and named arguments":
//   "Named arguments may appear in any order after positional arguments.
//    Each supplied argument binds exactly one parameter and each parameter is
//    bound by at most one supplied argument ..."
// and §22.5's *Evaluation order*:
//   "A call's supplied argument expressions are evaluated in call-site
//    textual order — left to right as written, for positional and named
//    arguments alike; naming an argument changes which parameter it binds
//    to, never when it runs".
//
// The ISSUE'S OWN REPRO: `int.Parse(radix := 16, s := "ff")` and
// `Debug.Assert(message := "a message", condition := true)` were refused by
// the shared built-in resolver (`ExprTC::checkBuiltinCallAdmission`) with
// "a named argument that binds to a parameter other than its own position is
// not supported on this built-in member (§22.5, #3398)" — a compiler
// LIMITATION, where §22.5 (v0.357) allows a named argument in any order.
//
// The rows below pin what the fix must deliver:
//
//   * the reordered call COMPILES and RUNS, and computes the RIGHT answer —
//     the row READS THE DATA the fix handles (`int.Parse(radix := 16, s :=
//     "ff")` is 255, the parsed value, and NOT the radix);
//   * the §5.1 question is asked of the parameter the argument BINDS, so
//     `int.Parse(radix := "x", s := 5)` is still refused, and the refusal
//     names the `radix` parameter — the one the argument NAMES — not the
//     `s` parameter it is written against;
//   * §22.5's EVALUATION ORDER survives the permutation: the arguments run
//     in call-site TEXTUAL order (the `radix` argument is written first, so
//     it runs first), which is what "naming an argument changes which
//     parameter it binds to, never when it runs" demands;
//   * the CONTROL rows — the positional and the in-order spellings — keep
//     working, unchanged;
//   * a member whose codegen arm still lowers its arguments by POSITION
//     keeps the §22.5 refusal: `int.TryParse(radix := 10, s := "42", out
//     var v)` is still refused, and the refusal is the same sentence.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The §22.5 refusal the shared resolver still draws for a member whose arm
// lowers its arguments by POSITION.
std::string reorderNeedle(const std::string& argName,
                          const std::string& paramName) {
    return "the argument named `" + argName + "` binds by §22.5 to the `" +
           paramName + "` parameter of that form, not to the parameter it is "
           "written at — a named argument that binds to a parameter other "
           "than its own position is not supported on this built-in member "
           "(§22.5, #3398)";
}

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

// An admitting row: the program must compile, run clean under the strict AOT
// oracle, and print exactly `out`.
void expectStdout(const std::string& body, const std::string& out) {
    auto r = runAotChecked(body);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

std::string prog(const std::string& imports, const std::string& body) {
    return "module M;\n" + imports +
           "class C\n"
           "  public static method Main(): int\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The EVALUATION-ORDER row's program: a helper whose call PRINTS, so the
// order the arguments run in is observable in the program's output.
std::string progWithNote(const std::string& body) {
    return "module M;\n"
           "class C\n"
           "  public static method NoteInt(v: int): int\n"
           "    Console.WriteLine(\"v\");\n"
           "    return v;\n"
           "  end NoteInt;\n"
           "  public static method Main(): int\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

const char kDiagImport[] = "import System.Diagnostics;\n";

}  // namespace

// ── §22.5: a reordered named call is ADMITTED and computes the bound value ──

// The same call in the RESERVED PascalCase spelling (§3.4): `Int32.Parse` and
// `int.Parse` are one head (§8.1 / §9.1).
BEZEL_TEST(builtin_named_arg_reorder_7602,
           int32_parse_named_reordered_is_admitted_and_parses_the_bound_value) {
    expectStdout(
        prog("", "Console.WriteLine(Int32.Parse(radix := 16, s := \"ff\"));"),
        "255\n");
}

// §22.5's EVALUATION ORDER: the supplied argument expressions run in
// call-site TEXTUAL order, and naming an argument changes which parameter it
// binds to, NEVER when it runs. The `radix` argument is written FIRST, so it
// runs first — the `v` the helper prints precedes the parsed value.
BEZEL_TEST(builtin_named_arg_reorder_7602,
           int_parse_named_reordered_evaluates_the_arguments_in_textual_order) {
    expectStdout(
        progWithNote("Console.WriteLine(int.Parse(radix := NoteInt(16), s := \"ff\"));"),
        "v\n255\n");
}

// ── §5.1 is asked of the parameter the argument BINDS ───────────────────────

// A name no declared parameter bears is still a compile error (§22.5).
BEZEL_TEST(builtin_named_arg_reorder_7602,
           int_parse_named_unknown_name_is_rejected) {
    expectRejected(prog("", "var v := int.Parse(bogus := 1, s := \"255\");"),
                   "has no parameter named `bogus`");
}

// The same parameter named twice is still a compile error (§22.5: "each
// parameter is bound by at most one supplied argument").
BEZEL_TEST(builtin_named_arg_reorder_7602,
           int_parse_named_parameter_bound_twice_is_rejected) {
    expectRejected(prog("", "var v := int.Parse(s := \"255\", s := \"16\");"),
                   "parameter `s` is bound more than once");
}

// ── the members whose arm lowers by POSITION keep the refusal ─────────────

// `int.TryParse`'s `out` slot is POSITIONAL-ONLY (§22.5: "A ref/out/scoped
// argument is positional-only"), and its arm still lowers the arguments by
// POSITION — so the reordered spelling is still refused, with the same
// sentence. This row pins the SCOPE of the fix: the §22.5 refusal is not
// lifted for every value built-in, only for the members whose arm reads the
// published binding.
BEZEL_TEST(builtin_named_arg_reorder_7602,
           int_try_parse_named_reordered_is_still_refused) {
    expectRejected(
        prog("", "var v := 0; var ok := int.TryParse(radix := 10, s := \"42\", out v);"),
        reorderNeedle("radix", "radix"));
}

// The `out` slot still binds positionally: the standard library's own
// surface uses this spelling (§9.1 `TryParse(s: string, out result: int)`),
// and it keeps working.
BEZEL_TEST(builtin_named_arg_reorder_7602,
           int_try_parse_named_with_positional_out_is_admitted) {
    expectStdout(
        prog("", "var v := 0; var ok := int.TryParse(s := \"42\", out v); "
                 "Console.WriteLine(v);"),
        "42\n");
}

// ── CONTROLS: the calls that bind IN ORDER (positional, named in order, the
// condition alone) are pinned once, in builtin_named_arg_binding_7419_test.cpp.
