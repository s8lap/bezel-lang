// Codegen — issue #1029 (§28.3): `var x := <owned-nullable> ?? new Cell(7)`
// leaked the RHS-allocated Cell when the left operand was null. The ownership
// pass did not recognise the built-in `??` operator as an owned-producing
// expression, so x.autoDropAtScopeEnd was never set and no scope-end disposal
// was registered. Fix:
//   1. producerState (Ownership.cpp): `??` with an Owned RHS returns Owned.
//   2. usingVarOwnsClassObjectDynamically (LlvmBackend_Setup.cpp): the same gate
//      registers a dynamicDispose for the binding.
//   3. unwindOwnedLocalClass (LlvmBackend_Setup.cpp): mirrors (1)/(2) for the
//      exception-unwind path.
//   4. lowerCoalesce (LlvmBackend_Exprs.cpp): when the RHS is a fresh owned-class
//      producer, null the LHS slot before the conditional branch so the LHS
//      variable's null-guarded scope-end disposal is a no-op. // §28.3
//
// fix #4964 (§28.3 producer table `a ?? b` row, K7.3, K3.13, v0.307): the
// original rows spelled the left operand as a NAMED OWNED LOCAL
// (`var maybe: Cell? := …; var x := maybe ?? new Cell(7);`). §28.3 makes that a
// mixed meet — "A named owned binding written as an arm is an unmarked read and
// therefore a borrow arm … mixed = rejected — equalize the arms first" — so the
// program is now rejected (the `named_owned_left_…_rejected` rows below pin
// that). The #1029 disposal oracle is kept on the spec-legal owned⊓owned
// spelling: the left operand is an `owned Cell?`-returning call (an owned
// producer), which exercises the same null-left / non-null-left codegen paths
// with the same Cell.n dispose counter. Ported from a hand-rolled JIT runner to
// runAot (the strict AOT oracle) while touching it.

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Cell.n is incremented in the destructor (= disposal counter). Make(f) is an
// owned producer of a nullable Cell: Cell(1) when f, null otherwise.
const char* kCell =
    "class Cell "
    "  public static n: int := 0; "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  destructor Cell.n := Cell.n + 1; end destructor; "
    "  public static method Make(f: bool): owned Cell? "
    "    if f then return new Cell(1); end; "
    "    return null; "
    "  end Make; "
    "end Cell; ";

// Runs the program and returns its exit code, or -999 if it did not run.
int run(const std::string& src) {
    JitResult r = runAot(src);
    if (!r.ran) return -999;
    return r.exitCode;
}

bool rejectedWithMixedMeet(const std::string& src) {
    JitResult r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("the operands of this `??` mix an owned value "
                           "with a borrowed arm") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §28.3 / #1029 example A: null LHS → x owns the new RHS cell → disposed once.
// Before the #1029 fix: Cell.n=0 (leak); after: Cell.n=1.
BEZEL_TEST(null_coalesce_owned_disposal_1029,
           null_lhs_rhs_disposed_spec_28_3) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    var x := Cell.Make(false) ?? new Cell(7); "
        "  end; "
        "  return Cell.n; "  // 1: Cell(7) disposed at scope end of `do`
        "end Main; end C;"), 1);
}

// §28.3 / #1029 example B: non-null LHS → x owns the LHS cell → disposed exactly
// once. RHS is never created.
BEZEL_TEST(null_coalesce_owned_disposal_1029,
           nonnull_lhs_disposed_exactly_once_spec_28_3) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    var x := Cell.Make(true) ?? new Cell(7); "
        "  end; "
        "  return Cell.n; "  // 1: Cell(1) disposed exactly once (via x)
        "end Main; end C;"), 1);
}

// §28.3 / #1029 non-owned RHS: `maybe ?? borrowedLocal` — both arms are borrows
// (K7.2 borrowed⊓borrowed), so x carries no disposal obligation and `maybe`
// keeps its own. Both allocated cells are disposed exactly once (one via the
// using var, one via maybe's auto-drop).
BEZEL_TEST(null_coalesce_owned_disposal_1029,
           borrow_rhs_no_spurious_dispose_spec_28_3) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    using var defaultCell := new Cell(99); "
        "    var maybe: Cell? := new Cell(1); "
        "    var x := maybe ?? defaultCell; "
        "  end; "
        "  return Cell.n; "  // 2
        "end Main; end C;"), 2);
}

// §28.3 / #1029: value read through the ?? result is correct (7 on null path).
BEZEL_TEST(null_coalesce_owned_disposal_1029,
           null_lhs_rhs_value_correct_spec_28_3) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    var x := Cell.Make(false) ?? new Cell(7); "
        "    return x.V; "  // 7: RHS was used
        "  end; "
        "end Main; end C;"), 7);
}

// §28.3 / #1029: value read through the ?? result is correct (1 on non-null path).
BEZEL_TEST(null_coalesce_owned_disposal_1029,
           nonnull_lhs_value_correct_spec_28_3) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    var x := Cell.Make(true) ?? new Cell(7); "
        "    return x.V; "  // 1: LHS (Cell.V=1) was used, not RHS
        "  end; "
        "end Main; end C;"), 1);
}

// fix #4964: the original #1029 spelling, a NAMED OWNED LOCAL on the left and a
// fresh producer on the right, is a K7.3 mixed meet (§28.3 `a ?? b` row).
BEZEL_TEST(null_coalesce_owned_disposal_1029,
           named_owned_left_null_fresh_right_rejected_4964_28_3) {
    BEZEL_EXPECT_TRUE(rejectedWithMixedMeet(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    var maybe: Cell? := null; "
        "    var x := maybe ?? new Cell(7); "
        "  end; "
        "  return Cell.n; "
        "end Main; end C;"));
}

BEZEL_TEST(null_coalesce_owned_disposal_1029,
           named_owned_left_nonnull_fresh_right_rejected_4964_28_3) {
    BEZEL_EXPECT_TRUE(rejectedWithMixedMeet(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    var maybe: Cell? := new Cell(1); "
        "    var x := maybe ?? new Cell(7); "
        "    return x.V; "
        "  end; "
        "end Main; end C;"));
}
