// #8308 / §29.11 (v0.357) — a string-interpolation HOLE is an ordinary
// expression position, so a consumption inside a hole is one of §29.11's
// statically visible consumption points.
//
// §29.11's *Consumption points (normative list)* is a CLOSED list of USES:
//
//   1. Enumeration or a terminal operator (`for`, `Count`, `First`, `ToArray`, …)
//   2. Applying a further deferred operator: `var q2 := q.Select(f);`
//   3. Passing the query as an `IEnumerable<T>` argument.
//
// The list is a list of USES, not of statement positions: a terminal operator
// spends the enumeration right wherever it is written, and an interpolation
// hole (`$"{…}"`, §18.5) is an expression position like any other.
//
// Before this fix the compiler never looked inside a hole, so:
//
//   (a) the §28.3.10 must-use lint's §29.11 suppression 4 did not reach a
//       query consumed in a hole — the binding drew the FALSE must-use
//       warning at its auto-drop although its enumeration right WAS spent
//       ("a spent right is the use"); and
//
//   (b) a SECOND consumption after a hole was not the located §29.11
//       rejection: the program compiled and raised
//       `InvalidOperationException` at RUN time, the loud failure §29.11
//       reserves for a re-enumeration the flow analysis CANNOT see.
//
// The rows below are the issue's own repros — (b) first, the located
// rejection, then (a), the false warning — plus the TWIN that STILL warns:
// the same query, never consumed, must keep drawing the lint. The twin is
// what proves the fix suppressed the lint on the SPENT right rather than on
// the query.
//
// NOT A DISPOSAL CHANGE. The suppression removes only the diagnostic; the
// auto-drop still runs at the binding's scope end, exactly as §29.11
// requires ("the frame is still auto-dropped at scope end, and the condition
// suppresses on the lint's own ground").

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// True iff the program is REJECTED at compile time and ONE diagnostic carries
// every needle — a specific diagnostic, not merely "it failed". A rejection
// for an unrelated reason (an unknown member, a type error) must not be able
// to satisfy the assertion.
bool rejectedWith(const std::string& src,
                  std::initializer_list<const char*> needles) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        bool all = true;
        for (const char* n : needles)
            if (d.message.find(n) == std::string::npos) all = false;
        if (all) return true;
    }
    return false;
}

}  // namespace

// §29.11 consumption point 1 (a terminal operator) inside an interpolation
// hole spends the enumeration right, so the SECOND consumption of the same
// bound query is the located §29.11 rejection — the same one the argument and
// binding spellings get. This is the issue's own repro: before the fix it
// compiled, and the second consumption raised `InvalidOperationException` at
// run time.
BEZEL_TEST(ownership_29_11_8308,
           hole_terminal_operator_then_for_in_is_rejected_spec_29_11) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var a := new Array<string>();\n"
        "    a.Add(\"a\"); a.Add(\"b\");\n"
        "    var q2 := a.Where(lambda(s) => s.Length > 0);\n"
        "    Console.WriteLine($\"{q2.First()}\");\n"
        "    for var s in q2 do Console.WriteLine(s); end for;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        {"bound lazy query `q2` was already consumed", "§29.11"}));
}

// The CONTROL for the row above, and the pair that shows the hole spelling is
// now treated like every other position: the SAME program with the
// consumption in an ordinary statement position — the spelling that already
// got the located §29.11 rejection — is rejected with the SAME message. The
// twin's rejection on the BASE arm (pre-fix) is what makes the hole row's
// green-on-tip meaningful: the shape is genuinely rejected for §29.11 and
// not for some unrelated reason.
BEZEL_TEST(ownership_29_11_8308,
           statement_terminal_operator_then_for_in_is_rejected_spec_29_11) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var a := new Array<string>();\n"
        "    a.Add(\"a\"); a.Add(\"b\");\n"
        "    var q2 := a.Where(lambda(s) => s.Length > 0);\n"
        "    var first := q2.First();\n"
        "    Console.WriteLine(first);\n"
        "    for var s in q2 do Console.WriteLine(s); end for;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        {"bound lazy query `q2` was already consumed", "§29.11"}));
}

// §29.11 consumption point 3 (the query passed as an `IEnumerable<T>`
// argument) inside an interpolation hole is a consumption point too, so the
// binding's enumeration right is SPENT and §28.3.10's suppression 4 applies:
// the must-use warning must NOT fire. This is the repro the issue's comment
// carries (the ergonomics study's B23 item): before the fix `q` drew
// "owned value in `q` is implicitly disposed at the end of its scope".
BEZEL_TEST(ownership_29_11_8308,
           hole_ienumerable_argument_suppresses_must_use_spec_29_11) {
    auto r = parseAndCheckOwnership(
        "module P;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var names: Array<string> := [\"a\", \"b\"];\n"
        "    var q := names.Where(lambda(n) => n <> \"a\");\n"
        "    Console.WriteLine($\"kept: {String.Join(\", \", q)}\");\n"
        "    return;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "q"));
}

// The TWIN of the row above, and the control the fix must not weaken: the
// SAME query, never consumed at all, spends nothing, so the lint MUST fire —
// naming `q`. Without this row the quiet row above would be satisfied by the
// lint never firing for a bound query, which is the vacuous-green shape.
BEZEL_TEST(ownership_29_11_8308,
           unconsumed_query_still_draws_must_use_spec_29_11) {
    auto r = parseAndCheckOwnership(
        "module P;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var names: Array<string> := [\"a\", \"b\"];\n"
        "    var q := names.Where(lambda(n) => n <> \"a\");\n"
        "    Console.WriteLine(\"kept\");\n"
        "    return;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "q"));
}

// The same hole spelling, with the query consumed in a hole AND consumed
// again outside it: the hole spends the right, so the SECOND consumption is
// the located §29.11 rejection. This is the pair that shows the hole counts
// as a consumption point from BOTH sides — the lint's suppression and the
// static rejection read the same list.
BEZEL_TEST(ownership_29_11_8308,
           hole_terminal_operator_then_outside_terminal_is_rejected_spec_29_11) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var a := new Array<string>();\n"
        "    a.Add(\"a\"); a.Add(\"b\");\n"
        "    var q2 := a.Where(lambda(s) => s.Length > 0);\n"
        "    Console.WriteLine($\"{q2.First()}\");\n"
        "    Console.WriteLine(q2.Last());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        {"bound lazy query `q2` was already consumed", "§29.11"}));
}
