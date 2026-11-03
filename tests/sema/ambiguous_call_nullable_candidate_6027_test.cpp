// Issue #6027 (spec §22.6, v0.338) -- an ambiguous-call message cites the
// NULLABLE candidate with its `?` MARKER, so the two candidates the user reads
// are distinguishable.
//
// THE DEFECT (ALREADY FIXED in the tree before this test existed). §22.6
// (v0.350) says: "the call is **ambiguous** and is a compile error. The
// compiler reports the error and cites the candidate overloads". For the pair
// `Handle(e: IOException)` / `Handle(e: IOException?)` the ambiguity itself is
// right -- both need a reference conversion, so they are co-equal (§22.6) --
// but the candidate LISTING dropped the nullable marker, so the same candidate
// was cited twice:
//
//     ambiguous call to `Handle` ... candidates:
//       Handle(IOException)
//       Handle(IOException)
//
// The two overloads ARE distinguishable by their argument (an exact
// `IOException` argument picks the first, a `null` argument picks the second),
// so the message must show the difference.
//
// THE FIX LANDED ELSEWHERE (before this test existed). The defect was fixed by
// commit 5a83fc0e ("fix #6770: cite §22.6 ambiguity candidates with type
// arguments and ? (§22.6, v0.340)"), which introduced the candidate spelling
// `spelledTypeForSignature`: a candidate is cited by its parameter types AS
// SPELLED -- the last segment of a named type, then its generic arguments and
// its `?` marker. Before it the spelling printed only the bare name segment
// and DROPPED the marker, so `Handle(e: IOException?)` was cited as
// `Handle(IOException)`. Its sibling commit 7099c5ad ("fix #6871: spell an
// imported overload candidate's nullable as long?") gave the IMPORTED
// candidate the same spelling. Nothing closed the issue, so it stayed open on a
// tree that already behaves correctly.
//
// ORACLE. `runAot` (the AOT path). The rows assert the SPECIFIC diagnostic
// text -- the candidate LINES of the ambiguity message -- and that the program
// did NOT run. The control rows assert the EXACT STDOUT of the two
// distinguishable calls, so the rows cannot be satisfied by refusing every
// call to an overloaded member.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

bool has(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

// The ambiguity diagnostic of the program, or "" if there is none.
std::string ambiguityMessage(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("ambiguous call to") != std::string::npos)
            return d.message;
    return "";
}

std::string prog(const std::string& body) {
    return "module M;\n"
           "import System;\n"
           "import System.IO;\n"
           "class C\n"
           "  method Handle(e: IOException): int => 1;\n"
           "  method Handle(e: IOException?): int => 2;\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The row is a REJECTION: the program must not run, the front end must have
// reported an error, and the error must carry the named text.
void expectRejected(const std::string& body, const std::string& needle) {
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(has(ambiguityMessage(r), needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(bezel::test::diagnosticMessageContains(
        r, "compiler CRASHED"));
}

// A control row: the program compiles, runs clean under the strict AOT oracle,
// and prints exactly `out`.
void expectStdout(const std::string& body, const std::string& out) {
    JitResult r = runAotChecked(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// =====================================================================
// ROW 1 -- the call is genuinely AMBIGUOUS (a `FileNotFoundException` needs a
// reference conversion at either candidate), so §22.6 refuses it and cites the
// candidates.
//
// Pre-fix: refused too (the ambiguity itself was right) -- the row is GREEN on
//           both arms and load-bearing for row 2.
// Post-fix: refused with the candidate listing.
// =====================================================================
BEZEL_TEST(ambiguous_call_nullable_candidate_6027,
           the_ambiguous_call_is_refused_and_cites_its_candidates_22_6) {
    // per §22.6 (v0.350)
    expectRejected("    var c := new C();\n"
                   "    Console.WriteLine(c.Handle(new FileNotFoundException(\"x\")));\n",
                   "\n  Handle(IOException)\n");
}

// =====================================================================
// ROW 2 -- THE ISSUE'S OWN DEFECT: the candidate listing keeps the `?`
// MARKER on the nullable candidate, so the two cited candidates are
// distinguishable.
//
// Pre-fix: the listing read "Handle(IOException)" and "Handle(IOException)" --
//           the SAME candidate twice, the nullable marker dropped.
// Post-fix: the listing reads "Handle(IOException)" and "Handle(IOException?)".
// =====================================================================
BEZEL_TEST(ambiguous_call_nullable_candidate_6027,
           the_nullable_candidate_is_listed_with_its_question_mark_22_6) {
    // per §22.6 (v0.350)
    expectRejected("    var c := new C();\n"
                   "    Console.WriteLine(c.Handle(new FileNotFoundException(\"x\")));\n",
                   "\n  Handle(IOException?)\n");
}

// =====================================================================
// ROW 3 -- CONTROL: the two overloads ARE distinguishable by their argument,
// so an exact `IOException` argument picks the FIRST candidate (1) ...
//
// Pre-fix: 1 as well -- the control row is GREEN on both arms.
// Post-fix: 1.
// =====================================================================
BEZEL_TEST(ambiguous_call_nullable_candidate_6027,
           control_an_exact_ioexception_argument_picks_the_first_candidate) {
    // per §22.6 (v0.350)
    expectStdout("    var c := new C();\n"
                 "    Console.WriteLine(c.Handle(new IOException(\"x\")));\n",
                 "1\n");
}

// =====================================================================
// ROW 4 -- CONTROL: ... and a `null` argument picks the NULLABLE candidate
// (2), which is the very distinction the message must show.
//
// Pre-fix: 2 as well -- the control row is GREEN on both arms.
// Post-fix: 2.
// =====================================================================
BEZEL_TEST(ambiguous_call_nullable_candidate_6027,
           control_a_null_argument_picks_the_nullable_candidate) {
    // per §22.6 (v0.350) + §7.4 (v0.350)
    expectStdout("    var c := new C();\n"
                 "    Console.WriteLine(c.Handle(null));\n",
                 "2\n");
}
