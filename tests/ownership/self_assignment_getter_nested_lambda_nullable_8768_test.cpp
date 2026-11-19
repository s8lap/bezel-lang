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
// ROWS 6-9 are the SECOND fix-forward's rows (the review lane r42-rev-m98
// REJECTED cbbe641f4+721f6c54c: the getter walk still folded the state of a
// branch that ENDS in `return`/`throw` into the join, so four programs the
// METHOD twin admits were over-rejected). A branch that ends in `return` or
// `throw` leaves the CALLABLE, so it never reaches the statement after the
// construct and its end state is not one of that construct's EXITS — it must
// contribute NO state to the join. The METHOD body already excludes exactly
// such a branch (`armNeverFallsThrough`), and this walk now REUSES that rule
// instead of re-deriving a weaker one. ROW 6 is the `if`/`else` whose `then`
// returns; ROW 7 the `elif` chain whose first arm returns; ROW 8 the `throw`
// arm; ROW 9 the `try` whose body returns and whose `catch` arm stores null.
// Each is RED on 721f6c54c and GREEN with this fix, and each is ADMITTED by
// the METHOD twin of the same shape (the oracle).
//
// ROWS 10-13 are the THIRD fix-forward's rows (the review lane r42-rev-m111
// REJECTED cbbe641f4+721f6c54c+bd353efee: the getter walk's `case` arm
// ALWAYS folded the no-match fall-through edge into the join, so a getter
// whose `case` is EXHAUSTIVE and whose every arm stores null was REJECTED
// ("cannot self-assign") while the tip admitted it and the METHOD twin — the
// oracle this walk names — admits it). §19.6: "In statement position, an
// unmatched value falls through with no action" — with an exhaustive `case`
// there IS no unmatched value, so there is no no-match edge to fold in. The
// AST carries the compiler's own exhaustiveness verdict
// (`CaseStmt::noMatchEdgeStaticallyDead`), and the method body's CFG adds
// that edge only when the verdict says it is live. ROW 10 is an `int`
// scrutinee whose arms are a value and a wildcard; ROW 11 a `bool` scrutinee
// whose arms cover `true` and `false`; ROW 12 a wildcard-only `case`; ROW 13
// a `case` whose FIRST arm ends in `return` and whose second arm is the
// wildcard — the diverging arm contributes no state to the join, and the
// wildcard leaves no no-match edge. Each is RED on bd353efee and GREEN with
// this fix, and each is ADMITTED by the METHOD twin of the same shape.
//
// ROW 14 is the THIRD fix-forward's must-still-REJECT row: a `case` that is
// NOT exhaustive (an `int` scrutinee with no wildcard) DOES have the
// no-match fall-through edge, so the pre-case OWNED state is one of the
// `case`'s exits and the §28.3 state match still applies. The METHOD twin
// REJECTS it (MEASURED), so the getter twin must reject it too — this row is
// the control against a fix that drops the no-match edge unconditionally.
//
// ROWS 1-3 are red on the base (r30-c1) and green with the fix. ROWS 4-5 are
// the suite's two boundary controls: they are green on r30-c1 (the base
// admits them, as it must) and red on the rejected commit — the wrong fix each
// catches is named in the commit body. The admitting twins
// (`t := move t`, a borrowed-state local, a primitive, a poly-null local that
// never becomes owned, the `try`/`catch` and the nested-lambda spellings of
// ROW 4) are corpus candidates, not gtest rows.
//
// per §28.3, §28.9, §19.6, §15.3, §28.7, v0.359, #8768

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

// The rows below build a `Cell` WITH a constructor, so the local can be
// initialized with a fresh owned producer (`new Cell(1)`) — the shape the
// review lane's probes use.
const char* kCellCtor = R"(
module M;
class Cell
  public value: int := 0;
  public constructor(x: int) this.value := x; end;
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

// ROW 6 — the SECOND fix-forward's row 1: an `if`/`else` whose `then` arm
// ENDS in `return`. The `then` arm leaves the callable, so it never reaches
// the assignment after the `if` and its end state is not an exit of the `if`:
// the only path that reaches the self-assignment is the `else` arm, which
// stored null, so the local is POLY-NULL there and the §28.3 state match does
// not apply. The METHOD twin of the same shape is ADMITTED (MEASURED) and the
// getter twin must agree — the rejected 721f6c54c REJECTED it ("cannot
// self-assign"), because the returning arm's OWNED state survived the meet.
//
// The guard is a LOCAL (`c`), not a literal: the `then` arm must be REACHABLE
// at the compile-time level (so the walk sees the `return`) while being FALSE
// at run time (so the program runs to completion, the `else` arm discharges
// the owned value, and the checked oracle sees no leak).
BEZEL_TEST(self_assignment_getter_nested_lambda_nullable_8768,
           getter_body_early_return_branch_accepted_spec_28_3_v0_359) {
    std::string src = std::string(kCellCtor) + R"(
class H
  public property Item: Cell?
    get => do
      var t: Cell? := new Cell(1);
      var c := 0;
      if c = 1 then
        return null;
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

// ROW 7 — the SECOND fix-forward's row 2: the same fact in an `elif` chain
// whose FIRST arm returns. Every path that reaches the self-assignment stored
// null on the way, so the local is poly-null there. The METHOD twin is
// ADMITTED (MEASURED); the rejected 721f6c54c REJECTED it. The guard is a
// local for the reason ROW 6 gives.
BEZEL_TEST(self_assignment_getter_nested_lambda_nullable_8768,
           getter_body_elseif_return_chain_accepted_spec_28_3_v0_359) {
    std::string src = std::string(kCellCtor) + R"(
class H
  public property Item: Cell?
    get => do
      var t: Cell? := new Cell(1);
      var c := 0;
      if c = 1 then
        return null;
      else if 2 < 3 then
        t := null;
      else
        t := null;
      end;
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

// ROW 8 — the SECOND fix-forward's row 3: an `if`/`else` whose `then` arm
// ENDS in `throw`. A `throw` leaves the callable exactly as a `return` does,
// so its arm contributes NO state to the join. The METHOD twin is ADMITTED
// (MEASURED); the rejected 721f6c54c REJECTED it.
//
// The guard is a LOCAL (`c`), not a literal: the arm must be REACHABLE at the
// compile-time level (so the walk sees the `throw`) while being FALSE at
// run time (so the program runs to completion and exits 0 — the `else` arm
// is the one that runs, and it stores null).
BEZEL_TEST(self_assignment_getter_nested_lambda_nullable_8768,
           getter_body_throw_branch_accepted_spec_28_3_v0_359) {
    std::string src = std::string(kCellCtor) + R"(
class H
  public property Item: Cell?
    get => do
      var t: Cell? := new Cell(1);
      var c := 0;
      if c = 1 then
        throw new Exception("x");
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

// ROW 9 — the SECOND fix-forward's row 4: a `try` whose BODY ends in a
// DIVERGING statement and whose `catch` arm stores null. The body's
// normal-completion path leaves the callable, so it is not an exit of the
// `try`; the catch arm is the only exit, and it stored null, so the local is
// poly-null at the self-assignment. The METHOD twin is ADMITTED (MEASURED);
// the rejected 721f6c54c REJECTED it.
//
// The body THROWS at run time (so the catch arm runs, discharges the owned
// value, and the checked oracle sees no leak) — a `throw` and a `return` are
// the same divergence for this rule (`armNeverFallsThrough` recognizes both),
// and the review lane's compile-only probe for this shape is the corpus
// candidate that keeps the `return` spelling.
BEZEL_TEST(self_assignment_getter_nested_lambda_nullable_8768,
           getter_body_try_return_catch_accepted_spec_28_3_v0_359) {
    std::string src = std::string(kCellCtor) + R"(
class H
  public property Item: Cell?
    get => do
      var t: Cell? := new Cell(1);
      try
        throw new Exception("x");
      catch var e: Exception =>
        do
          t := null;
        end;
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

// ROW 10 — the THIRD fix-forward's row 1: an EXHAUSTIVE `case` over an `int`
// scrutinee — a value arm and the wildcard arm — whose every arm stores null.
// §19.6 proves the exhaustiveness ("Any scrutinee — irrefutable arm: some arm
// is irrefutable — an unguarded `_`"), so the no-match fall-through edge is
// statically DEAD: there is no unmatched value to fall through with no action.
// Every path that reaches the self-assignment stored null, so the local is
// POLY-NULL there and the §28.3 state match does not apply.
//
// The METHOD twin of the same shape is ADMITTED (MEASURED) and the rejected
// bd353efee REJECTED it ("cannot self-assign"), because its getter walk
// folded the pre-case OWNED state in as the no-match edge whatever the
// `case`'s exhaustiveness.
BEZEL_TEST(self_assignment_getter_nested_lambda_nullable_8768,
           getter_body_case_exhaustive_int_accepted_spec_28_3_v0_359) {
    std::string src = std::string(kCellCtor) + R"(
class H
  public property Item: Cell?
    get => do
      var t: Cell? := new Cell(1);
      var k := 1;
      case k of
        1 => t := null,
        _ => t := null,
      end case;
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

// ROW 11 — the THIRD fix-forward's row 2: the same fact with a `bool`
// scrutinee whose arms cover `true` and `false`. §19.6's "Bool scrutinee:
// both `true` and `false` are covered" proves the exhaustiveness, so the
// no-match edge is statically dead. The METHOD twin is ADMITTED (MEASURED);
// the rejected bd353efee REJECTED it.
BEZEL_TEST(self_assignment_getter_nested_lambda_nullable_8768,
           getter_body_case_exhaustive_bool_accepted_spec_28_3_v0_359) {
    std::string src = std::string(kCellCtor) + R"(
class H
  public property Item: Cell?
    get => do
      var t: Cell? := new Cell(1);
      var b := true;
      case b of
        true => t := null,
        false => t := null,
      end case;
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

// ROW 12 — the THIRD fix-forward's row 3: a WILDCARD-ONLY `case`. The
// wildcard is irrefutable, so the `case` matches every value of the
// scrutinee and the no-match fall-through edge is statically dead. The METHOD
// twin is ADMITTED (MEASURED); the rejected bd353efee REJECTED it.
BEZEL_TEST(self_assignment_getter_nested_lambda_nullable_8768,
           getter_body_case_wildcard_only_accepted_spec_28_3_v0_359) {
    std::string src = std::string(kCellCtor) + R"(
class H
  public property Item: Cell?
    get => do
      var t: Cell? := new Cell(1);
      var k := 1;
      case k of
        _ => t := null,
      end case;
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

// ROW 13 — the THIRD fix-forward's row 4: a `case` whose FIRST arm ends in
// `return` and whose second arm is the wildcard. The returning arm leaves
// the callable, so it never reaches the point after the `case` and its end
// state (OWNED, it stored `new Cell(4)`) is not an exit of the `case`; and
// the wildcard leaves no no-match edge. The only exit is the wildcard arm,
// which stored null, so the local is POLY-NULL at the self-assignment.
//
// The scrutinee is 0, so the returning arm does NOT run: the program is
// leak-free (the owned value the returning arm would abandon is the #8767
// getter-local disposal family, a pre-existing and separate defect), while
// the compile-time walk still sees the arm and its divergence.
BEZEL_TEST(self_assignment_getter_nested_lambda_nullable_8768,
           getter_body_case_diverging_arm_wildcard_accepted_spec_28_3_v0_359) {
    std::string src = std::string(kCellCtor) + R"(
class H
  public property Item: Cell?
    get => do
      var t: Cell? := new Cell(1);
      var k := 0;
      case k of
        1 => do
          t := new Cell(4);
          return null;
        end,
        _ => t := null,
      end case;
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

// ROW 14 — the THIRD fix-forward's must-still-REJECT row: a `case` that is
// NOT exhaustive. The scrutinee is an `int` and no arm is irrefutable, so
// §19.6 leaves the no-match fall-through edge LIVE ("an unmatched value
// falls through with no action") and the pre-case OWNED state is one of the
// `case`'s exits: the arm that stores null and the fall-through that does not
// meet at OWNED, so the §28.3 state match applies and the self-assignment is
// rejected. The METHOD twin REJECTS the same shape (MEASURED), so the getter
// twin must reject it — this is the control against a fix that drops the
// no-match edge unconditionally.
BEZEL_TEST(self_assignment_getter_nested_lambda_nullable_8768,
           getter_body_case_non_exhaustive_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kCellCtor) + R"(
class H
  public property Item: Cell?
    get => do
      var t: Cell? := new Cell(1);
      var k := 1;
      case k of
        1 => t := null,
        2 => t := null,
      end case;
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
