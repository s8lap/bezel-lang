// Sema — #6740 (generalises #6714) / stdlib §5 `IGrouping<TKey, T>` + §4.12
// (v0.340).
//
// `IGrouping<TKey, T>` declares exactly one member, `Key`; it inherits
// `IEnumerable<T>` (§4.12: `GetEnumerator` only) and §5's operators. Any other
// bare (non-callee) member name — `Foo`, `Keys`, `Size`, `Capacity`, `Count2` —
// names nothing. Left untyped, such a read reached codegen (malformed IR or a
// statement dropped behind an `internal:` warning). #6714 rejected only
// `Count` / `Length`; this pins every other undeclared name, in the `.` and the
// `?.` arm, in lambda and statement positions. Operators in call position,
// `Key`, and `for var v in g` stay accepted with values.

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
    "  var a: Array<int> := [1, 2, 2, 3, 3, 3]; ";
constexpr const char* kPost = " end Main; end M;";

void expectRejected(const std::string& body, const std::string& name) {
    JitResult r = runAot(std::string(kPre) + body + kPost);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ("no member `" + name + "` on `IGrouping<TKey, T>`").c_str()));
}

void expectValue(const std::string& body, int value) {
    JitResult r = runAot(std::string(kPre) + body + kPost);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, value);
}

const char* kG = "a.GroupBy(lambda(x: int) => x)";

std::string lambdaBody(const std::string& expr) {
    return std::string("Console.WriteLine(") + kG +
           ".Select(lambda(g: IGrouping<int, int>) => " + expr +
           ").Count()); return 0;";
}

std::string stmtBody(const std::string& expr) {
    return std::string("for var g in ") + kG + " do Console.WriteLine(" + expr +
           "); end; return 0;";
}

std::string nullCondBody(const std::string& expr) {
    return std::string("for var g in ") + kG +
           " do var h: IGrouping<int, int>? := g; Console.WriteLine(" + expr +
           "); end; return 0;";
}

}  // namespace

// ---- `.` arm, lambda parameter ----
BEZEL_TEST(igrouping_undeclared_member_reject_6740, lambda_foo_rejected_spec_5) {
    expectRejected(lambdaBody("g.Foo"), "Foo");
}
BEZEL_TEST(igrouping_undeclared_member_reject_6740, lambda_keys_rejected_spec_5) {
    expectRejected(lambdaBody("g.Keys"), "Keys");
}
BEZEL_TEST(igrouping_undeclared_member_reject_6740, lambda_size_rejected_spec_5) {
    expectRejected(lambdaBody("g.Size"), "Size");
}
BEZEL_TEST(igrouping_undeclared_member_reject_6740,
           lambda_capacity_rejected_spec_5) {
    expectRejected(lambdaBody("g.Capacity"), "Capacity");
}
BEZEL_TEST(igrouping_undeclared_member_reject_6740,
           lambda_count2_rejected_spec_5) {
    expectRejected(lambdaBody("g.Count2"), "Count2");
}
// Where position (a bool-typed use) was silently accepted before.
BEZEL_TEST(igrouping_undeclared_member_reject_6740,
           where_lambda_foo_rejected_spec_5) {
    expectRejected(std::string("return ") + kG +
                       ".Where(lambda(g: IGrouping<int, int>) => g.Foo).Count();",
                   "Foo");
}

// ---- `.` arm, statement position (for var) ----
BEZEL_TEST(igrouping_undeclared_member_reject_6740, stmt_foo_rejected_spec_5) {
    expectRejected(stmtBody("g.Foo"), "Foo");
}
BEZEL_TEST(igrouping_undeclared_member_reject_6740, stmt_keys_rejected_spec_5) {
    expectRejected(stmtBody("g.Keys"), "Keys");
}
BEZEL_TEST(igrouping_undeclared_member_reject_6740, stmt_size_rejected_spec_5) {
    expectRejected(stmtBody("g.Size"), "Size");
}
BEZEL_TEST(igrouping_undeclared_member_reject_6740,
           stmt_capacity_rejected_spec_5) {
    expectRejected(stmtBody("g.Capacity"), "Capacity");
}
BEZEL_TEST(igrouping_undeclared_member_reject_6740,
           stmt_count2_rejected_spec_5) {
    expectRejected(stmtBody("g.Count2"), "Count2");
}

// ---- `?.` arm ----
BEZEL_TEST(igrouping_undeclared_member_reject_6740,
           null_conditional_foo_rejected_spec_5) {
    expectRejected(nullCondBody("h?.Foo"), "Foo");
}
BEZEL_TEST(igrouping_undeclared_member_reject_6740,
           null_conditional_keys_rejected_spec_5) {
    expectRejected(nullCondBody("h?.Keys"), "Keys");
}
BEZEL_TEST(igrouping_undeclared_member_reject_6740,
           null_conditional_size_rejected_spec_5) {
    expectRejected(nullCondBody("h?.Size"), "Size");
}
BEZEL_TEST(igrouping_undeclared_member_reject_6740,
           null_conditional_capacity_rejected_spec_5) {
    expectRejected(nullCondBody("h?.Capacity"), "Capacity");
}
BEZEL_TEST(igrouping_undeclared_member_reject_6740,
           null_conditional_count2_rejected_spec_5) {
    expectRejected(nullCondBody("h?.Count2"), "Count2");
}

// ---- Controls: the declared surface keeps compiling with right values ----

// Key sum 1+2+3 = 6; Count() sum 1+2+3 = 6.
BEZEL_TEST(igrouping_undeclared_member_reject_6740, key_and_count_op_accepted_spec_5) {
    expectValue(std::string("return ") + kG +
                    ".Select(lambda(g: IGrouping<int, int>) => g.Key + g.Count()).Sum();",
                12);
}
// Where / Any / Contains / ToArray / OrderBy / First / Sum on the group.
BEZEL_TEST(igrouping_undeclared_member_reject_6740, operators_accepted_spec_5) {
    expectValue(
        std::string("var s: int := 0; for var g in ") + kG +
            " do s := s + g.Where(lambda(v: int) => v > 1).Count() + "
            "g.OrderBy(lambda(v: int) => v).First() + g.Sum(); "
            "s := s + g.ToArray().Count + "
            "g.Select(lambda(v: int) => v).Count(); "
            "if g.Any(lambda(v: int) => v = 3) then s := s + 1; end; "
            "if g.Contains(2) then s := s + 1; end; "
            "end; return s;",
        // groups {1},{2,2},{3,3,3}:
        // Where>1: 0+2+3=5; First: 1+2+3=6; Sum: 1+4+9=14; ToArray.Count:
        // 1+2+3=6; Select.Count: 6; Any(=3): 1; Contains(2): 1  => 39
        39);
}
// for var v in g, and the `?.` operator call spelling.
BEZEL_TEST(igrouping_undeclared_member_reject_6740, iterate_and_nullcond_ops_accepted_spec_5) {
    expectValue(
        std::string("var s: int := 0; for var g in ") + kG +
            " do var h: IGrouping<int, int>? := g; "
            "for var v in g do s := s + v; end; "
            "s := s + (h?.Count() ?? 0) + (h?.Key ?? 0); end; return s;",
        // 14 + 6 + 6
        26);
}
