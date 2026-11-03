// Issue #7363 (stdlib §9.4 Boolean / §9.5 Char, v0.350; spec §22.6, §5.1)
// -- a RADIX ARGUMENT given to a `bool` or a `char` receiver is a COMPILE
// ERROR. Neither type declares a radix overload, so no candidate is
// reachable, and the call must not compile.
//
// THE DEFECT. `true.ToString(16)` and `'a'.ToString(16)` COMPILED and
// printed "True" and "a" -- the radix was silently ignored -- while the
// INTEGER receiver honoured it (`(255).ToString(16)` printed "ff"). A call
// that looks like it asks for a radix and gets something else, silently.
//
// THE GOVERNING TEXT.
//
//   * stdlib §9.4 (v0.350) declares `Boolean` with EXACTLY ONE `ToString`:
//     "public pure method ToString(): string;"  -- "True" or "False". No
//     radix overload.
//   * stdlib §9.5 (v0.350) declares `Char` with EXACTLY ONE `ToString`:
//     "public pure method ToString(): string;".  No radix overload.
//   * spec §22.6 (v0.350): "If no candidate is applicable at all -- no
//     candidate is reachable: a conversion §5.1 does not admit as implicit,
//     ... -- the call is a compile error with no candidates cited."
//   * spec §5.1 (v0.350): an `int` is never implicitly convertible to a
//     `string`, so the only one-argument `ToString` form a `bool`/`char`
//     receiver has (the format form) is not reachable with a radix.
//
// THE FIX LANDED ELSEWHERE (before this test existed). The defect was fixed
// by commit 2250daa5 ("fix #6825 #7284 #7290 #7293 #7294 #7295 #7299 #7300
// #7323 narrow #6812 #7372: anchor the built-in call admission on ONE
// declared-signature table", 2026-10-28): the built-in call admission is
// anchored on ONE declared-signature table, so the argument is checked
// against the DECLARED parameter type and the radix arm finds no candidate.
// This commit adds the regression test that pins the #7363 repro and closes
// the issue.
//
// ORACLE. `runAot` (the AOT path). The reject rows assert the SPECIFIC
// diagnostic text ("no matching overload of `bool.ToString`") and that the
// program did NOT run; the control row asserts exact stdout (a VALUE
// oracle, never "no diagnostic was produced") and pins that the INTEGER
// receiver still honours the radix.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

std::string prog(const std::string& body) {
    return "module M;\n"
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The row is a REJECTION: the program must not run, the front end must have
// reported an error AT A REAL LOCATION, and the error must carry the
// no-candidate text.
void expectNoOverload(const std::string& body, const char* callee) {
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The row claims a LOCATED error, so the row ASSERTS the location: the
    // house's own located-rejection helper (tests/support/aot_result_asserts.hpp)
    // requires a `file:line:column:` prefix on a diagnostic that is not a
    // crash, a hang or a sanitizer report.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, (std::string("no matching overload of `") +
                                      callee + "`")
                                         .c_str()));
    // §22.6's own clause, and §5.1's rule the argument misses.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "no candidates cited"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "22.6"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "5.1"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
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
// ROW 1 -- the issue's own repro, the `bool` arm: `true.ToString(16)`.
//
// Pre-fix: COMPILED and printed "True" -- the radix was silently ignored.
// Post-fix: REJECTED -- no candidate is reachable (§22.6).
// =====================================================================
BEZEL_TEST(bool_char_radix_overload_rejected_7363,
           a_radix_argument_on_a_bool_receiver_is_a_compile_error_stdlib_9_4) {
    expectNoOverload("    Console.WriteLine(true.ToString(16));\n",
                     "bool.ToString");
}

// =====================================================================
// ROW 2 -- the `char` arm of the issue's repro: `'a'.ToString(16)`.
//
// Pre-fix: COMPILED and printed "a" -- the radix was silently ignored.
// Post-fix: REJECTED.
// =====================================================================
BEZEL_TEST(bool_char_radix_overload_rejected_7363,
           a_radix_argument_on_a_char_receiver_is_a_compile_error_stdlib_9_5) {
    expectNoOverload("    Console.WriteLine('a'.ToString(16));\n",
                     "char.ToString");
}

// =====================================================================
// CONTROL -- the NO-ARGUMENT form of the same two receivers still renders
// its no-format value, and the INTEGER receiver still HONOURS the radix.
// A VALUE oracle (exact stdout), green on both arms: the fix refuses the
// radix arm only, and leaves every other arm of the same member alone.
//
// Pre-fix and post-fix: "True", "a", "ff", "377".
// =====================================================================
BEZEL_TEST(bool_char_radix_overload_rejected_7363,
           the_no_argument_form_and_the_integer_radix_still_work_stdlib_9_1) {
    expectStdout("    Console.WriteLine(true.ToString());\n"
                 "    Console.WriteLine('a'.ToString());\n"
                 "    Console.WriteLine((255).ToString(16));\n"
                 "    Console.WriteLine((255).ToString(8));\n",
                 "True\na\nff\n377");
}
