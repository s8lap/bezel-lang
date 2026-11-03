// Regression test for issue #6024 -- `?.` on a built-in collection /
// StringBuilder member (`a?.Count`, `a?.Length`) inferred a NON-NULLABLE
// type, the NULL receiver yielded 0, and `var n: int := a?.Count` was
// ACCEPTED.
//
// THE DEFECT (already fixed on this tree; the issue stayed OPEN).
//
//   spec §7.5 (v0.350): "The result type of `?.` is always nullable."
//   stdlib §4.4 / §4.5 (v0.350) declare `Count` on the built-in collection
//   heads and §3.1 declares `Length` on `StringBuilder`.
//
//   The issue's own measurements: `var a: Array<int>? := null; var x := a?.Count;
//   var y := x ?? -1;` printed `0` (expected `-1`) behind the internal #2434
//   "failed to lower" degrade warning, and `var n: int := a?.Count;` COMPILED
//   (printed 0; the spec refuses it). The same held for Dictionary.Count,
//   Array.Length and Set.Count.
//
// WHAT FIXED IT.
//
//   commit c0e504b4 -- "narrow #6024: type `?.Count` / `?.Length` on built-in
//   heads as `int?`" (the TYPING half: the read is typed `int?`, so a
//   non-nullable initializer is refused by the §7 gate).
//   commit 5d1310c4 -- "narrow #6024: lower collection `?.Count` / `?.Length`
//   value as int?" (the VALUE half: the null-conditional member arm lowers
//   the receiver once, branches on the nullable payload and produces the
//   real count).
//
//   Neither commit carried a closing keyword for #6024 -- both are `narrow`
//   commits -- so the issue stayed OPEN until this one.
//
// THE ORACLE.
//
//   The VALUE rows assert the EXACT stdout of the AOT program (the real count
//   for a non-null receiver, the `??` fallback for a null one).
//   The TYPING row asserts the POSITIVE refusal: the program does NOT run and
//   the diagnostic names the nullable initializer and cites §7.
//   The CONTROL rows assert that the `string?.Length` spelling and the plain
//   `.` spelling were never affected -- they pass on BOTH arms, so a
//   "refuse everything" or "lower nothing" regression cannot pass.
//
//   No row asserts the ABSENCE of a diagnostic on a program that ran, so no
//   row is vacuous in the `check_vacuous_tests` sense (#3138).
//
// Cite: spec §7.5 (v0.350), §7 (v0.350); stdlib §3.1 / §4.4 / §4.5 (v0.350).

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

// The refusal: the program did NOT run and the diagnostic names the
// nullable initializer and cites the nullability rule.
bool refusedAsNullable(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("is nullable but the target type is non-nullable") !=
                std::string::npos &&
            d.message.find("(§7)") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// THE VALUE ROWS -- the null receiver yields the `??` operand, the non-null
// receiver yields the real count / length.
// ---------------------------------------------------------------------------
BEZEL_TEST(null_cond_builtin_count_6024,
           null_array_count_falls_back_to_the_coalesce_operand_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a: Array<int>? := null; "
        "    var x := a?.Count; "
        "    Console.WriteLine(x ?? -1); ")),
        std::string("-1"));
}

BEZEL_TEST(null_cond_builtin_count_6024,
           null_stringbuilder_length_falls_back_to_the_coalesce_operand_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var sb: StringBuilder? := null; "
        "    var x := sb?.Length; "
        "    Console.WriteLine(x ?? -1); ")),
        std::string("-1"));
}

BEZEL_TEST(null_cond_builtin_count_6024,
           non_null_array_count_is_the_real_count_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); "
        "    var b: Array<int>? := a; "
        "    Console.WriteLine(b?.Count ?? -1); ")),
        std::string("2"));
}

BEZEL_TEST(null_cond_builtin_count_6024,
           non_null_stringbuilder_length_is_the_real_length_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var sb := new StringBuilder(); "
        "    sb.Append(\"abc\"); "
        "    var b: StringBuilder? := sb; "
        "    Console.WriteLine(b?.Length ?? -1); ")),
        std::string("3"));
}

BEZEL_TEST(null_cond_builtin_count_6024,
           null_dictionary_count_falls_back_to_the_coalesce_operand_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var d: Dictionary<string, int>? := null; "
        "    var x := d?.Count; "
        "    Console.WriteLine(x ?? -1); ")),
        std::string("-1"));
}

BEZEL_TEST(null_cond_builtin_count_6024,
           null_set_count_falls_back_to_the_coalesce_operand_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var s: Set<int>? := null; "
        "    var x := s?.Count; "
        "    Console.WriteLine(x ?? -1); ")),
        std::string("-1"));
}

// ---------------------------------------------------------------------------
// THE TYPING ROW -- a nullable `?.` read may not initialize a NON-nullable
// local.  The pre-fix tree ACCEPTED this program and printed 0.
// ---------------------------------------------------------------------------
BEZEL_TEST(null_cond_builtin_count_6024,
           nullable_count_is_refused_at_a_non_nullable_initializer_7) {
    auto r = runAot(prog(
        "    var a: Array<int>? := null; "
        "    var n: int := a?.Count; "
        "    Console.WriteLine(n); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(refusedAsNullable(r));
}

// ---------------------------------------------------------------------------
// THE CONTROL ROWS -- the `string?.Length` spelling and the plain `.`
// spelling were never affected.  They pass on BOTH arms.
// ---------------------------------------------------------------------------
BEZEL_TEST(null_cond_builtin_count_6024,
           control_string_nullable_length_is_the_real_length_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var s: string? := \"ab\"; "
        "    Console.WriteLine(s?.Length ?? -1); ")),
        std::string("2"));
}

BEZEL_TEST(null_cond_builtin_count_6024,
           control_plain_dot_count_is_the_real_count_4_4) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); "
        "    Console.WriteLine(a.Count); ")),
        std::string("2"));
}
