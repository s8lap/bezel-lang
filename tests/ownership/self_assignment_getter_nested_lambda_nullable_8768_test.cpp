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
// ROW 4 and ROW 5 are the FIX-FORWARD rows (the review lane r42-rev-m86 found
// the first attempt at this fix OVER-rejecting): a store that happens on EVERY
// path REPLACES the local's §28.3 state, and the state the rule asks about is
// the state at the PROGRAM POINT. ROW 4 is the branch join (`if`/`else`, both
// arms store null — the local is poly-null after the join, so the
// self-assignment is not the rule's case); ROW 5 is the same fact in a
// `do … end` BLOCK, whose statements RUN, so the store there is
// unconditional. Both programs are RED on the rejected commit cbbe641f4 and
// GREEN with this fix; both are ADMITTED by the METHOD twin of the same
// shape, which is the oracle the commit names.
//
// ROWS 1-3 are red on the base (r30-c1) and green with the fix. ROWS 4-5 are
// the suite's two boundary controls: they are green on r30-c1 (the base
// admits them, as it must) and red on the rejected commit — the wrong fix each
// catches is named in the commit body. The admitting twins
// (`t := move t`, a borrowed-state local, a primitive, a poly-null local that
// never becomes owned, the `try`/`catch` and the nested-lambda spellings of
// ROW 4) are corpus candidates, not gtest rows.
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

// ROW 4 — the FIX-FORWARD's branch-join control: an `if`/`else` whose BOTH
// arms store null. Every path reaches the assignment with the local in the
// POLY-NULL state, so the §28.3 state match does not apply to it and the
// program is admitted — exactly as the METHOD twin of the same shape is
// (MEASURED). The rejected commit cbbe641f4 REJECTED it ("cannot
// self-assign"), because its bitmask widened the state at a nested store and
// kept the stale initial Owned bit. This row is the suite's boundary control
// against that wrong fix.
BEZEL_TEST(self_assignment_getter_nested_lambda_nullable_8768,
           getter_body_branch_join_null_store_accepted_spec_28_3_v0_359) {
    std::string src = std::string(kCell) + R"(
class H
  public property Item: Cell?
    get => do
      var t: Cell? := new Cell();
      if 1 < 2 then
        t := null;
      else
        t := null;
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
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ROW 5 — the FIX-FORWARD's block control: the same fact in a `do … end`
// BLOCK, whose statements RUN in order, so the store there is unconditional
// and REPLACES the local's state (the local is poly-null at the assignment).
// The METHOD twin of the same shape is admitted (MEASURED); the rejected
// commit cbbe641f4 rejected it. This row is the suite's boundary control
// against a wrong fix that treats a `do … end` block as a scope-introducer
// and widens the state at its stores.
BEZEL_TEST(self_assignment_getter_nested_lambda_nullable_8768,
           getter_body_do_block_null_store_accepted_spec_28_3_v0_359) {
    std::string src = std::string(kCell) + R"(
class H
  public property Item: Cell?
    get => do
      var t: Cell? := new Cell();
      do
        t := null;
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
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

}  // namespace
