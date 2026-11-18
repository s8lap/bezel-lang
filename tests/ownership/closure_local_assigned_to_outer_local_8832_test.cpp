// narrow #8832 / §28.3 closure bullet (the local-scope bullet + provenance
// clauses 1 and 6), §28.9 (spec v0.359): a closure local `f` capturing an owned
// local of an INNER block, assigned to a function-typed local `g` of an
// enclosing block (`g := f;`).
//
// §28.3's closure bullet: a function value "may be bound to a local ... whose
// scope does not strictly enclose any source's scope (the local-scope bullet
// above, loop back edges included; an initializer is admitted by construction,
// each later assignment checked on its own)". Clause 1: "a captured owned local
// [contributes] its enclosing scope"; clause 6: "A local or parameter
// contributes, at each read, the union of the sets of the forms the flow
// analysis traces it to". So `f` read at `g := f;` carries the inner block's
// scope and the assignment is rejected exactly as `g := lambda() => a.Count;`
// is ("closure capturing `a` is assigned to `g`, which outlives it").
//
// MEASURED on r30-c1 (0d5d80a58): rows 1-3 compiled and `g()` read the capture
// after the inner block disposed it (heap-use-after-free in `lam$1`, the free
// in `bz$M.P$One` at the inner block's end). borrowSourceForExpr's bare-
// identifier shape gates on returnedExprMayBorrow, which answers false for a
// function type, so the holder's tracked closure source never reached the
// assignment.
//
// NARROW: the issue's `int`-capture spelling (`var x := 3; var f := lambda() =>
// x; g := f;`) is NOT a rejection. Clause 1: "a captured value that carries no
// borrow — a value type that holds none ... contributes nothing, and lives in
// its heap capture cell", so its set is empty and §27.5 requires it to run. Its
// use-after-free is a codegen capture-cell lifetime defect left open on #8832.
//
// Rows 1-3 are red on the base and assert the diagnostic at the assignment.
// Row 4 is the one boundary control: `g := f` where `g` is in the SAME block as
// the capture must still compile and run ASan-clean 200x. It is red under the
// plausible wrong fix that rejects every alias of a capture-bearing function
// local without comparing the target's scope against the source's.
//
// per §28.3, §28.9, v0.359, #8832

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kCell = R"(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int;
  public constructor(x: int) this.v := x; end constructor;
end Cell;
)";

const char* kMain = R"(
  public static method Main(): int
    var s := 0;
    for var i in 1..200 do s := s + One(); end;
    Console.WriteLine(s); return 0; end Main; end P;
)";

// ROW 1 — the issue's own owned-capture repro (`p29`): an owned `Array<int>`
// literal local captured by `f`, then `g := f;` (line 9, column 7).
BEZEL_TEST(closure_local_assigned_to_outer_local_8832,
           alias_of_closure_over_inner_array_assigned_outward_rejected_spec_28_3_v0_359) {
    const std::string src = R"(
module M; import System; import System.Linq; import System.Collections.Generic;
class P
  static method One(): int
    var g: () -> int := lambda() => 0;
    do
      var a: Array<int> := [1,2,3];
      var f := lambda() => a.Count;
      g := f;
    end;
    return g();
  end One;
  public static method Main(): int
    var s := 0;
    for var i in 1..200 do s := s + One(); end;
    Console.WriteLine(s); return 0; end Main; end P;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":9:7: closure capturing `a` is assigned to `g`, which outlives it"));
}

// ROW 2 — the class-instance twin: an owned `Cell` local captured by `f`
// (the assignment `g := f;` is at line 14, column 7).
BEZEL_TEST(closure_local_assigned_to_outer_local_8832,
           alias_of_closure_over_inner_owned_class_local_assigned_outward_rejected_spec_28_3_v0_359) {
    const std::string src = std::string(kCell) + R"(
class P
  static method One(): int
    var g: () -> int := lambda() => 0;
    do
      var c := new Cell(3);
      var f := lambda() => c.v;
      g := f;
    end;
    return g();
  end One;)" + kMain;
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":14:7: closure capturing `c` is assigned to `g`, which outlives it"));
}

// ROW 3 — the loop-carried spelling the local-scope bullet names ("loop back
// edges included"): `g` is declared outside the loop, `c` is per-iteration.
BEZEL_TEST(closure_local_assigned_to_outer_local_8832,
           alias_of_closure_over_per_iteration_local_assigned_to_loop_carried_local_rejected_spec_28_3_v0_359) {
    const std::string src = std::string(kCell) + R"(
class P
  static method One(): int
    var g: () -> int := lambda() => 0;
    var k := 0;
    for var i in 0..<2 do
      var c := new Cell(3);
      var f := lambda() => c.v;
      k := k + f();
      g := f;
    end;
    return g();
  end One;)" + kMain;
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":16:7: closure capturing `c` is assigned to `g`, which outlives it"));
}

// ROW 4 (boundary control) — `g` is declared in the SAME block as the capture,
// so its scope does not strictly enclose the source's: admitted, and it must
// run ASan-clean 200x (prints 600).
BEZEL_TEST(closure_local_assigned_to_outer_local_8832,
           alias_of_closure_assigned_to_same_block_local_runs_clean_spec_28_3_v0_359) {
    const std::string src = std::string(kCell) + R"(
class P
  static method One(): int
    var c := new Cell(3);
    var g: () -> int := lambda() => 0;
    var f := lambda() => c.v;
    g := f;
    return g();
  end One;)" + kMain;
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "600\n");
}

}  // namespace
