// fix #7109 / §28.3 *State match on reassignment*, bullet 1 (spec v0.359): the
// self-assignment rule in a LAMBDA body and in a GETTER body.
//
// The spec sentence names no scope: "A self-assignment is no exception: for an
// owned-state class-typed local `t`, `t := t;` stores the unmarked read `t`, a
// borrow, into an owned-state LHS and is rejected at the assignment by this
// rule, not a §28.7 discharge." The method-body arm (`handleAssignment`,
// #6609) rejected it; a lambda body's assignments reach only a deliberately
// narrow mirror of that arm, and a getter accessor body never reaches it, so
// both spellings compiled (MEASURED on b279e8daf, rc=0). The loop-body
// spelling of the issue is already rejected on the base (#8328) and is not
// pinned here.
//
// Both rows are red on the base and green with the fix. The admitting twins
// (`t := move t`, a borrowed-state local, a primitive) are corpus candidates,
// not gtest rows.
//
// per §28.3, §15.3, §28.7, v0.359, #7109

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

constexpr const char* kSelfAssign = "cannot self-assign";

const char* kCell = R"(
module M;
class Cell
  public value: int := 0;
end Cell;
)";

// ROW 1 — the issue's (a): a lambda body.
BEZEL_TEST(self_assignment_lambda_getter_body_7109,
           lambda_body_owned_local_self_assign_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kCell) + R"(
class Program
  public static method Main(): int
    var f := lambda(): int => do var u := new Cell(); u := u; return 0; end;
    return f();
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSelfAssign));
}

// ROW 2 — the issue's (b): a getter body.
BEZEL_TEST(self_assignment_lambda_getter_body_7109,
           getter_body_owned_local_self_assign_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kCell) + R"(
class H
  public property Item: Cell?
    get => do
      var t: Cell? := new Cell();
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
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSelfAssign));
}

}  // namespace
