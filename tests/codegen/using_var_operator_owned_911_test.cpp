// Codegen — issue #911 (discovery sweep #7): `using var c := a + b`, where the
// user-defined `operator +` returns `owned T`, leaked the owned operator result.
// usingVarOwnsClassObjectDynamically only inspected a CallExpr initializer, so a
// BinaryExpr (operator-overload) initializer fell through and no scope-end
// disposal was registered. Sema sets the BinaryExpr's exprSymbol to the operator
// MethodSymbol; its OperatorDecl carries returnOwned, so the fix registers
// disposal exactly when the operator returns owned (a borrow-returning operator
// is left undisposed). §25 / §28.10.

#include "support/codegen_support.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
const char* kCell =
    "class Cell public static n: int := 0; public V: int; "
    "  public constructor(v: int) Cell.n:=Cell.n+1; V:=v; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; "
    "  public static method operator +(a: Cell, b: Cell): owned Cell => "
    "    new Cell(a.V + b.V); "
    "end Cell; ";
}  // namespace

// The verified repro: `using var result := a + b` disposes the owned result.
BEZEL_TEST(using_var_operator_owned_911, owned_operator_result_disposed) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    using var a := new Cell(10); "
        "    using var b := new Cell(20); "
        "    using var result := a + b; "
        "  end; "
        "  return Cell.n; "  // 0
        "end Main; end C;"), 0);
}

// The operator result is also usable (V == 30) before disposal.
BEZEL_TEST(using_var_operator_owned_911, owned_operator_result_value) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  using var a := new Cell(10); "
        "  using var b := new Cell(20); "
        "  using var result := a + b; "
        "  return result.V; "  // 30
        "end Main; end C;"), 30);
}
