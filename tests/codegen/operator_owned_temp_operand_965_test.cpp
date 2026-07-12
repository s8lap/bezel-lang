// Codegen — issue #965 (#912 follow-on): an OWNED-TEMPORARY operand of a class
// operator call leaked. #912 disposed a chained owned-OPERATOR result operand
// (`(a+b)` in `a+b+c`); the same `lowerBinary` site must also dispose the other
// owned-temporary operand shapes: a bare `new C(...)` operand, and an
// `owned`-returning method/function call result (`Make(1)`). Each is passed to
// the operator as a borrow and, once the call returns, has no owner.
//
// THE FIX generalizes the #912 detection to `isOwnedTempOperand`: a NewExpr (of
// a class), an owned-returning operator BinaryExpr, or an owned-returning
// CallExpr — disposed after the operator call. A named-local / member-access
// borrow operand is none of these shapes (no double-free), and the existing
// `paramConsumes` guard still skips an operand the operator takes by `owned`.
// §28.3 / §25.1.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
// Cell counts live instances (ctor +1, dtor -1); a final count of 0 means every
// allocation was disposed exactly once. Make(): owned Cell is a factory whose
// result is an owned temporary when used directly as an operand.
const char* kCell =
    "class Cell public static n: int := 0; public V: int; "
    "  public constructor(v: int) Cell.n:=Cell.n+1; V:=v; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; "
    "  public static method operator +(a: Cell, b: Cell): owned Cell => "
    "    new Cell(a.V + b.V); "
    "  public static method Make(v: int): owned Cell => new Cell(v); "
    "end Cell; ";
}  // namespace

// Both operands are anonymous `new` temporaries — both disposed (was n=2).
BEZEL_TEST(operator_owned_temp_operand_965, both_new_operands_no_leak) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do using var r := new Cell(1) + new Cell(2); end; "
        "  return Cell.n; "  // 0
        "end Main; end C;"), 0);
}

// One `new` operand, one named-local borrow — only the `new` temp is disposed
// (the borrow `b` is owned by its own `using var`; no double-free). Was n=1.
BEZEL_TEST(operator_owned_temp_operand_965, new_operand_and_borrow_no_leak) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    using var b := new Cell(2); "
        "    using var r := new Cell(1) + b; "
        "  end; "
        "  return Cell.n; "  // 0
        "end Main; end C;"), 0);
}

// An `owned`-returning factory call as an operand is an owned temporary. Was n=1.
BEZEL_TEST(operator_owned_temp_operand_965, owned_call_operand_no_leak) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    using var b := new Cell(2); "
        "    using var r := Cell.Make(1) + b; "
        "  end; "
        "  return Cell.n; "  // 0
        "end Main; end C;"), 0);
}

// Control: two named-local BORROW operands are never disposed by this path —
// they remain owned by their `using var`s, disposed exactly once each (n=0, no
// over-disposal / double-free).
BEZEL_TEST(operator_owned_temp_operand_965, borrow_operands_not_over_disposed) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    using var a := new Cell(1); "
        "    using var b := new Cell(2); "
        "    using var r := a + b; "
        "  end; "
        "  return Cell.n; "  // 0
        "end Main; end C;"), 0);
}

// The result is still correct after the operand temporaries are disposed.
BEZEL_TEST(operator_owned_temp_operand_965, new_operands_result_value) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  using var r := new Cell(4) + new Cell(5); "
        "  return r.V; "  // 9
        "end Main; end C;"), 9);
}
