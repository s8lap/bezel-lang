// Regression tests for issue #7949 — stdlib §2.4's `Environment.GetInt(name)`,
// `Environment.GetBool(name)` and `Environment.Variables()` reached NO codegen
// arm at all, so all three degraded.
//
// WHAT THE DOCUMENTS REQUIRE (stdlib §2.4, v0.355, quoted):
//
//     public static method Get(name: string): string?;    // null when the
//                                                        //   variable is
//                                                        //   absent
//     public static method GetInt(name: string): int?;    // convenience tier:
//                                                        //   null when the
//                                                        //   variable is
//                                                        //   absent OR
//                                                        //   malformed —
//                                                        //   deliberately
//                                                        //   conflated
//                                                        //   (recorded)
//     public static method GetBool(name: string): bool?;  //   GetBool accepts
//                                                        //   exactly
//                                                        //   Boolean.Parse's
//                                                        //   forms
//                                                        //   ("true"/"false",
//                                                        //   case-insensitive);
//                                                        //   "1"/"yes" are null
//     public static method Variables(): owned Dictionary<string, string>;
//
// and: "`Variables()` returns a freshly-allocated `owned Dictionary<string,
// string>` (per the §1 convention for fresh-instance returns)".
//
// MEASURED pre-fix with the real `build/bezelc` (base b0325c3c), one shape per
// member:
//
//   var n := Environment.GetInt("X"); Console.WriteLine(n ?? -1);
//     -> warning: internal: lowerLocalDecl's initializer expression failed to
//        lower to a value for this local's slot (degraded to a zero-initialized
//        value) — #2434
//     -> prints `0` — for an ABSENT variable (expected `-1`) and for a
//        variable just set to "42" (expected `42`).
//
//   Console.WriteLine(Environment.GetInt("X"));
//     -> warning: internal: the argument of this `Console.WriteLine` failed to
//        lower to a value, so the whole statement emits nothing — not even the
//        line terminator — #3079
//     -> prints NOTHING.
//
//   var b := Environment.GetBool("X"); Console.WriteLine(b ?? false);
//     -> the same #2434 degrade; prints `False`.
//
//   var v := Environment.Variables(); Console.WriteLine(v.Count > 0);
//     -> the same #2434 degrade; prints `False` for a process whose
//        environment holds dozens of variables.
//
// The second half of the defect is SEMANTIC: `Environment.GetInt` / `GetBool`
// carried NO result type in sema either, so an INFERRED local (`var n := ...`)
// had no type, and the `??` lowering's nullable-value arm — which is gated on
// the left operand's sema type — could not lower the coalesce at all. The
// tell: `var n: bool := Environment.GetInt("X");` was ADMITTED pre-fix (a
// typed `int` initializer is rejected against a `bool` slot), and
// `var g: bool := Environment.Get("X");` died in the IR verifier
// (`trunc ptr %12 to i1`) — the codegen produced a `bz_string*` for a `bool`
// slot.
//
// The fix is TWO halves that must land together (the #2348 commit that
// implemented `Get`/`Set` recorded the same deferral for `GetInt`/`GetBool`):
//   * the SEMANTIC result type (`int?` / `bool?`), in sema's static-builtin-
//     class accept-list; and
//   * the CODEGEN lowering, which produces §7.2's tagged companion struct
//     `{i8 hasValue; T value}` the two types name.
//
// ORACLES. Every row runs the real AOT path (`runAot`) and asserts the
// PRINTED VALUE — a compile-only assertion cannot tell "works" from "emits
// nothing", which is the whole defect class (#3079 / #2434). The two REJECTION
// rows assert the specific diagnostic and that the program never ran.
//
// Hermetic by construction: each test program is its own forked process, so
// `Set` mutates only that process's environment, and every variable name is
// prefixed `BZ_T_7949_` to avoid colliding with the harness's own.
//
// Cite: stdlib §2.4, §9.1 (the integer grammar), §9.4 (Boolean.Parse's forms),
// §4.8/§4.9 (the Dictionary surface), language spec §7 (the nullable-argument
// rule), §7.7 (`??`), §7.11 (the operators that do not lift over `T?`),
// v0.355.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& body) {
    return "module Conf;\npublic class Program\n"
           "  public static method Main()\n" + body +
           "  end Main;\nend Program;\n";
}

}  // namespace

// ═══════════ GetInt — stdlib §2.4's `int?` convenience tier ═══════════════

// The issue's own repro: the value of a variable that IS set.
BEZEL_TEST(environment_getfamily_7949, get_int_reads_back_a_set_value) {
    const std::string p = prog(
        "    Environment.Set(\"BZ_T_7949_N\", \"42\");\n"
        "    var n := Environment.GetInt(\"BZ_T_7949_N\");\n"
        "    Console.WriteLine(n ?? -1);");
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// §2.4: "null when the variable is absent" — so `n ?? -1` is `-1`.
BEZEL_TEST(environment_getfamily_7949, get_int_absent_variable_is_null) {
    const std::string p = prog(
        "    var n := Environment.GetInt(\"BZ_T_7949_ABSENT\");\n"
        "    Console.WriteLine(n ?? -1);");
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\n");
}

// §2.4's deliberate CONFLATION: "null when the variable is absent OR
// malformed". A malformed value is null, NOT zero and NOT an exception.
BEZEL_TEST(environment_getfamily_7949, get_int_malformed_value_is_null) {
    const std::string p = prog(
        "    Environment.Set(\"BZ_T_7949_J\", \"not-an-int\");\n"
        "    var n := Environment.GetInt(\"BZ_T_7949_J\");\n"
        "    Console.WriteLine(n ?? -1);");
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\n");
}

// stdlib §9.1's integer grammar: "an optional leading `+` or `-`, then one or
// more digits of that radix, and nothing else; no whitespace, no `0x`/`0b`
// prefix, no digit separators" — so a leading space is MALFORMED, and the
// value is null.
BEZEL_TEST(environment_getfamily_7949, get_int_leading_whitespace_is_null) {
    const std::string p = prog(
        "    Environment.Set(\"BZ_T_7949_W\", \" 42\");\n"
        "    var n := Environment.GetInt(\"BZ_T_7949_W\");\n"
        "    Console.WriteLine(n ?? -1);");
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\n");
}

// stdlib §9.1: "a value outside the type's range raising `OverflowException`"
// — the §2.4 tier's null-for-malformed rule turns the overflow into null.
BEZEL_TEST(environment_getfamily_7949, get_int_out_of_range_is_null) {
    const std::string p = prog(
        "    Environment.Set(\"BZ_T_7949_H\", \"99999999999999\");\n"
        "    var n := Environment.GetInt(\"BZ_T_7949_H\");\n"
        "    Console.WriteLine(n ?? -1);");
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\n");
}

// A negative value is a well-formed one.
BEZEL_TEST(environment_getfamily_7949, get_int_negative_value_is_read) {
    const std::string p = prog(
        "    Environment.Set(\"BZ_T_7949_I\", \"-7\");\n"
        "    var n := Environment.GetInt(\"BZ_T_7949_I\");\n"
        "    Console.WriteLine(n ?? -1);");
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "-7\n");
}

// The conformance corpus row l2_4_0004's shape: the result compares EQUAL to
// null for a malformed value ("null when the variable is absent OR malformed
// — deliberately conflated").
BEZEL_TEST(environment_getfamily_7949, get_int_malformed_compares_equal_to_null) {
    const std::string p = prog(
        "    Environment.Set(\"BZ_T_7949_EQ\", \"not-an-int\");\n"
        "    Console.WriteLine(Environment.GetInt(\"BZ_T_7949_EQ\") = null);");
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// ═══════════ GetBool — stdlib §2.4's `bool?` convenience tier ═════════════

// §2.4: "GetBool accepts exactly Boolean.Parse's forms ("true"/"false",
// case-insensitive)".
BEZEL_TEST(environment_getfamily_7949, get_bool_reads_back_true) {
    const std::string p = prog(
        "    Environment.Set(\"BZ_T_7949_T\", \"true\");\n"
        "    var b := Environment.GetBool(\"BZ_T_7949_T\");\n"
        "    Console.WriteLine(b ?? false);");
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// The same, case-insensitively ("TRUE").
BEZEL_TEST(environment_getfamily_7949, get_bool_is_case_insensitive) {
    const std::string p = prog(
        "    Environment.Set(\"BZ_T_7949_U\", \"TRUE\");\n"
        "    var b := Environment.GetBool(\"BZ_T_7949_U\");\n"
        "    Console.WriteLine(b ?? false);");
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// "false" is a well-formed FALSE — not null. The comparison form is the
// discriminator: a well-formed `false` compares EQUAL to `false`, while the
// pre-fix degraded local (the #2434 zero-init) was not even a boolean — the
// whole argument failed to lower and the statement emitted nothing (#3079),
// so this row is RED on the base.
BEZEL_TEST(environment_getfamily_7949, get_bool_reads_back_false) {
    const std::string p = prog(
        "    Environment.Set(\"BZ_T_7949_F\", \"false\");\n"
        "    Console.WriteLine(Environment.GetBool(\"BZ_T_7949_F\") = false);");
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// §2.4: `"1"/"yes" are null` — a well-formed false and a malformed value are
// DIFFERENT answers, and only the malformed one is null. `b ?? true` is the
// discriminator: a null `b` yields `true`, a well-formed `false` yields
// `false`.
BEZEL_TEST(environment_getfamily_7949, get_bool_one_is_null) {
    const std::string p = prog(
        "    Environment.Set(\"BZ_T_7949_1\", \"1\");\n"
        "    var b := Environment.GetBool(\"BZ_T_7949_1\");\n"
        "    Console.WriteLine(b ?? true);");
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// An ABSENT variable is null for GetBool too.
BEZEL_TEST(environment_getfamily_7949, get_bool_absent_variable_is_null) {
    const std::string p = prog(
        "    var b := Environment.GetBool(\"BZ_T_7949_BABSENT\");\n"
        "    Console.WriteLine(b ?? true);");
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// ═══════════ Variables — stdlib §2.4's `owned Dictionary<string, string>` ═══

// §2.4: "Variables() returns a freshly-allocated `owned Dictionary<string,
// string>`". The table holds the process environment, so a variable set just
// before the call is IN it, and the value reads back.
BEZEL_TEST(environment_getfamily_7949, variables_reads_the_process_environment) {
    const std::string p = prog(
        "    Environment.Set(\"BZ_T_7949_VAR\", \"hello\");\n"
        "    using var v := Environment.Variables();\n"
        "    Console.WriteLine(v.Count > 0);\n"
        "    Console.WriteLine(v[\"BZ_T_7949_VAR\"]);");
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nhello\n");
}

// A second call returns a FRESH table (§2.4's "freshly-allocated"), so a
// variable set AFTER the first call is not in the first table.
BEZEL_TEST(environment_getfamily_7949, variables_returns_a_fresh_table) {
    const std::string p = prog(
        "    using var v1 := Environment.Variables();\n"
        "    Environment.Set(\"BZ_T_7949_LATE\", \"x\");\n"
        "    using var v2 := Environment.Variables();\n"
        "    Console.WriteLine(v1.Count < v2.Count);");
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// ═══════════ The two REJECTION rows the typing makes exact ═════════════════

// stdlib §2.1: "No overload takes a nullable parameter, so a `null` argument
// — the literal, or an un-narrowed `string?` or `Object?` — has no
// applicable candidate and is a compile error (language spec §5.1, §22.6)".
// `Environment.GetInt` is an `int?`, so passing it straight to
// `Console.WriteLine` is that compile error — the caller narrows first
// (`??`) or prints it through an interpolation hole.
//
// Pre-fix this shape did NOT reject: the call was UNTYPED, so the §7 gate had
// nothing to fire on and the statement degraded to the #3079 "emits nothing".
BEZEL_TEST(environment_getfamily_7949, get_int_argument_at_a_non_nullable_parameter_is_rejected) {
    const std::string p = prog(
        "    Console.WriteLine(Environment.GetInt(\"BZ_T_7949_REJ\"));");
    auto r = runAot(p);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "is nullable but the target type is non-nullable"));
}

// spec §7.11 (v0.267): "the arithmetic, bitwise, or shift operators do not
// lift over `T?`" — so `GetInt(...) + 1` is a compile error, not a silent
// zero. Pre-fix the call was UNTYPED and the whole statement degraded.
BEZEL_TEST(environment_getfamily_7949, get_int_arithmetic_does_not_lift) {
    const std::string p = prog(
        "    Console.WriteLine(Environment.GetInt(\"BZ_T_7949_LIFT\") + 1);");
    auto r = runAot(p);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "is not defined on a nullable value type"));
}
