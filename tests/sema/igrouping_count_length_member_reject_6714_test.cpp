// Sema — #6714 / stdlib §5 `IGrouping<TKey, T>` + §4.12 (v0.339).
//
// stdlib §5 declares `IGrouping<TKey, T>: IEnumerable<T>` with exactly one
// member, `readonly property Key: TKey`. `IEnumerable<T>` (§4.12) declares only
// `GetEnumerator`; the element count is §5's `Count()` OPERATOR, spelled with
// parentheses. So the bare `g.Count` / `g.Length` have no member to resolve.
// Before the fix sema left them untyped and codegen emitted malformed IR
// ("add i32, ptr") or silently dropped the whole statement (#3079 warning).
// A reject is the right answer, matching #6389's IEnumerable<T> arms.
//
// Sibling shapes pinned as ACCEPTED with values: `g.Key`, `g.Count()`,
// `g.First()`, `for var v in g`, and the same on an IGrouping-typed local.

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
    "  var a: Array<int> := [1, 2, 2, 3, 3, 3]; ";
constexpr const char* kPost = " end Main; end M;";

void expectRejected(const std::string& body, const std::string& needle) {
    JitResult r = runAot(std::string(kPre) + body + kPost);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle.c_str()));
}

void expectValue(const std::string& body, int value) {
    JitResult r = runAot(std::string(kPre) + body + kPost);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, value);
}

const char* kG = "a.GroupBy(lambda(x: int) => x)";

}  // namespace

// The issue's own repro: lambda parameter, Select.
BEZEL_TEST(igrouping_count_length_member_reject_6714,
           select_lambda_param_count_property_rejected_spec_5) {
    expectRejected(
        std::string("Console.WriteLine(") + kG +
            ".Select(lambda(g: IGrouping<int, int>) => g.Count).Sum()); "
            "return 0;",
        "no member `Count` on `IGrouping<TKey, T>`");
}

BEZEL_TEST(igrouping_count_length_member_reject_6714,
           select_lambda_param_length_property_rejected_spec_5) {
    expectRejected(
        std::string("Console.WriteLine(") + kG +
            ".Select(lambda(g: IGrouping<int, int>) => g.Length).Sum()); "
            "return 0;",
        "no member `Length` on `IGrouping<TKey, T>`");
}

// Where / OrderBy lambdas were silently ACCEPTED before (untyped member).
BEZEL_TEST(igrouping_count_length_member_reject_6714,
           where_lambda_count_property_rejected_spec_5) {
    expectRejected(
        std::string("return ") + kG +
            ".Where(lambda(g: IGrouping<int, int>) => g.Count > 1).Count();",
        "no member `Count` on `IGrouping<TKey, T>`");
}

BEZEL_TEST(igrouping_count_length_member_reject_6714,
           orderby_lambda_count_property_rejected_spec_5) {
    expectRejected(
        std::string("return ") + kG +
            ".OrderBy(lambda(g: IGrouping<int, int>) => g.Count).Count();",
        "no member `Count` on `IGrouping<TKey, T>`");
}

// For-loop variable and an IGrouping-typed local.
BEZEL_TEST(igrouping_count_length_member_reject_6714,
           for_var_count_property_rejected_spec_5) {
    expectRejected(
        std::string("for var g in ") + kG +
            " do Console.WriteLine(g.Count); end; return 0;",
        "no member `Count` on `IGrouping<TKey, T>`");
}

BEZEL_TEST(igrouping_count_length_member_reject_6714,
           typed_local_count_property_rejected_spec_5) {
    expectRejected(
        std::string("for var g in ") + kG +
            " do var h: IGrouping<int, int> := g; "
            "Console.WriteLine(h.Count); end; return 0;",
        "no member `Count` on `IGrouping<TKey, T>`");
}

BEZEL_TEST(igrouping_count_length_member_reject_6714,
           typed_local_length_property_rejected_spec_5) {
    expectRejected(
        std::string("for var g in ") + kG +
            " do var h: IGrouping<int, int> := g; "
            "Console.WriteLine(h.Length); end; return 0;",
        "no member `Length` on `IGrouping<TKey, T>`");
}

// ---- Controls: the declared surface keeps compiling with right values ----

// Key sum 1+2+3 = 6.
BEZEL_TEST(igrouping_count_length_member_reject_6714,
           key_property_still_works_spec_5) {
    expectValue(std::string("return ") + kG +
                    ".Select(lambda(g: IGrouping<int, int>) => g.Key).Sum();",
                6);
}

// Count() operator: 1+2+3 = 6.
BEZEL_TEST(igrouping_count_length_member_reject_6714,
           count_operator_still_works_spec_5) {
    expectValue(std::string("return ") + kG +
                    ".Select(lambda(g: IGrouping<int, int>) => g.Count()).Sum();",
                6);
}

BEZEL_TEST(igrouping_count_length_member_reject_6714,
           first_operator_still_works_spec_5) {
    expectValue(std::string("return ") + kG +
                    ".Select(lambda(g: IGrouping<int, int>) => g.First()).Sum();",
                6);
}

// for var v in g over each group: 1 + 4 + 9 = 14.
BEZEL_TEST(igrouping_count_length_member_reject_6714,
           iterate_group_still_works_spec_5) {
    expectValue(std::string("var s: int := 0; for var g in ") + kG +
                    " do for var v in g do s := s + v; end; end; return s;",
                1 + 2 + 2 + 3 + 3 + 3);
}

// Typed local: Count() + Key + First() = (1+1+1) + (2+2+2) + (3+3+3) = 18.
BEZEL_TEST(igrouping_count_length_member_reject_6714,
           typed_local_members_still_work_spec_5) {
    expectValue(std::string("var s: int := 0; for var g in ") + kG +
                    " do var h: IGrouping<int, int> := g; "
                    "s := s + h.Count() + h.Key + h.First(); end; return s;",
                18);
}

// The `?.` spelling reaches a separate sema block (#6512's); before the fix it
// was left untyped and the statement was dropped behind an `internal:` warning.
BEZEL_TEST(igrouping_count_length_member_reject_6714,
           null_conditional_count_property_rejected_spec_5) {
    expectRejected(
        std::string("for var g in ") + kG +
            " do var h: IGrouping<int, int>? := g; "
            "Console.WriteLine(h?.Count); end; return 0;",
        "no member `Count` on `IGrouping<TKey, T>`");
}

BEZEL_TEST(igrouping_count_length_member_reject_6714,
           null_conditional_length_property_rejected_spec_5) {
    expectRejected(
        std::string("for var g in ") + kG +
            " do var h: IGrouping<int, int>? := g; "
            "Console.WriteLine(h?.Length); end; return 0;",
        "no member `Length` on `IGrouping<TKey, T>`");
}
