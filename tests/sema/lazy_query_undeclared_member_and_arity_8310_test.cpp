// fix #8310 (stdlib §5 `System.Collections.Linq` / §4.12, §26.5 rule 1,
// v0.355): a call on a LAZY query value (the result of `Where` / `Select` /
// ...) that names a member stdlib §5 does not declare, or a §5 operator at an
// arity §5 does not declare, was accepted and computed nothing.
//
//   * `q.FirstOrDefault()` -- §5 declares `First` / `FirstOrNull`, no
//     `FirstOrDefault` -- printed `[0]` and leaked (LSan, rc=1);
//   * `q.Aggregate(fold)` -- §5 declares only `Aggregate<TAccum>(seed, fold)`
//     -- printed an empty line.
// Both stayed untyped, so codegen degraded them behind an `internal:` warning
// (#2434 / #3079). The direct-collection receivers (`Array<T>`) and a named
// `IEnumerable<T>` already rejected them; the lazy sentinel did not.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {

std::string prog(const std::string& tail) {
    return "module M; class C public static method Main(): int "
           "using var m := new Array<string>(); "
           "m.Add(\"a\" + \"!\"); m.Add(\"b\" + \"!\"); "
           "var q := m.Where(lambda(s) => s.Length > 0); " +
           tail + " return 0; end Main; end C;";
}

void expectRejected(const std::string& tail, const char* needle) {
    auto r = runAotChecked(prog(tail));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

void expectRuns(const std::string& tail, const char* out) {
    auto r = runAotChecked(prog(tail));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

}  // namespace

BEZEL_TEST(sema_lazy_query_undeclared_member_8310, first_or_default_rejected) {
    expectRejected("var x := q.FirstOrDefault(); Console.WriteLine($\"[{x}]\");",
                   "no member `FirstOrDefault`");
}

BEZEL_TEST(sema_lazy_query_undeclared_member_8310, last_or_default_rejected) {
    expectRejected("var x := q.LastOrDefault(); Console.WriteLine($\"[{x}]\");",
                   "no member `LastOrDefault`");
}

BEZEL_TEST(sema_lazy_query_undeclared_member_8310,
           single_or_default_rejected) {
    expectRejected("var x := q.SingleOrDefault(); Console.WriteLine($\"[{x}]\");",
                   "no member `SingleOrDefault`");
}

BEZEL_TEST(sema_lazy_query_undeclared_member_8310,
           element_at_or_default_rejected) {
    expectRejected(
        "var x := q.ElementAtOrDefault(0); Console.WriteLine($\"[{x}]\");",
        "no member `ElementAtOrDefault`");
}

BEZEL_TEST(sema_lazy_query_undeclared_member_8310, seedless_aggregate_rejected) {
    expectRejected("Console.WriteLine(q.Aggregate(lambda(x, y) => x + y));",
                   "wrong number of arguments for stdlib §5 LINQ operator "
                   "`Aggregate`");
}

BEZEL_TEST(sema_lazy_query_undeclared_member_8310, count_two_args_rejected) {
    expectRejected("var n := q.Count(1, 2); Console.WriteLine($\"{n}\");",
                   "wrong number of arguments for stdlib §5 LINQ operator "
                   "`Count`");
}

// Controls: the declared spellings keep compiling and print the right value.
BEZEL_TEST(sema_lazy_query_undeclared_member_8310, first_or_null_control) {
    expectRuns("var x := q.FirstOrNull(); Console.WriteLine($\"[{x}]\");",
               "[a!]\n");
}

BEZEL_TEST(sema_lazy_query_undeclared_member_8310, seeded_aggregate_control) {
    expectRuns("Console.WriteLine(q.Aggregate(\"\", "
               "lambda(x, y) => x + y));",
               "a!b!\n");
}

BEZEL_TEST(sema_lazy_query_undeclared_member_8310, count_control) {
    expectRuns("Console.WriteLine($\"{q.Count()}\");", "2\n");
}

BEZEL_TEST(sema_lazy_query_undeclared_member_8310, first_control) {
    expectRuns("Console.WriteLine(q.First());", "a!\n");
}
