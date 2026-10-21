// Sema — #6805 / language spec §27.6 (v0.340) + stdlib §5 (string is
// `IEnumerable<char>`, §9.6).
//
// §27.6 lists extension methods among the reference-able members, but the §5
// operators are dispatched structurally (no method symbol), so a bare
// `s.First` over a `string` cannot form a function value. Before the fix the
// reference was left untyped and codegen degraded `f` to a ZERO function value
// (warning #2434); `f()` dereferenced null. The honest outcome until real
// support exists (#6784 for IGrouping / IEnumerable, #2470/#2492/#2537 for the
// builtin collections) is a located compile-time rejection at the formation.
// The call spelling, the parenthesised callee and a user extension with a
// non-§5 name stay accepted with exact output.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

constexpr const char* kPre =
    "module M; public class M public static method Main(): int "
    "  var s := \"hello\"; ";
constexpr const char* kPost = " end Main; end M;";

void expectRejected(const std::string& body, const std::string& member) {
    JitResult r = runAot(std::string(kPre) + body + kPost);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ("a bare reference to `" + member + "` (language spec \xC2\xA7"
            "27.6) is not yet supported on `string`").c_str()));
}

}  // namespace

// The verbatim #6805 repro.
BEZEL_TEST(string_bare_operator_reference_reject_6805, first_rejected_spec_27_6) {
    expectRejected(
        "var f: () -> char := s.First; Console.WriteLine(f()); return 0;",
        "First");
}
BEZEL_TEST(string_bare_operator_reference_reject_6805, last_untyped_target_rejected_spec_27_6) {
    expectRejected("var f := s.Last; return 0;", "Last");
}
BEZEL_TEST(string_bare_operator_reference_reject_6805, count_rejected_spec_27_6) {
    expectRejected("var f := s.Count; return 0;", "Count");
}
BEZEL_TEST(string_bare_operator_reference_reject_6805,
           null_conditional_first_rejected_spec_27_6) {
    expectRejected(
        "var t: string? := s; var f := t?.First; return 0;", "First");
}
BEZEL_TEST(string_bare_operator_reference_reject_6805,
           argument_position_rejected_spec_27_6) {
    expectRejected(
        "var q := s.Select(lambda(c: char) => c).Count(); "
        "var g: () -> char := s.Last; return q;",
        "Last");
}

// ---- Controls: the accepted spellings keep exact output ----
BEZEL_TEST(string_bare_operator_reference_reject_6805, call_form_prints_h_o_spec_5) {
    JitResult r = runAot(std::string(kPre) +
                         "Console.WriteLine(s.First()); "
                         "Console.WriteLine(s.Last()); return 0;" + kPost);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("h\no\n"));
}
BEZEL_TEST(string_bare_operator_reference_reject_6805,
           null_conditional_call_prints_h_spec_7_5) {
    JitResult r = runAot(std::string(kPre) +
                         "var t: string? := s; var c := t?.First(); "
                         "Console.WriteLine(c ?? 'x'); return 0;" + kPost);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("h\n"));
}
BEZEL_TEST(string_bare_operator_reference_reject_6805,
           lambda_spelling_prints_h_spec_27_6) {
    JitResult r = runAot(std::string(kPre) +
                         "var f: () -> char := lambda() => s.First(); "
                         "Console.WriteLine(f()); return 0;" + kPost);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("h\n"));
}
BEZEL_TEST(string_bare_operator_reference_reject_6805,
           parenthesised_callee_prints_h_spec_27_6) {
    JitResult r = runAot(std::string(kPre) +
                         "Console.WriteLine((s.First)()); return 0;" + kPost);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("h\n"));
}
// A string member that is not a §5 operator is not rejected by this check.
BEZEL_TEST(string_bare_operator_reference_reject_6805, length_read_unaffected_spec_9_6) {
    JitResult r = runAot(std::string(kPre) +
                         "Console.WriteLine(s.Length); return 0;" + kPost);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}
