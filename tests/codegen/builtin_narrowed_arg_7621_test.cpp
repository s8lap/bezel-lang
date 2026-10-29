// Spec-derived regression test for issue #7621 — a nullable VALUE-typed
// argument at a NON-nullable value parameter of a BUILT-IN member (campaign 2,
// builtin-call-admission, lane C2-24).
//
// ─── THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ─────────────────────
//
// spec §5.1 "Nullable conversions": `T` converts to `T?`, one way; a `T?`
//   reaches no non-nullable parameter. spec §7 / §7.3: an un-narrowed `T?`
//   at a `T` parameter is a compile error ("the argument ... is nullable but
//   the target type is non-nullable"); `!` (§7.8), `??` (§7.7) or a §7.4
//   narrowing is required.
// spec §7.4 "How narrowing is established": a null test, an early exit, and
//   the declaration's own assignment (`var d: double? := 1.5;`) narrow a LOCAL
//   non-null. spec §22.6: "a read of a binding §7.4 has narrowed non-null is
//   of its non-nullable type". So a NARROWED nullable read is a LEGAL argument
//   at a `T` parameter and must evaluate as the `T` it denotes.
//
// ─── THE DEFECT (MEASURED on staging 9867034f) ──────────────────────────────
//
// The issue reports `Math.Sqrt(d)` / `s.Substring(i)` as "not rejected": those
// reads are declaration-NARROWED (`var d: double? := 1.5;`), so accepting them
// is what §7.4 / §22.6 require (and the user-method parameter accepts the same
// shape). The real defect is the other half: `Double.IsNaN(d)`,
// `Char.IsDigit(c)`, `TimeSpan.FromSeconds(d)`, `DateTime.AddDays(d)`,
// `StringBuilder.Append(i)` ... over a narrowed read passed the
// `{i8 hasValue; T}` carrier where the bare `T` is expected and the compiler
// aborted in the LLVM verifier ("IR verification failed"). Every UN-narrowed
// nullable was already refused at all of these (control rows).
//
// The oracle is the program's OUTPUT (accept rows) or a located §7 diagnostic
// (refuse rows).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::diagnosticMessageContains;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; import System.Text; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// A program whose nullable PARAMETER `x` is never narrowed (§7.4).
std::string paramProg(const std::string& type, const std::string& body) {
    return "module M; import System; import System.Text; "
           "class C "
           "  public static method Main(): int "
           "    return F(null); "
           "  end Main; "
           "  public static method F(x: " + type + "): int " +
           body +
           "    return 0; "
           "  end F; "
           "end C;";
}

// A nullable PARAMETER `x` called with a real value, so `x!` / `x ?? v` run.
std::string paramRun(const std::string& type, const std::string& arg,
                     const std::string& body) {
    return "module M; import System; import System.Text; "
           "class C "
           "  public static method Main(): int "
           "    return F(" + arg + "); "
           "  end Main; "
           "  public static method F(x: " + type + "): int " +
           body +
           "    return 0; "
           "  end F; "
           "end C;";
}

void expectNullableRefused(const std::string& type, const std::string& stmt,
                           const char* callee) {
    auto r = runAot(paramProg(type, stmt));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("the argument to `") + callee +
            "` is nullable but the target type is non-nullable").c_str()));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}

}  // namespace

// ===========================================================================
// ACCEPT: a §7.4-narrowed read is the non-nullable type it denotes (§22.6).
// ===========================================================================

BEZEL_TEST(builtin_narrowed_arg_7621, double_is_nan_declaration_narrowed) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var d: double? := 1.5; Console.WriteLine(Double.IsNaN(d)); "
        "    Console.WriteLine(Double.IsFinite(d)); "
        "    Console.WriteLine(Double.IsInfinity(d));")),
        "False\nTrue\nFalse");
}

BEZEL_TEST(builtin_narrowed_arg_7621, double_is_nan_of_narrowed_int_widens) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var i: int? := 3; Console.WriteLine(Double.IsNaN(i));")),
        "False");
}

BEZEL_TEST(builtin_narrowed_arg_7621, float_predicates_null_test_narrowed) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var f: float? := 1.5f; "
        "    if f <> null then "
        "      Console.WriteLine(Float.IsNaN(f)); "
        "      Console.WriteLine(Float.IsInfinity(f)); "
        "    end;")),
        "False\nFalse");
}

BEZEL_TEST(builtin_narrowed_arg_7621, ieee_and_bitwise_equals_narrowed) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var d: double? := 1.5; var f: float? := 2.5f; "
        "    Console.WriteLine(Double.IeeeEquals(d, d)); "
        "    Console.WriteLine(Double.BitwiseEquals(d, 1.5)); "
        "    Console.WriteLine(Float.IeeeEquals(f, 2.5f)); "
        "    Console.WriteLine(Float.BitwiseEquals(f, f));")),
        "True\nTrue\nTrue\nTrue");
}

BEZEL_TEST(builtin_narrowed_arg_7621, char_statics_narrowed) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var c: char? := 'a'; "
        "    Console.WriteLine(Char.IsDigit(c)); "
        "    Console.WriteLine(char.IsLetter(c)); "
        "    Console.WriteLine(Char.IsUpper(c)); "
        "    Console.WriteLine(Char.ToUpper(c));")),
        "False\nTrue\nFalse\nA");
}

BEZEL_TEST(builtin_narrowed_arg_7621, timespan_from_narrowed) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var d: double? := 2.0; var i: int? := 3; "
        "    Console.WriteLine(TimeSpan.FromSeconds(d).TotalSeconds); "
        "    Console.WriteLine(TimeSpan.FromDays(i).TotalDays);")),
        "2\n3");
}

BEZEL_TEST(builtin_narrowed_arg_7621, datetime_add_narrowed) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var i: int? := 2; var d: double? := 1.0; "
        "    var t := new DateTime(2020, 1, 31); "
        "    Console.WriteLine(t.AddMonths(i).Month); "
        "    Console.WriteLine(t.AddDays(d).Day);")),
        "3\n1");
}

BEZEL_TEST(builtin_narrowed_arg_7621, string_builder_append_narrowed) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var i: int? := 7; var c: char? := 'x'; var d: double? := 1.5; "
        "    var sb := new StringBuilder(); "
        "    sb.Append(i); sb.Append(c); sb.Append(d); "
        "    Console.WriteLine(sb.ToString());")),
        "7x1.5");
}

BEZEL_TEST(builtin_narrowed_arg_7621, string_members_narrowed_unchanged) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var i: int? := 1; var c: char? := 'b'; var s := \"abcd\"; "
        "    Console.WriteLine(s.Substring(i)); "
        "    Console.WriteLine(s.IndexOf(c)); "
        "    Console.WriteLine(Math.Sqrt(4.0 + 0.0)); ")),
        "bcd\n1\n2");
}

BEZEL_TEST(builtin_narrowed_arg_7621, math_narrowed_unchanged) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var d: double? := 2.25; "
        "    Console.WriteLine(Math.Sqrt(d)); "
        "    Console.WriteLine(Math.Max(d, 1.0));")),
        "1.5\n2.25");
}

// `!` (§7.8) and `??` (§7.7) at the same positions.
BEZEL_TEST(builtin_narrowed_arg_7621, bang_and_coalesce_accepted) {
    BEZEL_EXPECT_EQ(runAotOut(paramRun("double?", "1.5",
        "    Console.WriteLine(Double.IsNaN(x!)); "
        "    Console.WriteLine(Double.IsNaN(x ?? 0.0)); "
        "    Console.WriteLine(TimeSpan.FromSeconds(x!).TotalSeconds); "
        "    Console.WriteLine(Math.Sqrt(x ?? 4.0));")),
        "False\nFalse\n1.5\n1.224744871391589");
    BEZEL_EXPECT_EQ(runAotOut(paramRun("char?", "'q'",
        "    Console.WriteLine(Char.IsDigit(x!)); "
        "    Console.WriteLine(Char.ToUpper(x ?? 'z'));")),
        "False\nQ");
}

// A null-test-narrowed PARAMETER (not only a local) reads at its payload type.
BEZEL_TEST(builtin_narrowed_arg_7621, narrowed_parameter_via_early_exit) {
    BEZEL_EXPECT_EQ(runAotOut(paramRun("double?", "1.5",
        "    if x = null then return 1; end; "
        "    Console.WriteLine(Double.IsNaN(x)); "
        "    Console.WriteLine(TimeSpan.FromMinutes(x).TotalMinutes);")),
        "False\n1.5");
}

// ===========================================================================
// REFUSE (controls, green before the fix too): an UN-narrowed nullable, a
// reassigned-from-null local, and a `null` literal reach no non-nullable
// parameter (§5.1, §7, §7.3, §7.4).
// ===========================================================================

BEZEL_TEST(builtin_narrowed_arg_7621, refuse_unnarrowed_double_at_statics) {
    expectNullableRefused("double?", "    var z := Double.IsNaN(x);", "Double.IsNaN");
    expectNullableRefused("double?", "    var z := Double.IeeeEquals(x, 1.0);", "Double.IeeeEquals");
    expectNullableRefused("double?", "    var z := Math.Sqrt(x);", "Math.Sqrt");
}

BEZEL_TEST(builtin_narrowed_arg_7621, refuse_unnarrowed_int_at_double_static) {
    expectNullableRefused("int?", "    var z := Double.IsNaN(x);", "Double.IsNaN");
}

BEZEL_TEST(builtin_narrowed_arg_7621, refuse_unnarrowed_float_char) {
    expectNullableRefused("float?", "    var z := Float.IsNaN(x);", "Float.IsNaN");
    expectNullableRefused("char?", "    var z := char.IsDigit(x);", "char.IsDigit");
    expectNullableRefused("char?", "    var z := Char.ToUpper(x);", "Char.ToUpper");
}

BEZEL_TEST(builtin_narrowed_arg_7621, refuse_unnarrowed_string_members) {
    expectNullableRefused("int?", "    var z := \"abc\".Substring(x);", "string.Substring");
    expectNullableRefused("int?", "    var z := \"abc\".PadLeft(x);", "string.PadLeft");
    expectNullableRefused("char?", "    var z := \"abc\".IndexOf(x);", "string.IndexOf");
    expectNullableRefused("int?", "    var z := \"abc\".Repeat(x);", "string.Repeat");
}

BEZEL_TEST(builtin_narrowed_arg_7621, refuse_unnarrowed_temporal) {
    expectNullableRefused("double?", "    var z := TimeSpan.FromSeconds(x);", "TimeSpan.FromSeconds");
    expectNullableRefused("double?", "    var z := DateTime.Now.AddDays(x);", "DateTime.AddDays");
    expectNullableRefused("int?", "    var z := DateTime.Now.AddMonths(x);", "DateTime.AddMonths");
}

BEZEL_TEST(builtin_narrowed_arg_7621, refuse_reassigned_from_null_local) {
    auto r = runAot(prog(
        "    var d: double? := 1.5; d := null; "
        "    var z := Double.IsNaN(d);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument to `Double.IsNaN` is nullable but the target type is non-nullable"));
}

BEZEL_TEST(builtin_narrowed_arg_7621, refuse_null_literal) {
    auto r = runAot(prog("    var z := Double.IsNaN(null);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
