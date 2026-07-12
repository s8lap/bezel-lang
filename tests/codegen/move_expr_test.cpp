// Codegen + typing for the `move` expression (§28.3). At runtime `move x`
// yields the operand value unchanged (ownership transfer is compile-time);
// it also keeps the operand's type, so member access on a moved value
// resolves. Observable via the int exit code. (Regression: `move` had no
// codegen case — it lowered to null and segfaulted as an argument / RHS —
// and no recorded type — member access on a moved value returned 0.)

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int runProg(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
const char* kR = "module M; class R public V: int := 0; end R; ";
} // namespace

BEZEL_TEST(codegen_move, move_local_preserves_value_and_type) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class R public V: int := 9; end R; "
        "class C public static method Main(): int "
        "  var r := new R(); var s := move r; return s.V; "
        "end Main; end C;"), 9);
}

BEZEL_TEST(codegen_move, move_as_constructor_argument) {
    BEZEL_EXPECT_EQ(runProg(std::string(kR) +
        "class Box public item: R; "
        "  public constructor(r: R) item := r; end constructor; "
        "  public method Val(): int => item.V; end Box; "
        "class C public static method Main(): int "
        "  var r := new R(); r.V := 5; var b := new Box(move r); "
        "  return b.Val(); "
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_move, move_as_field_assignment_rhs) {
    BEZEL_EXPECT_EQ(runProg(std::string(kR) +
        "class Box public item: R; "
        "  public constructor(r: R) item := move r; end constructor; "
        "  public method Val(): int => item.V; end Box; "
        "class C public static method Main(): int "
        "  var r := new R(); r.V := 6; var b := new Box(r); "
        "  return b.Val(); "
        "end Main; end C;"), 6);
}

BEZEL_TEST(codegen_move, move_as_method_argument) {
    BEZEL_EXPECT_EQ(runProg(std::string(kR) +
        "class C public static method Take(r: R): int => r.V; "
        "  public static method Main(): int "
        "    var r := new R(); r.V := 8; return Take(move r); "
        "  end Main; end C;"), 8);
}
