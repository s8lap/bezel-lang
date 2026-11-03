// Regression test for issue #7889 -- `a?.LastIndexOf(...)` on a nullable
// Array SILENTLY DEGRADED (#2434), and a wrong-arity `?.` call BYPASSED the
// §22.6 arity gate.
//
// THE DEFECT (already fixed on this tree; the issue stayed OPEN).
//
//   The issue's own measurements: `a?.LastIndexOf(1)` on an `Array<int>?` (and
//   the `Array<string>?` form) compiled with the internal #2434 "failed to
//   lower" warnings and printed 0 four times, where stdlib §4.4 and spec §7.5
//   give 2, -7, 2, -7 for [1,2,1], null, ["a","b","a","c"], null.
//
//   Spec §7.5 (v0.350): the result type of `?.` is always nullable, and the
//   `?.` spelling resolves to the SAME member the `.` spelling resolves to.
//   Spec §22.6 (v0.350): a call that matches no declared overload is a
//   compile error, reported at the call -- and the arity gate must not be
//   bypassed by the `?.` spelling.
//
// WHAT FIXED IT.
//
//   commit cafca8bb -- "fix #7671: admit and lower `Array<T>.LastIndexOf(item)`
//   (stdlib §4.4, v0.350)". Stdlib §4.4 declares
//   `Array<T>.LastIndexOf(item: T): int` (last occurrence, or -1) and §4 lists
//   `Array<T>.IndexOf` and `LastIndexOf` together under "Equality search"; the
//   member drew "no member `LastIndexOf` on `Array<T>`" before it, so the
//   `?.` spelling had nothing to lower and degraded to a zero value. The
//   commit admits the name (only for the `Array` head), lowers the search, and
//   keeps the one-argument arity.
//
//   commit 6a0a1cfe -- "fix #7778: `a?.Year` / `a?.Ticks` on DateTime? (and
//   TimeSpan?, DateOnly?, TimeOnly?) lower". Sema types the `?.` spelling by
//   resolving its `.` TWIN (a speculative synthetic `.` access over a
//   placeholder of the non-nullable receiver type), and codegen lowers the
//   NullCondMemberExpr arm by branching on the tagged struct's hasValue and
//   lowering that same synthetic `.` access over the payload -- so the `?.`
//   spelling carries the SAME one-argument arity gate the `.` spelling has.
//
//   The first commit closed #7671 (the `.` spelling) and the second closed
//   #7778 (the time-value `?.` reads); neither named this issue, so it stayed
//   OPEN until this one.
//
// THE ORACLE.
//
//   The VALUE rows assert the EXACT stdout of the AOT program: the real
//   last index for a non-null receiver, the `??` operand for a null one. On
//   the pre-fix tree the member did not exist at all, so the `?.` spelling
//   drew "no member `LastIndexOf` on `Array<T>`" and every one of these rows
//   is RED.
//
//   The ARITY row asserts the POSITIVE refusal: the program does NOT run and
//   the diagnostic names the member and cites §22.6. It is red on the tree
//   that carries the value fix but not the `?.` arity gate.
//
//   The CONTROL row asserts that the plain `.` spelling resolves to the SAME
//   member (§27.6) and produces the same index.
//
// Cite: stdlib §4.4 (v0.350); spec §7.5 (v0.350), §22.6 (v0.350).

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
// THE VALUE ROWS -- the `?.` spelling of the search lowers to the real
// index, and to the `??` operand on a null receiver.
// ---------------------------------------------------------------------------
BEZEL_TEST(null_cond_lastindexof_7889,
           nullable_int_array_lastindexof_is_the_real_index_4_4) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(1); "
        "    var b: Array<int>? := a; "
        "    Console.WriteLine(b?.LastIndexOf(1) ?? -7); ")),
        std::string("2"));
}

BEZEL_TEST(null_cond_lastindexof_7889,
           null_int_array_lastindexof_is_the_coalesce_operand_4_4) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a: Array<int>? := null; "
        "    Console.WriteLine(a?.LastIndexOf(1) ?? -7); ")),
        std::string("-7"));
}

BEZEL_TEST(null_cond_lastindexof_7889,
           nullable_string_array_lastindexof_is_the_real_index_4_4) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a := new Array<string>(); "
        "    a.Add(\"a\"); a.Add(\"b\"); a.Add(\"a\"); a.Add(\"c\"); "
        "    var b: Array<string>? := a; "
        "    Console.WriteLine(b?.LastIndexOf(\"a\") ?? -7); ")),
        std::string("2"));
}

BEZEL_TEST(null_cond_lastindexof_7889,
           null_string_array_lastindexof_is_the_coalesce_operand_4_4) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a: Array<string>? := null; "
        "    Console.WriteLine(a?.LastIndexOf(\"a\") ?? -7); ")),
        std::string("-7"));
}

// ---------------------------------------------------------------------------
// THE ARITY ROW -- the `?.` spelling does not bypass the §22.6 arity gate.
// ---------------------------------------------------------------------------
BEZEL_TEST(null_cond_lastindexof_7889,
           nullable_lastindexof_with_a_wrong_arity_is_refused_22_6) {
    auto r = runAot(prog(
        "    var a := new Array<int>(); "
        "    a.Add(1); "
        "    var b: Array<int>? := a; "
        "    Console.WriteLine(b?.LastIndexOf(1, 2, 3) ?? -7); "));
    BEZEL_EXPECT_FALSE(r.ran);
    bool named = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("LastIndexOf") != std::string::npos &&
            d.message.find("§22.6") != std::string::npos)
            named = true;
    BEZEL_EXPECT_TRUE(named);
}

// ---------------------------------------------------------------------------
// THE CONTROL ROW -- the plain `.` spelling resolves to the SAME member
// (§27.6) and produces the same index.
// ---------------------------------------------------------------------------
BEZEL_TEST(null_cond_lastindexof_7889,
           control_plain_dot_lastindexof_is_the_real_index_4_4) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(1); "
        "    Console.WriteLine(a.LastIndexOf(1)); ")),
        std::string("2"));
}
