// Issue #7695 (stdlib §2.5 DateTime / §2.7 Random / §3.1 StringBuilder,
// v0.350; spec §7.5 for the `?.` form) -- an UNDECLARED member reached
// through `?.`, or a property read on a `Random` / `StringBuilder` receiver,
// is a LOCATED COMPILE ERROR, not a silent degradation to zero behind
// exit 0.
//
// THE DEFECT. The `?.` spelling takes a SEPARATE resolver
// (`resolveNullCondMember`, §7.5) from the `.` spelling, so the closed
// surface question the `.` form asks was not asked there; and a `Random` /
// `StringBuilder` receiver carried no `builtinClassSym` target, so the read
// was not typed at all. Both got only the internal #2434 warning, the
// statement emitted nothing, and the program exited 0.
//
// THE ISSUE'S OWN REPROS:
//
//   * `d?.Foo` and `d?.AddFoo(1L)` on a `DateTime` / `DateTime?` receiver
//     (the `?.` resolver);
//   * `new Random(1L).Seed` (a property read on a `Random` receiver);
//   * `sb.IsEmpty` (a property read on a `StringBuilder` receiver).
//
// THE FIX LANDED ELSEWHERE (before this test existed). The defect was fixed
// by commit 0d986bd3 ("Reject undeclared members on by-name built-ins, narrow
// #7661", 2026-10-30): "The same question is asked in the `?.` form
// (resolveNullCondMember, §7.5)". This commit adds the regression test that
// pins the #7695 repros and closes the issue.
//
// ORACLE. `runAot` (the AOT path). The reject rows assert the SPECIFIC
// diagnostic text and that the program did NOT run; the control row asserts
// exact stdout (a VALUE oracle, never "no diagnostic was produced").

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::diagnosticMessageContains;
using bezel::test::JitResult;
using bezel::test::runAot;

std::string prog(const std::string& body) {
    return "module M;\n"
           "import System;\n"
           "import System.Text;\n"
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The row is a REJECTION: the program must not run, the front end must have
// reported an error, and the error must carry the closed-surface text.
void expectNoMember(const std::string& body, const char* needle) {
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
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
// ROW 1 -- the `?.` form on a `DateTime?` receiver: an undeclared member
// read through `?.` is refused. §7.5 (v0.350) defines the `?.` member
// read; stdlib §2.5 gives `DateTime` a closed member surface and `Foo` is
// not on it.
//
// Pre-fix: ACCEPTED and degraded -- the statement emitted nothing and the
//           program exited 0 behind the internal #2434 warning.
// Post-fix: REJECTED with the closed-surface diagnostic.
// =====================================================================
BEZEL_TEST(builtin_closed_surface_nullcond_member_7695,
           an_undeclared_member_through_qmark_on_a_date_time_is_refused_7_5) {
    expectNoMember("    var d: DateTime? := DateTime.FromTicks(5L);\n"
                   "    Console.WriteLine(d?.Foo);\n",
                   "no member `Foo` on built-in type `DateTime`");
}

// =====================================================================
// ROW 2 -- the `?.` CALL twin of the same unknown name (the issue's
// `d?.AddFoo(1L)`): refused as well.
//
// Pre-fix: ACCEPTED and degraded -- the `?.` resolver bound no member, the
//           statement emitted nothing, and the program exited 0.
// Post-fix: REJECTED with the closed-surface diagnostic.
// =====================================================================
BEZEL_TEST(builtin_closed_surface_nullcond_member_7695,
           the_qmark_call_twin_of_the_unknown_name_is_refused_7_5) {
    expectNoMember("    var d: DateTime? := DateTime.FromTicks(5L);\n"
                   "    Console.WriteLine(d?.AddFoo(1L));\n",
                   "no member `AddFoo` on built-in type `DateTime`");
}

// =====================================================================
// ROW 3 -- a property read on a `Random` receiver (the issue's
// `new Random(1L).Seed`). stdlib §2.7 (v0.350) declares `Random` with a
// closed member surface; `Seed` is not on it.
//
// Pre-fix: ACCEPTED and degraded to zero behind exit 0.
// Post-fix: REJECTED.
// =====================================================================
BEZEL_TEST(builtin_closed_surface_nullcond_member_7695,
           a_property_read_on_a_random_receiver_is_refused_stdlib_2_7) {
    expectNoMember("    var r := new Random(1L);\n"
                   "    Console.WriteLine(r.Seed);\n",
                   "no member `Seed` on built-in type `Random`");
}

// =====================================================================
// ROW 4 -- a property read on a `StringBuilder` receiver (the issue's
// `sb.IsEmpty`). stdlib §3.1 (v0.350) declares `StringBuilder` with a
// closed member surface; `IsEmpty` is not on it.
//
// Pre-fix: ACCEPTED and degraded -- the statement emitted nothing behind
//           exit 0 (the internal #3079 warning).
// Post-fix: REJECTED.
// =====================================================================
BEZEL_TEST(builtin_closed_surface_nullcond_member_7695,
           a_property_read_on_a_string_builder_receiver_is_refused_stdlib_3_1) {
    expectNoMember("    var sb := new StringBuilder();\n"
                   "    Console.WriteLine(sb.IsEmpty);\n",
                   "no member `IsEmpty` on built-in type `StringBuilder`");
}

// =====================================================================
// CONTROL -- the DECLARED members of the same surfaces still read through
// the SAME `?.` spelling, and read the right value. A VALUE oracle (exact
// stdout), green on both arms: the fix must refuse only the names the
// standard library does not declare.
//
// Pre-fix and post-fix: "5" and "0".
// =====================================================================
BEZEL_TEST(builtin_closed_surface_nullcond_member_7695,
           declared_members_through_qmark_still_read_7_5_2_5_3_1) {
    expectStdout("    var d: DateTime? := DateTime.FromTicks(5L);\n"
                 "    Console.WriteLine(d?.Ticks ?? 0L);\n"
                 "    var sb: StringBuilder? := new StringBuilder();\n"
                 "    Console.WriteLine(sb?.Length ?? 0);\n",
                 "5\n0");
}
