// Issue #7260 (spec §28.9 / §8.7 / §28.2, v0.344) -- a FUNCTION-TYPED value
// has NO `Dispose()` member at all, so `f.Dispose()` on one is a LOCATED
// COMPILE ERROR, not a real dispose.
//
// THE GOVERNING TEXT.
//
//   * spec §28.9 (v0.344) "Runtime-managed types", opening sentence: "The
//     following types are exempt from manual disposal ... Their `Dispose()`
//     is a no-op (or, for types that are not classes and therefore have no
//     `Dispose()` at all, the type's lifetime is managed by the runtime
//     without user involvement)."
//   * spec §28.9 (v0.344), the function-typed bullet: "Function-typed values
//     -- values of any function type `(T1, ..., Tn) -> R`, REGARDLESS OF
//     WHETHER THEY CAPTURE. This includes lambda closures (capturing or
//     not), method references (`obj.Method` and `Type.StaticMethod`), and any
//     value whose static type is a function type."
//   * spec §8.7 (v0.344): "`Dispose()` is class-only. The compiler generates
//     `IDisposable` on every non-static class."
//   * spec §28.2 (v0.344): "`Dispose()` is not on `Object`; it is class-only."
//
// A FUNCTION TYPE IS NOT A CLASS, so it falls in §28.9's parenthetical: it
// has no `Dispose()` at all.
//
// THE DEFECT. The compiler gave a function-typed value a REAL dispose -- a
// `Dispose()` call on a closure cell actually ran, and two banked rows pinned
// that non-conformant behaviour. The issue's own title records the shape
// ("a function-typed value gets a REAL `Dispose()` (exception + SIGSEGV) but
// §28.9 says it has NO `Dispose()` at all -- TWO BANKED ROWS pin the
// non-conformant behaviour").
//
// THE FIX LANDED ELSEWHERE (before this test existed). The defect was fixed
// by commit be9652d53 ("narrow #7260: a function-typed value has no
// Dispose() member", 2026-10-27): the `Kind::Function` arm of
// `closedValueSurfaceMemberReason` no longer returns {} for `Dispose`; it
// draws the class-only diagnostic, the same message shape and the same
// citations the closest sibling (`o.Dispose()` at an `Object`) already had.
// Nothing closed the issue, so it stayed open on a tree that already behaves
// correctly.
//
// ORACLE. `runAot` (the AOT path). The reject rows assert the SPECIFIC
// diagnostic text -- "no member `Dispose` on a function value" -- AND that the
// refusal is LOCATED (the house's own `failedAtCompileTimeWithRealLocation`),
// and that the program did NOT run. The control row asserts the EXACT
// STDOUT of the function value being CALLED, so the rule cannot be satisfied
// by refusing the function type itself.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

std::string prog(const std::string& decls, const std::string& body) {
    return "module M;\n" + decls +
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The row is a REJECTION: the program must not run, the front end must have
// reported an error AT A REAL LOCATION, and the error must carry the
// class-only text with its citations.
void expectNoDispose(const std::string& decls, const std::string& body) {
    JitResult r = runAot(prog(decls, body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The row claims a LOCATED error, so the row ASSERTS the location: the
    // house's own located-rejection helper
    // (tests/support/aot_result_asserts.hpp) requires a `file:line:column:`
    // prefix on a diagnostic that is not a crash, a hang or a sanitizer
    // report.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Dispose` on a function value"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "class-only"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "8.7"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "28.2"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "28.9"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
}

void expectStdout(const std::string& decls, const std::string& body,
                  const std::string& expected) {
    JitResult r = runAot(prog(decls, body));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') out.pop_back();
    BEZEL_EXPECT_EQ(out, expected);
}

}  // namespace

// =====================================================================
// ROW 1 -- a LAMBDA-typed local. §28.9's function-typed bullet covers
// "lambda closures (capturing or not)".
//
// Pre-fix: ACCEPTED and RAN -- the call really disposed the closure cell.
// Post-fix: REJECTED with the class-only diagnostic.
// =====================================================================
BEZEL_TEST(function_value_has_no_dispose_member_7260,
           a_lambda_valued_local_has_no_dispose_member_28_9) {
    expectNoDispose("",
                    "    var f := lambda(x: int) => x + 1;\n"
                    "    f.Dispose();\n");
}

// =====================================================================
// ROW 2 -- a METHOD REFERENCE. §28.9's function-typed bullet names
// "method references (`obj.Method` and `Type.StaticMethod`)" explicitly, so
// the reference is a function type too and has no `Dispose()`.
//
// Pre-fix: ACCEPTED and RAN.
// Post-fix: REJECTED.
// =====================================================================
BEZEL_TEST(function_value_has_no_dispose_member_7260,
           a_method_reference_has_no_dispose_member_28_9) {
    expectNoDispose("public class Helper\n"
                    "  public static method Make(): int\n"
                    "    return 5;\n"
                    "  end Make;\n"
                    "end Helper;\n",
                    "    var f := Helper.Make;\n"
                    "    f.Dispose();\n");
}

// =====================================================================
// ROW 3 -- a FUNCTION-TYPED PARAMETER: "any value whose static type is a
// function type" is covered, however it is bound.
//
// Pre-fix: ACCEPTED and RAN.
// Post-fix: REJECTED.
// =====================================================================
BEZEL_TEST(function_value_has_no_dispose_member_7260,
           a_function_typed_parameter_has_no_dispose_member_28_9) {
    expectNoDispose("public class Runner\n"
                    "  public static method Run(g: (int) -> int): int\n"
                    "    g.Dispose();\n"
                    "    return 0;\n"
                    "  end Run;\n"
                    "end Runner;\n",
                    "    Console.WriteLine(Runner.Run(lambda(x: int) => x));\n");
}

// =====================================================================
// CONTROL -- the function value itself is untouched: it still CALLS, and it
// still prints the right value. A VALUE oracle (exact stdout), green on
// both arms: it pins that §28.9's rule removes the `Dispose()` member and
// nothing else.
//
// Pre-fix and post-fix: "3".
// =====================================================================
BEZEL_TEST(function_value_has_no_dispose_member_7260,
           the_function_value_itself_still_calls_28_9) {
    expectStdout("",
                 "    var f := lambda(x: int) => x + 1;\n"
                 "    Console.WriteLine(f(2));\n",
                 "3");
}
