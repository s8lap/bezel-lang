// Codegen — issue #912 (discovery sweep #7): a CHAINED operator expression
// (`a + b + c` = `(a + b) + c`) leaked the INTERMEDIATE owned operator result.
//
// `lowerBinary` evaluates the left operand, which for a chained operator is
// itself an owned-returning `operator +` call producing a fresh owned class
// temporary. That temporary is passed as a BORROW argument to the outer
// operator and, once the call returns, has no owner — but codegen never
// disposed it. (Bug A from the issue — `using var` bound to the FINAL operator
// result — was already fixed by #911; this is Bug B, the intermediate.)
//
// THE FIX: after the operator call returns, dispose an operand that is itself an
// owned-returning operator result (mirrors the #911 owned-operator detection:
// OperatorDecl::returnOwned + a class/interface result). A named-local / borrow
// operand is never such a BinaryExpr, so it is left untouched (no double-free);
// and if THIS operator's parameter is `owned` (it consumes the operand), the
// temporary is not disposed here either. §28.3 / §25.1.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
// A class counting live instances: every constructor increments, every
// destructor decrements, so the final count is 0 iff every allocation was
// disposed exactly once (a leak → >0, a double-free → <0 or a crash).
const char* kCell =
    "class Cell public static n: int := 0; public V: int; "
    "  public constructor(v: int) Cell.n:=Cell.n+1; V:=v; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; "
    "  public static method operator +(a: Cell, b: Cell): owned Cell => "
    "    new Cell(a.V + b.V); "
    "end Cell; ";
}  // namespace

// The verified repro: `a + b + c` disposes the intermediate `(a+b)` and the
// final result, so Cell.n returns to 0 (was 1 — the intermediate leaked).
BEZEL_TEST(operator_chain_temp_dispose_912, three_term_chain_no_leak) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    using var a := new Cell(1); "
        "    using var b := new Cell(2); "
        "    using var c := new Cell(3); "
        "    using var result := a + b + c; "
        "  end; "
        "  return Cell.n; "  // 0
        "end Main; end C;"), 0);
}

// A deeper chain `a + b + c + d` disposes BOTH intermediates `(a+b)` and
// `((a+b)+c)` — recursion through lowerBinary handles arbitrary depth.
BEZEL_TEST(operator_chain_temp_dispose_912, four_term_chain_no_leak) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    using var a := new Cell(1); "
        "    using var b := new Cell(2); "
        "    using var c := new Cell(3); "
        "    using var d := new Cell(4); "
        "    using var result := a + b + c + d; "
        "  end; "
        "  return Cell.n; "  // 0
        "end Main; end C;"), 0);
}

// The chained result is still correct (the intermediate disposal does not
// corrupt or free the final value): 1 + 2 + 3 == 6.
BEZEL_TEST(operator_chain_temp_dispose_912, chain_result_value_preserved) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  using var a := new Cell(1); "
        "  using var b := new Cell(2); "
        "  using var c := new Cell(3); "
        "  using var result := a + b + c; "
        "  return result.V; "  // 6
        "end Main; end C;"), 6);
}

// Regression guard for #911 (Bug A): a single, NON-chained operator result
// bound to a `using var` is disposed exactly once — the operand `a`/`b` are
// named-local borrows, never disposed by the new intermediate-disposal path.
BEZEL_TEST(operator_chain_temp_dispose_912, single_operator_still_balanced) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    using var a := new Cell(1); "
        "    using var b := new Cell(2); "
        "    using var result := a + b; "
        "  end; "
        "  return Cell.n; "  // 0
        "end Main; end C;"), 0);
}
