// fix #8994 / §28.3.10 (spec v0.359): an owned local is disposed exactly once on
// every control-flow path. A lambda body's `return` operand is read-checked
// (fix #8902) and its moves recorded, but the `return` ends its path, so a move
// recorded there must not reach the fall-through path: the local's scope-end
// disposal is skipped only on the path that actually transferred it.
//
// Regression from #8902: the ReturnStmt arm of
// MethodOwnership::applyUsingVarBorrowPinInLambdaStmt ran `recordMovesIn`, whose
// `markMovedByName` inserts the local into the persistent `everDischarged_` set,
// so the conditional-discharge gate dropped the scope-end disposal of `u` on the
// path where it was not moved. Corpus twin: s28_3_10_7408 (rule corpus).
//
// per §28.3.10, §28.3, v0.359, #8994

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

const char* kProgram = R"(
module Conf;
import System;
import System.Threading.Tasks;
public class Cell
  public static n: int := 0;
  public value: int := 0;
  public constructor(v: int) Cell.n := Cell.n + 1; this.value := v; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
public class Program
  public static async method Take(owned c: Cell): Task<int>
    return c.value;
  end Take;
  public static async method Run(j: int): Task<int>
    var f := async lambda(k: int): Task<int> => do var u := new Cell(1); if k mod 2 = 0 then return await Program.Take(move u); end; return u.value; end;
    return await f(j);
  end Run;
  public static async method Main(): Task<int>
    var total := 0;
    for var j in 0..<200 do total := total + await Program.Run(j); end;
    Console.WriteLine("total=" + total.ToString() + " n=" + Cell.n.ToString());
    return 0;
  end Main;
end Program;
)";

BEZEL_TEST(lambda_body_return_move_fallthrough_8994,
           async_lambda_conditional_move_on_return_disposes_on_fallthrough_spec_28_3_10_v0_359) {
    auto r = runAot(kProgram);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("total=200 n=0") != std::string::npos);
}

}  // namespace
