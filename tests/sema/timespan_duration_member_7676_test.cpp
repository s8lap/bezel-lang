// Regression test for issue #7676 -- `TimeSpan.Duration()` (a member stdlib
// §2.5 does NOT declare) was refused with the INTERNAL text "produced no
// value, and no dispatch arm emitted any code for this call" instead of a
// no-member diagnostic.
//
// THE DEFECT (already fixed on this tree; the issue stayed OPEN).
//
//   stdlib §2.5 (v0.350) declares `TimeSpan` with a CLOSED member surface and
//   `Duration` is not on it: "CompareTo", "Ticks", "Days", "Hours", "Minutes",
//   "Seconds", "Milliseconds", "TotalDays", "TotalHours", "TotalMinutes",
//   "TotalSeconds", "TotalMilliseconds", plus the §8.1 universal members.
//
//   The issue's own measurement: the refusal was correct (the call IS an
//   error) but the TEXT was internal -- a codegen-stage message, not a
//   diagnostic. spec §22.6 (v0.350): a call that matches no declared member
//   is a compile error, reported at the call.
//
// WHAT FIXED IT.
//
//   commit 0d986bd3 -- "Reject undeclared members on by-name built-ins,
//   narrow #7661". Sema now asks one closed-surface question after
//   resolveMemberAccess for every receiver spelling, so `TimeSpan.Duration()`
//   is refused by SEMA with a no-member diagnostic that names the member and
//   cites stdlib §2.5 -- and never reaches the codegen stage that produced
//   the internal text.
//
//   That commit is a `narrow` commit: it names #7661 in its subject and
//   carries NO closing keyword, so #7676 stayed OPEN until this one.
//
// THE ORACLE.
//
//   The DEFECT row asserts the POSITIVE diagnostic: the program does NOT run
//   and the diagnostic names `Duration`, cites stdlib §2.5, and is NOT the
//   internal "produced no value" text the issue complains about. On the
//   pre-fix tree the program was refused with the internal text, so this row
//   is RED there and GREEN here.
//
//   The CONTROL row asserts that a member the standard library DOES declare
//   keeps working -- it passes on BOTH arms.
//
// Cite: stdlib §2.5 (v0.350); spec §8.1 (v0.350), §22.6 (v0.350).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; "
           "public class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// THE DEFECT ROW -- the refusal is a NO-MEMBER diagnostic, not the internal
// codegen text.
// ---------------------------------------------------------------------------
BEZEL_TEST(timespan_duration_member_7676,
           timespan_duration_is_refused_with_a_no_member_diagnostic_2_5) {
    auto r = runAot(prog(
        "    var t := TimeSpan.FromTicks(5L); "
        "    Console.WriteLine(t.Duration()); "));
    BEZEL_EXPECT_FALSE(r.ran);
    bool named = false;
    bool internalText = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("no member `Duration` on built-in type `TimeSpan`") !=
            std::string::npos &&
            d.message.find("§2.5") != std::string::npos)
            named = true;
        if (d.message.find("produced no value") != std::string::npos)
            internalText = true;
    }
    BEZEL_EXPECT_TRUE(named);
    BEZEL_EXPECT_FALSE(internalText);
}

// ---------------------------------------------------------------------------
// THE CONTROL ROW -- a member the standard library DOES declare.
// ---------------------------------------------------------------------------
BEZEL_TEST(timespan_duration_member_7676,
           control_timespan_ticks_is_still_readable_2_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var t := TimeSpan.FromTicks(5L); "
        "    Console.WriteLine(t.Ticks); ")),
        std::string("5"));
}
