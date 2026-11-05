// Pin test for issue #7575 -- `l.Select(lambda(x: int) => x + 1).Sum()` on a
// receiver typed as the borrowing list interface (stdlib §4.1, v0.355). The lazy
// `Select` and the eager `Sum` are the Linq operators of stdlib §5
// (System.Collections.Linq, v0.355), declared over the sequence interface that
// the list interface extends, so [1,2,3] -> [2,3,4] -> 9.
//
// ALREADY FIXED on staging b52409a7 (verified by lane r39-verify-vA); this
// file only PINS the issue's literal repro (typed lambda parameter). The
// untyped-lambda spelling is pinned by
// ilist_receiver_deferred_operators_7625_test.cpp.
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

BEZEL_TEST(ilist_select_typed_lambda_sum_7575,
           select_typed_lambda_sum_on_ilist_stdlib_5_v0_355) {
    auto r = runAotChecked(
        "module M;\n"
        "import System.Linq;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a: Array<int> := [1,2,3];\n"
        "    var l: IList<int> := a;\n"
        "    Console.WriteLine(l.Select(lambda(x: int) => x + 1).Sum());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9\n"));
}
