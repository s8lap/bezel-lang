// Sema — #6784 / language spec §27.6 (v0.340) + stdlib §5 `IGrouping<TKey, T>`.
//
// §27.6: "Method references work for static methods, instance methods (the
// instance is captured at formation ...), properties, and extension methods."
// A bare `g.First` over an `IGrouping<int, int>` loop variable (and the other
// §5 operators `IGrouping` / `IEnumerable<T>` inherit) names a real member, but
// the compiler dispatches the §5 operators structurally (no method symbol), so
// no function value can be formed for the reference. Before the fix the
// reference was left untyped, codegen degraded `f` to a ZERO function value
// (warning #2434) and `f()` SEGV'd (exit 139). The honest outcome until real
// support exists (the #2470/#2492/#2537 precedent for the builtin collections)
// is a located compile-time rejection at the formation. The call spelling
// `g.First()` and the lambda spelling `lambda() => g.First()` stay accepted
// with exact output.

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
    "  var a: Array<int> := [1, 2, 2, 3]; ";
constexpr const char* kPost = " end Main; end M;";
const char* kG = "a.GroupBy(lambda(x: int) => x)";

void expectRejected(const std::string& body, const std::string& member,
                    const std::string& recv) {
    JitResult r = runAot(std::string(kPre) + body + kPost);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ("a bare reference to `" + member + "` (language spec \xC2\xA7"
            "27.6) is not yet supported on `" + recv + "`").c_str()));
}

std::string stmt(const std::string& ty, const std::string& init) {
    return std::string("for var g in ") + kG + " do var f: " + ty +
           " := " + init + "; Console.WriteLine(f()); end; return 0;";
}

}  // namespace

// The verbatim #6784 repro.
BEZEL_TEST(igrouping_bare_operator_reference_reject_6784, first_rejected_spec_27_6) {
    expectRejected(stmt("() -> int", "g.First"), "First", "IGrouping<K, T>");
}
BEZEL_TEST(igrouping_bare_operator_reference_reject_6784, last_rejected_spec_27_6) {
    expectRejected(stmt("() -> int", "g.Last"), "Last", "IGrouping<K, T>");
}
BEZEL_TEST(igrouping_bare_operator_reference_reject_6784, sum_rejected_spec_27_6) {
    expectRejected(stmt("() -> int", "g.Sum"), "Sum", "IGrouping<K, T>");
}
BEZEL_TEST(igrouping_bare_operator_reference_reject_6784, any_rejected_spec_27_6) {
    expectRejected(stmt("() -> bool", "g.Any"), "Any", "IGrouping<K, T>");
}
BEZEL_TEST(igrouping_bare_operator_reference_reject_6784, tostring_rejected_spec_27_6) {
    expectRejected(stmt("() -> string", "g.ToString"), "ToString",
                   "IGrouping<K, T>");
}
BEZEL_TEST(igrouping_bare_operator_reference_reject_6784,
           null_conditional_first_rejected_spec_27_6) {
    expectRejected(
        std::string("for var g in ") + kG +
            " do var h: IGrouping<int, int>? := g; var f := h?.First; end; "
            "return 0;",
        "First", "IGrouping<K, T>");
}
BEZEL_TEST(igrouping_bare_operator_reference_reject_6784,
           lambda_position_rejected_spec_27_6) {
    expectRejected(std::string("var n := ") + kG +
                       ".Select(lambda(g: IGrouping<int, int>) => g.First)"
                       ".Count(); return 0;",
                   "First", "IGrouping<K, T>");
}
BEZEL_TEST(igrouping_bare_operator_reference_reject_6784,
           ienumerable_first_rejected_spec_27_6) {
    expectRejected(
        "var e: IEnumerable<int> := a; var f: () -> int := e.First; "
        "Console.WriteLine(f()); return 0;",
        "First", "IEnumerable<T>");
}

// ---- Controls: the accepted spellings keep exact output ----
BEZEL_TEST(igrouping_bare_operator_reference_reject_6784,
           call_form_prints_1_2_3_spec_5) {
    JitResult r = runAot(std::string(kPre) + "for var g in " + kG +
                         " do var x := g.First(); Console.WriteLine(x); end; "
                         "return 0;" + kPost);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n2\n3\n"));
}
BEZEL_TEST(igrouping_bare_operator_reference_reject_6784,
           lambda_spelling_prints_1_2_3_spec_27_6) {
    JitResult r = runAot(std::string(kPre) + "for var g in " + kG +
                         " do var f: () -> int := lambda() => g.First(); "
                         "Console.WriteLine(f()); end; return 0;" + kPost);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n2\n3\n"));
}
// `Key` is a real property (typed), unaffected.
BEZEL_TEST(igrouping_bare_operator_reference_reject_6784, key_read_unaffected_spec_5) {
    JitResult r = runAot(std::string(kPre) + "for var g in " + kG +
                         " do Console.WriteLine(g.Key); end; return 0;" + kPost);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n2\n3\n"));
}
