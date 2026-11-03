// Issue #7749 (stdlib §3.1 StringBuilder / §2.5 DateTime / §2.6 Guid / §2.7
// Random, v0.350) -- an unknown PROPERTY READ on a built-in class is a
// LOCATED COMPILE ERROR, not a silent degradation to the internal #2434
// warning behind exit 0.
//
// THE DEFECT. A member that is not on a by-name built-in's declared surface
// was left untyped and unreported by sema. Codegen's degradation then turned
// the read into a zero value or an empty statement, so the program compiled
// and RAN. Per stdlib (v0.350) a member that is not declared does not exist,
// so the read must be a located error.
//
// THE ISSUE'S OWN REPRO (its `a4.bzl`):
//
//     using var sb := new StringBuilder();
//     var j: int := sb.Nope;      // <- the property read: accepted, degraded
//     var k := sb.Nope2();        // <- the call twin: already refused
//
// `sb.Nope` bound no member; the local `j` was zero-initialized and the
// program exited 0 having printed nothing.
//
// THE FIX LANDED ELSEWHERE (before this test existed). The defect was fixed
// by commit 0d986bd3 ("Reject undeclared members on by-name built-ins, narrow
// #7661", 2026-10-30), which asks ONE closed-surface question after
// `resolveMemberAccess` for every receiver spelling and, in the `?.` form
// (§7.5), in `resolveNullCondMember`. This commit adds the regression test
// that pins the #7749 repro and closes the issue.
//
// ORACLE. `runAot` (the AOT path). The reject rows assert the SPECIFIC
// diagnostic text -- "no member `Nope` on built-in type `StringBuilder`" --
// and that the program did NOT run. The control rows assert exact stdout, so
// the row is a positive oracle (a VALUE), never "no diagnostic was produced".

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
// reported an error, and the error must carry the closed-surface text. The
// "compiler CRASHED" exclusion keeps a crash from satisfying the row.
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
// ROW 1 -- THE ISSUE'S OWN REPRO: an unknown PROPERTY read on a
// `StringBuilder` local. stdlib §3.1 (v0.350) declares `StringBuilder` with
// a closed member surface; `Nope` is not on it.
//
// Pre-fix: ACCEPTED and degraded -- the local was zero-initialized, the
//           program compiled and exited 0 (the #2434 internal warning).
// Post-fix: REJECTED with the closed-surface diagnostic.
// =====================================================================
BEZEL_TEST(builtin_closed_surface_property_read_7749,
           an_unknown_property_read_on_a_builtin_class_is_refused_stdlib_3_1) {
    expectNoMember("    using var sb := new StringBuilder();\n"
                   "    var j: int := sb.Nope;\n"
                   "    Console.WriteLine(j);\n",
                   "no member `Nope` on built-in type `StringBuilder`");
}

// =====================================================================
// ROW 2 -- the SAME read at a `using var` binding, at the issue's exact
// spelling (the local's declared type is the built-in's).
// =====================================================================
BEZEL_TEST(builtin_closed_surface_property_read_7749,
           the_unknown_property_read_names_the_stdlib_section_3_1) {
    expectNoMember("    using var sb := new StringBuilder();\n"
                   "    var j: int := sb.Nope;\n"
                   "    Console.WriteLine(j);\n",
                   "stdlib §3.1 (v0.350) declares `StringBuilder` with a "
                   "closed member surface");
}

// =====================================================================
// ROW 3 -- the CALL twin of the same unknown name. The issue records that
// the call spelling was already refused; this row pins that it stays
// refused, so the two spellings of one name agree.
//
// Pre-fix: REJECTED (the CALL spelling was never the defect) -- but with the
//           PRE-FIX wording, so the closed-surface TEXT pin below did not
//           hold and this row failed on the pre-fix tree.
// Post-fix: REJECTED with the closed-surface diagnostic.
// =====================================================================
BEZEL_TEST(builtin_closed_surface_property_read_7749,
           the_call_twin_of_the_unknown_name_stays_refused_stdlib_3_1) {
    expectNoMember("    using var sb := new StringBuilder();\n"
                   "    var k := sb.Nope2();\n"
                   "    Console.WriteLine(k);\n",
                   "no member `Nope2` on built-in type `StringBuilder`");
}

// =====================================================================
// ROW 4 -- the same rule on the OTHER by-name built-ins the fix covers:
// an unknown property read on a `DateTime` (§2.5), a `Guid` (§2.6) and a
// `Random` (§2.7) receiver is a located error too.
//
// Pre-fix: ACCEPTED and degraded to zero behind exit 0.
// Post-fix: REJECTED.
// =====================================================================
BEZEL_TEST(builtin_closed_surface_property_read_7749,
           unknown_property_reads_on_the_other_by_name_builtins_are_refused) {
    expectNoMember("    var d := new DateTime(2024, 3, 15);\n"
                   "    Console.WriteLine(d.Nope);\n",
                   "no member `Nope` on built-in type `DateTime`");
    expectNoMember("    var g := Guid.NewGuid();\n"
                   "    Console.WriteLine(g.Nope);\n",
                   "no member `Nope` on built-in type `Guid`");
    expectNoMember("    var r := new Random(1L);\n"
                   "    Console.WriteLine(r.Nope);\n",
                   "no member `Nope` on built-in type `Random`");
}

// =====================================================================
// CONTROL -- the DECLARED members of the same surfaces still read, and
// read the right value. This row is a VALUE oracle (exact stdout), green on
// both arms: it pins that the closed-surface rule refuses only the names the
// standard library does not declare.
//
// Pre-fix and post-fix: "5" and "2".
// =====================================================================
BEZEL_TEST(builtin_closed_surface_property_read_7749,
           declared_members_of_the_same_surfaces_still_read_3_1_2_5) {
    expectStdout("    var d := DateTime.FromTicks(5L);\n"
                 "    Console.WriteLine(d.Ticks);\n"
                 "    var sb := new StringBuilder();\n"
                 "    sb.Append(\"ab\");\n"
                 "    Console.WriteLine(sb.Length);\n",
                 "5\n2");
}
