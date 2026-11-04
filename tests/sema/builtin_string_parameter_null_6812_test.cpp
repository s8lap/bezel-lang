// Spec-derived regression test for issue #6812 — a builtin member's
// declared NON-NULLABLE `string` parameter accepted `null` outside the
// hand-enumerated #6782 table.
//
// THE SPEC.
//
//   spec §7.3 (v0.350) — null is not assignable to a non-nullable target,
//   so a `null` argument at a parameter declared `string` is a type error.
//
//   stdlib §3.2 (v0.350) declares `public abstract method GetBytes(s:
//   string): owned Array<byte>;` inside `public abstract class Encoding`.
//   stdlib §2.4 (v0.350) declares `Environment.GetInt(name: string): int`
//   and `Environment.GetBool(name: string): bool`.  stdlib §7.2 (v0.350)
//   declares the `File` async members, `ReadAllTextAsync(path: string)` among
//   them.
//
//   The issue's point is that these positions are OUTSIDE the table #6782
//   enumerated by hand from the stdlib, and so they were never checked:
//   `Encoding.UTF8.GetBytes(null)` compiled (rc 0) and ran (exit 0),
//   identically to the control `Encoding.UTF8.GetBytes("x")`.
//
// THE ORACLE IS POSITIVE.
//
// Every refusal row asserts the refusal, a REAL source location, and the
// POSITIVE diagnostic text - the member's name and the §7 / stdlib rule it
// cites.  The control row asserts the VALUE the well-typed call produces.
//
// No row asserts that a diagnostic is absent.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The §7.3 refusal at a declared non-nullable `string` parameter: the
// program did NOT run, the rejection is a located diagnostic at a REAL
// source location, and the diagnostic names the member and the rule.
void expectRefused(const JitResult& r, const char* member) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, member));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§7"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
}

}  // namespace

// ===========================================================================
// THE `Encoding` FAMILY — stdlib §3.2 declares `GetBytes(s: string)`.
//
// The `null` literal reaches the parameter as an `Object?`, and the
// declared source type is a `string` / `ReadOnlyStringSpan`, so the call is
// a type error.
// ===========================================================================
BEZEL_TEST(builtin_string_parameter_null_6812,
           encoding_getbytes_refuses_a_null_source) {
    auto r = runAot(prog(
        "    var b := Encoding.UTF8.GetBytes(null); "
        "    Console.WriteLine(b.Count.ToString()); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "Encoding.GetBytes"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §3.2"));
}

// ===========================================================================
// THE `Environment` FAMILY — stdlib §2.4 declares `GetInt(name: string)`
// and `GetBool(name: string)`.
//
// These are the positions the issue called out as measurable (their
// non-null controls carry the #2434 degrade note, so the null question
// could not be read from them; a REFUSAL makes it readable).
// ===========================================================================
BEZEL_TEST(builtin_string_parameter_null_6812,
           environment_getint_refuses_a_null_name) {
    auto r = runAot(prog(
        "    var v := Environment.GetInt(null); "
        "    Console.WriteLine(v.ToString()); "));
    expectRefused(r, "Environment.GetInt");
}

BEZEL_TEST(builtin_string_parameter_null_6812,
           environment_getbool_refuses_a_null_name) {
    auto r = runAot(prog(
        "    var v := Environment.GetBool(null); "
        "    Console.WriteLine(v.ToString()); "));
    expectRefused(r, "Environment.GetBool");
}

// ===========================================================================
// THE `File` ASYNC FAMILY — stdlib §7.3 declares `ReadAllTextAsync(path:
// string)`.
// ===========================================================================
BEZEL_TEST(builtin_string_parameter_null_6812,
           file_readalltextasync_refuses_a_null_path) {
    auto r = runAot(prog("    var t := File.ReadAllTextAsync(null); "));
    expectRefused(r, "File.ReadAllTextAsync");
}

// ===========================================================================
// THE CONTROL — the well-typed call is still ADMITTED and still produces its
// value: one byte for the one-character source.
// ===========================================================================
BEZEL_TEST(builtin_string_parameter_null_6812,
           encoding_getbytes_accepts_a_string_and_keeps_its_value) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var b := Encoding.UTF8.GetBytes(\"x\"); "
        "    Console.WriteLine(b.Count.ToString()); ")),
        std::string("1"));
}
