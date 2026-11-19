// fix #8768 / §28.3 *State match on reassignment*, bullet 1 (spec v0.359):
// the self-assignment rule in a GETTER accessor body — the two spellings the
// #7109 fix's narrow getter scan missed.
//
// The spec sentence names no scope: "A self-assignment is no exception: for an
// owned-state class-typed local `t`, `t := t;` stores the unmarked read `t`, a
// borrow, into an owned-state LHS and is rejected at the assignment by this
// rule, not a §28.7 discharge." §28.9 makes the two accessor body spellings
// "semantically indistinguishable at every read site", so the METHOD body is
// the oracle: the method twin of every program below is rejected (MEASURED on
// the base), and the getter twin must answer the same.
//
// (1) a LAMBDA declared INSIDE a getter body: the getter scan walked the
//     getter's own statements and never crossed into the lambda body, and the
//     lambda-body arm that carries the rule (`applyUsingVarBorrowPinInLambdaStmt`)
//     runs only from the METHOD dataflow, which a getter accessor never
//     reaches.
//
// (2) the getter body's NULLABLE local: `var t: Cell? := null;` leaves `t`
//     poly-null, and the fresh producer in `t := new Cell(1);` makes it
//     OWNED — a program-point fact, which the #7109 scan (which read the
//     DECLARATION initializer and required every store to the local to be a
//     self-assignment) could not see.
//
// ROW 3 is the join case: the same rule with the owned-producing store under a
// branch, which the state track must carry across the join.
//
// Every row is red on the base and green with the fix. The admitting twins
// (`t := move t`, a borrowed-state local, a primitive, a poly-null local that
// never becomes owned) are corpus candidates, not gtest rows.
//
// per §28.3, §28.9, §15.3, §28.7, v0.359, #8768

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {

constexpr const char* kSelfAssign = "cannot self-assign";

const char* kCell = R"(
module M;
class Cell
  public value: int := 0;
end Cell;
)";

// ROW 1 — the issue's (1): a lambda declared INSIDE a getter body that
// self-assigns its own owned local.
BEZEL_TEST(self_assignment_getter_nested_lambda_nullable_8768,
           getter_body_nested_lambda_self_assign_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kCell) + R"(
class H
  public property Item: Cell?
    get => do
      var f := lambda(): int => do var u := new Cell(); u := u; return 0; end;
      return null;
    end;
  end Item;
end H;
class Program
  public static method Main(): int
    var h := new H();
    if h.Item = null then return 0; end;
    return 1;
  end Main;
end Program;
)";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSelfAssign));
}

// ROW 2 — the issue's (2): the getter body's nullable local, made owned by a
// fresh producer before the self-assignment.
BEZEL_TEST(self_assignment_getter_nested_lambda_nullable_8768,
           getter_body_nullable_local_self_assign_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kCell) + R"(
class H
  public property Item: Cell?
    get => do
      var t: Cell? := null;
      t := new Cell(1);
      t := t;
      return null;
    end;
  end Item;
end H;
class Program
  public static method Main(): int
    var h := new H();
    if h.Item = null then return 0; end;
    return 1;
  end Main;
end Program;
)";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSelfAssign));
}

// ROW 3 — the join: the owned-producing store under a branch, then the
// self-assignment. The method twin is rejected; the getter twin must be too.
BEZEL_TEST(self_assignment_getter_nested_lambda_nullable_8768,
           getter_body_conditional_store_self_assign_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kCell) + R"(
class H
  public property Item: Cell?
    get => do
      var t: Cell? := null;
      if 1 < 2 then
        t := new Cell(1);
      end;
      t := t;
      return null;
    end;
  end Item;
end H;
class Program
  public static method Main(): int
    var h := new H();
    if h.Item = null then return 0; end;
    return 1;
  end Main;
end Program;
)";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSelfAssign));
}

}  // namespace
