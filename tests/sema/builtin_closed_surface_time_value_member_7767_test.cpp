// Issue #7767 (stdlib §2.5 DateTime / TimeSpan / DateOnly / TimeOnly,
// §2.6 Guid, v0.350) -- an UNKNOWN member on a by-name built-in VALUE type
// is a LOCATED COMPILE ERROR, not a silent acceptance that degrades to zero
// behind exit 0.
//
// THE DEFECT. A member that is not on a by-name built-in's declared surface
// was left untyped and unreported by sema, so the read bound nothing, the
// statement was dropped by codegen's degradation, and the program compiled
// and RAN. The issue names the two shapes: `q.Value` (a `DateTime?`) and
// `d.Foo` (a `DateTime`), and the same rule holds for `TimeSpan`,
// `DateOnly`, `TimeOnly` and `Guid`.
//
// THE FIX LANDED ELSEWHERE (before this test existed). The defect was fixed
// by commit 0d986bd3 ("Reject undeclared members on by-name built-ins, narrow
// #7661", 2026-10-30): sema now asks ONE closed-surface question after
// `resolveMemberAccess`, for every receiver spelling, against the table the
// standard library documents. This commit adds the regression test that pins
// the #7767 repros and closes the issue.
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
// ROW 1 -- the issue's `d.Foo` on a `DateTime` receiver. stdlib §2.5
// (v0.350) declares `DateTime` with a closed member surface; `Foo` is not
// on it.
//
// Pre-fix: ACCEPTED and degraded -- the statement emitted nothing and the
//           program exited 0.
// Post-fix: REJECTED with the closed-surface diagnostic.
// =====================================================================
BEZEL_TEST(builtin_closed_surface_time_value_member_7767,
           an_unknown_member_on_a_date_time_is_refused_stdlib_2_5) {
    expectNoMember("    var d := new DateTime(2024, 3, 15);\n"
                   "    Console.WriteLine(d.Foo);\n",
                   "no member `Foo` on built-in type `DateTime`");
}

// =====================================================================
// ROW 2 -- the issue's `q.Value` on a `DateTime?` receiver: `Value` is
// not on `DateTime`'s documented surface (stdlib §2.5), so the read is
// refused rather than silently accepted.
//
// Pre-fix: ACCEPTED and degraded -- the local was zero-initialized and the
//           program exited 0.
// Post-fix: REJECTED.
// =====================================================================
BEZEL_TEST(builtin_closed_surface_time_value_member_7767,
           an_unknown_member_on_a_nullable_date_time_is_refused_stdlib_2_5) {
    expectNoMember("    var q: DateTime? := DateTime.FromTicks(5L);\n"
                   "    Console.WriteLine(q.Value);\n",
                   "no member `Value` on built-in type `DateTime`");
}

// =====================================================================
// ROW 3 -- the same rule on the OTHER by-name built-in value types the
// fix covers: `TimeSpan` and `DateOnly` (stdlib §2.5) and `Guid`
// (stdlib §2.6).
//
// Pre-fix: ACCEPTED and degraded to zero behind exit 0.
// Post-fix: REJECTED.
// =====================================================================
BEZEL_TEST(builtin_closed_surface_time_value_member_7767,
           unknown_members_on_the_other_builtin_value_types_are_refused) {
    expectNoMember("    var t := TimeSpan.FromTicks(5L);\n"
                   "    Console.WriteLine(t.Foo);\n",
                   "no member `Foo` on built-in type `TimeSpan`");
    expectNoMember("    var d := new DateOnly(2024, 3, 15);\n"
                   "    Console.WriteLine(d.Foo);\n",
                   "no member `Foo` on built-in type `DateOnly`");
    expectNoMember("    var g := Guid.NewGuid();\n"
                   "    Console.WriteLine(g.Value);\n",
                   "no member `Value` on built-in type `Guid`");
}

// =====================================================================
// ROW 4 -- `TimeOnly` (stdlib §2.5) as well: the last of the five the
// issue names.
//
// Pre-fix: ACCEPTED and degraded to zero behind exit 0.
// Post-fix: REJECTED.
// =====================================================================
BEZEL_TEST(builtin_closed_surface_time_value_member_7767,
           an_unknown_member_on_a_time_only_is_refused_stdlib_2_5) {
    expectNoMember("    var t := new TimeOnly(1, 2, 3);\n"
                   "    Console.WriteLine(t.Foo);\n",
                   "no member `Foo` on built-in type `TimeOnly`");
}

// =====================================================================
// CONTROL -- the DECLARED members of the same surfaces still read, and read
// the right value. A VALUE oracle (exact stdout), green on both arms: the
// closed-surface rule refuses only the names the standard library does not
// declare.
//
// Pre-fix and post-fix: "5", "7" and "36".
// =====================================================================
BEZEL_TEST(builtin_closed_surface_time_value_member_7767,
           declared_members_of_the_same_surfaces_still_read_2_5_2_6) {
    expectStdout("    var d := DateTime.FromTicks(5L);\n"
                 "    Console.WriteLine(d.Ticks);\n"
                 "    var t := TimeSpan.FromTicks(7L);\n"
                 "    Console.WriteLine(t.Ticks);\n"
                 "    var g := Guid.Empty;\n"
                 "    Console.WriteLine(g.ToString().Length);\n",
                 "5\n7\n36");
}
