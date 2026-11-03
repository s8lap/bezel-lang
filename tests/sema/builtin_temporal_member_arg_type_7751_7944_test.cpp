// Regression test for issues #7751 and #7944 (spec §5.1, §22.6, §26.5 rule 1,
// stdlib §2.5, v0.350).
//
// THE DEFECTS (two issues, ONE mechanism).
//
// #7751: "ill-typed positional arguments to DateTime/TimeSpan/DateOnly/TimeOnly
// members are not refused (dt.Add(5) runs, dt.CompareTo("x") aborts IR)".
// stdlib §2.5 (v0.350) declares the temporal records' members with TYPED
// parameters:
//
//   public method Add(span: TimeSpan): DateTime;
//   public method Subtract(other: DateTime): TimeSpan;
//   public method Subtract(span: TimeSpan): DateTime;
//   public method CompareTo(other: DateTime): int;
//   public method CompareTo(other: TimeSpan): int;
//   public method CompareTo(other: DateOnly): int;
//   public method ToDateTime(time: TimeOnly): DateTime;
//   public method CompareTo(other: TimeOnly): int;
//
// but the ONE built-in parameter table's `kinds` column is a BIT SET over a
// vocabulary that names the SCALAR kinds and a few NAMED ones -- there is NO
// bit for a RECORD. So those EIGHT positions carried NO type opinion and the
// §5.1 gate asked no question at all: `dt.Add(5)` COMPILED and RAN, and
// `dt.CompareTo("x")` reached the IR verifier and aborted the compiler.
//
// #7944: "rule 1 (§26.5) hole on value built-ins: `d.Add(3)` on a DateTime is
// accepted with or without a same-named extension and prints garbage;
// `TimeSpan.CompareTo(3)` is accepted". The same position, seen from the
// §26.5 side: an extension named `Add` on a `DateTime` receiver TYPED the
// call, so the program printed the extension's value instead of the standard
// library's -- and with no extension at all the ill-typed call was still
// accepted.
//
// WHAT FIXED THEM.
//   * #7944's extension half: commit a27c4a93 ("fix #7970: the built-in value
//     records' declared members block same-named extensions") committed §26.5
//     rule 1 for the built-in value records, so the record's own member is the
//     answer and a same-named extension is not a candidate.
//   * BOTH issues' argument-type half: commit 0b154fdc ("fix #8012 #8015: a
//     member position with no type opinion is checked, and a named kind bit
//     matches the declared ENTITY") gave the §5.1 gate the DECLARED TYPE at
//     those eight positions (the table itself is not edited -- a table row
//     cannot spell a record type), so an argument that is not the declared
//     record is refused at the source location with a §5.1 / §22.6 diagnostic.
//
// Neither commit named #7751 or #7944 in a closing position: a27c4a93 names
// #7970 and 0b154fdc names #8012, #8015 and #8020. `scripts/pick_issues.sh
// --github` prints EXACTLY those, so #7751 and #7944 were never closed even
// though their defects were already gone from the tree. This file pins them.
//
// THE ORACLE. `runAot` is the strict AOT oracle (fork-isolated
// Driver::compileFile + clang link + native exec). Every REFUSAL row asserts
// the POSITIVE: the exact §5.1 / §22.6 diagnostic text the gate emits, at a
// REAL source location (failedAtCompileTimeWithRealLocation). Every CONTROL
// row asserts the exact stdout the standard library's member computes. No row
// asserts the absence of a diagnostic.
//
// ASan is OFF in lanes by default (CLAUDE.md): this is an argument's type
// admission and a diagnostic, not allocation, layout, retain/release or
// disposal.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

}  // namespace

namespace {

// The program body is wrapped in a class with a `Main` method. `prefix` is
// spliced at MODULE level -- an extension block is a module-level
// declaration, not a member of the class.
std::string prog(const std::string& prefix, const std::string& body) {
    return "module M;\n" + prefix +
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// A refusal row: the program must NOT run, the diagnostic must carry `needle`
// at a REAL source location, and the gate's own §5.1 reason must be present.
void expectRefused(const std::string& prefix, const std::string& body,
                   const char* needle, const char* reason) {
    JitResult r = runAot(prog(prefix, body));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, reason));
    // The diagnostic is LOCATED: the driver's captured stderr carries a
    // `<path>:<line>:<col>:` prefix, so the refusal has a real source
    // position rather than a synthesized one.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// A value row: the program must compile CLEAN, run, exit 0 and print exactly
// this stdout.
void expectStdout(const std::string& prefix, const std::string& body,
                  const std::string& expected) {
    JitResult r = runAot(prog(prefix, body));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') out.pop_back();
    BEZEL_EXPECT_EQ(out, expected);
}

}  // namespace

// ===========================================================================
// #7751 -- the ILL-TYPED POSITIONAL ARGUMENT is refused, per §5.1 / §22.6.
// ===========================================================================

// stdlib §2.5 declares `Add(span: TimeSpan): DateTime`, so an `int` at that
// position is never implicitly convertible to a `TimeSpan` (§5.1).
BEZEL_TEST(builtin_temporal_member_arg_type_7751_7944,
           date_time_add_of_an_int_is_refused_5_1) {
    expectRefused("",
                  "    var dt := DateTime.FromTicks(0L);\n"
                  "    var x := dt.Add(5);\n"
                  "    Console.WriteLine(x);\n",
                  "no matching overload of `DateTime.Add` — stdlib §2.5 declares "
                  "`Add(span: TimeSpan): DateTime`",
                  "a `int` is never implicitly convertible to a `TimeSpan` (§5.1)");
}

// stdlib §2.5 declares `CompareTo(other: DateTime): int`, so a `string` at
// that position is refused. Before the fix this call ABORTED the compiler in
// the IR verifier instead of being diagnosed.
BEZEL_TEST(builtin_temporal_member_arg_type_7751_7944,
           date_time_compare_to_of_a_string_is_refused_5_1) {
    expectRefused("",
                  "    var dt := DateTime.FromTicks(0L);\n"
                  "    var n := dt.CompareTo(\"x\");\n"
                  "    Console.WriteLine(n);\n",
                  "no matching overload of `DateTime.CompareTo` — stdlib §2.5 "
                  "declares `CompareTo(other: DateTime): int`",
                  "a `string` is never implicitly convertible to a `DateTime` (§5.1)");
}

// stdlib §2.5 declares `CompareTo(other: TimeSpan): int` on `TimeSpan`.
BEZEL_TEST(builtin_temporal_member_arg_type_7751_7944,
           time_span_compare_to_of_a_date_time_is_refused_5_1) {
    expectRefused("",
                  "    var ts := TimeSpan.FromTicks(0L);\n"
                  "    var dt := DateTime.FromTicks(0L);\n"
                  "    var n := ts.CompareTo(dt);\n"
                  "    Console.WriteLine(n);\n",
                  "no matching overload of `TimeSpan.CompareTo` — stdlib §2.5 "
                  "declares `CompareTo(other: TimeSpan): int`",
                  "a `DateTime` is never implicitly convertible to a `TimeSpan` (§5.1)");
}

// stdlib §2.5 declares `CompareTo(other: DateOnly): int` on `DateOnly`.
BEZEL_TEST(builtin_temporal_member_arg_type_7751_7944,
           date_only_compare_to_of_an_int_is_refused_5_1) {
    expectRefused("",
                  "    var d := new DateOnly(2020, 1, 2);\n"
                  "    var n := d.CompareTo(5);\n"
                  "    Console.WriteLine(n);\n",
                  "no matching overload of `DateOnly.CompareTo` — stdlib §2.5 "
                  "declares `CompareTo(other: DateOnly): int`",
                  "a `int` is never implicitly convertible to a `DateOnly` (§5.1)");
}

// stdlib §2.5 declares `ToDateTime(time: TimeOnly): DateTime` on `DateOnly`.
BEZEL_TEST(builtin_temporal_member_arg_type_7751_7944,
           date_only_to_date_time_of_an_int_is_refused_5_1) {
    expectRefused("",
                  "    var d := new DateOnly(2020, 1, 2);\n"
                  "    var x := d.ToDateTime(3);\n"
                  "    Console.WriteLine(x);\n",
                  "no matching overload of `DateOnly.ToDateTime` — stdlib §2.5 "
                  "declares `ToDateTime(time: TimeOnly): DateTime`",
                  "a `int` is never implicitly convertible to a `TimeOnly` (§5.1)");
}

// stdlib §2.5 declares `CompareTo(other: TimeOnly): int` on `TimeOnly`.
BEZEL_TEST(builtin_temporal_member_arg_type_7751_7944,
           time_only_compare_to_of_an_int_is_refused_5_1) {
    expectRefused("",
                  "    var t := new TimeOnly(1, 2, 3);\n"
                  "    var n := t.CompareTo(5);\n"
                  "    Console.WriteLine(n);\n",
                  "no matching overload of `TimeOnly.CompareTo` — stdlib §2.5 "
                  "declares `CompareTo(other: TimeOnly): int`",
                  "a `int` is never implicitly convertible to a `TimeOnly` (§5.1)");
}

// stdlib §2.5 declares BOTH `Subtract(other: DateTime): TimeSpan` and
// `Subtract(span: TimeSpan): DateTime`, so the gate names the two forms and
// the two declared types.
BEZEL_TEST(builtin_temporal_member_arg_type_7751_7944,
           date_time_subtract_of_an_int_is_refused_5_1) {
    expectRefused("",
                  "    var dt := DateTime.FromTicks(0L);\n"
                  "    var x := dt.Subtract(5);\n"
                  "    Console.WriteLine(x);\n",
                  "no matching overload of `DateTime.Subtract` — stdlib §2.5 "
                  "declares `Subtract(other: DateTime): TimeSpan` / "
                  "`Subtract(span: TimeSpan): DateTime`",
                  "a `int` is never implicitly convertible to a `DateTime` or a "
                  "`TimeSpan` (§5.1)");
}

// ===========================================================================
// #7944 -- the SAME position, seen from the §26.5 side: a same-named
// extension does not type the call, and the ill-typed call is refused.
// ===========================================================================

// §26.5 rule 1 (v0.350): where the receiver's declared type declares a
// member of that name -- "of any arity, applicability, or accessibility" --
// the instance resolution is COMMITTED and extension blocks are never
// consulted for that name on that receiver. The extension below declares
// `Add(x: int): int`, so before the fix it TYPED `d.Add(3)` by its own `int`
// and the program printed the extension's value (garbage) instead of the
// standard library's.
BEZEL_TEST(builtin_temporal_member_arg_type_7751_7944,
           a_same_named_extension_does_not_type_the_declared_member_26_5) {
    expectRefused("extension DateTime\n"
                  "  public method Add(x: int): int => 7;\n"
                  "end;\n",
                  "    var d: DateTime := DateTime.FromUnixTimeSeconds(0L);\n"
                  "    Console.WriteLine(d.Add(3));\n",
                  "no matching overload of `DateTime.Add` — stdlib §2.5 declares "
                  "`Add(span: TimeSpan): DateTime`",
                  "a `int` is never implicitly convertible to a `TimeSpan` (§5.1)");
}

// #7944's SECOND arm: "With no extension, `d.Add(3)` is accepted although
// stdlib §2.5 declares only `Add(span: TimeSpan)`". The receiver here is an
// ANNOTATED local (`var d: DateTime := …`), so the call is not a read of an
// inferred value -- it is the §2.5 record's own member with an ill-typed
// argument.
BEZEL_TEST(builtin_temporal_member_arg_type_7751_7944,
           an_annotated_receiver_with_no_extension_still_refuses_the_ill_typed_call) {
    expectRefused("",
                  "    var d: DateTime := DateTime.FromUnixTimeSeconds(0L);\n"
                  "    Console.WriteLine(d.Add(3));\n",
                  "no matching overload of `DateTime.Add` — stdlib §2.5 declares "
                  "`Add(span: TimeSpan): DateTime`",
                  "a `int` is never implicitly convertible to a `TimeSpan` (§5.1)");
}

// #7944's THIRD arm: `TimeSpan.CompareTo(3)` was ACCEPTED and printed 1.
BEZEL_TEST(builtin_temporal_member_arg_type_7751_7944,
           time_span_compare_to_of_an_int_is_refused_5_1) {
    expectRefused("",
                  "    var ts := TimeSpan.FromTicks(5L);\n"
                  "    var n := ts.CompareTo(3);\n"
                  "    Console.WriteLine(n);\n",
                  "no matching overload of `TimeSpan.CompareTo` — stdlib §2.5 "
                  "declares `CompareTo(other: TimeSpan): int`",
                  "a `int` is never implicitly convertible to a `TimeSpan` (§5.1)");
}

// ===========================================================================
// THE CONTROL ROWS -- the conforming calls still compute stdlib §2.5's
// values. This is what proves the gate refuses the WRONG argument type and
// not the member.
// ===========================================================================

// `Add(span: TimeSpan): DateTime` adds the span: one hour is
// 3 600 s x 10 000 000 ticks/s = 36 000 000 000 ticks (stdlib §2.5's tick
// contract).
BEZEL_TEST(builtin_temporal_member_arg_type_7751_7944,
           the_declared_add_still_computes_the_span_2_5) {
    expectStdout("", "    var dt := DateTime.FromTicks(0L);\n"
                 "    var x := dt.Add(TimeSpan.FromTicks(36000000000L));\n"
                 "    Console.WriteLine(x.Ticks);\n",
                 "36000000000");
}

// `CompareTo` is tick order (stdlib §2.5): -1 when the receiver is earlier.
BEZEL_TEST(builtin_temporal_member_arg_type_7751_7944,
           the_declared_compare_to_still_computes_tick_order_2_5) {
    expectStdout("", "    var dt := DateTime.FromTicks(0L);\n"
                 "    var other := DateTime.FromTicks(100L);\n"
                 "    Console.WriteLine(dt.CompareTo(other));\n"
                 "    Console.WriteLine(other.CompareTo(dt));\n",
                 "-1\n1");
}

// The same for `TimeSpan`.
BEZEL_TEST(builtin_temporal_member_arg_type_7751_7944,
           the_declared_time_span_compare_to_still_computes_tick_order_2_5) {
    expectStdout("", "    var ts := TimeSpan.FromTicks(0L);\n"
                 "    var other := TimeSpan.FromTicks(100L);\n"
                 "    Console.WriteLine(ts.CompareTo(other));\n",
                 "-1");
}

// `DateOnly.ToDateTime(time: TimeOnly): DateTime` composes the two values.
BEZEL_TEST(builtin_temporal_member_arg_type_7751_7944,
           the_declared_to_date_time_still_composes_2_5) {
    expectStdout("", "    var d := new DateOnly(2020, 1, 2);\n"
                 "    var t := new TimeOnly(3, 4, 5);\n"
                 "    var x := d.ToDateTime(t);\n"
                 "    Console.WriteLine(x);\n",
                 "2020-01-02T03:04:05");
}

// `TimeOnly.CompareTo(other: TimeOnly): int`.
BEZEL_TEST(builtin_temporal_member_arg_type_7751_7944,
           the_declared_time_only_compare_to_still_computes_2_5) {
    expectStdout("", "    var t := new TimeOnly(1, 2, 3);\n"
                 "    var t2 := new TimeOnly(1, 2, 4);\n"
                 "    Console.WriteLine(t.CompareTo(t2));\n",
                 "-1");
}

// `Subtract(other: DateTime): TimeSpan` -- the difference in ticks.
BEZEL_TEST(builtin_temporal_member_arg_type_7751_7944,
           the_declared_subtract_still_computes_the_difference_2_5) {
    expectStdout("", "    var dt := DateTime.FromTicks(100L);\n"
                 "    var other := DateTime.FromTicks(0L);\n"
                 "    var x := dt.Subtract(other);\n"
                 "    Console.WriteLine(x.Ticks);\n",
                 "100");
}

// The CONFORMING member call, with an extension of the SAME NAME declared
// beside it: §26.5 rule 1 says the record's own member is the answer, so the
// extension is not a candidate and the standard library's value is computed.
BEZEL_TEST(builtin_temporal_member_arg_type_7751_7944,
           a_same_named_extension_does_not_type_the_declared_member_2_5) {
    expectStdout("extension DateTime\n"
                 "  public method Add(x: int): int => 7;\n"
                 "end;\n",
                 "    var d: DateTime := DateTime.FromTicks(0L);\n"
                 "    var x := d.Add(TimeSpan.FromTicks(36000000000L));\n"
                 "    Console.WriteLine(x.Ticks);\n",
                 "36000000000");
}
