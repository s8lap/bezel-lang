// fix #8782 / §28.3 *State match on reassignment* (spec v0.359): `u := v;` of an
// unmarked read of ANOTHER binding into an owned-state local, inside a lambda
// body.
//
// §28.3: "Assigning a borrow to an owned-state LHS ... is rejected", and item 1:
// "an owned-state LHS requires an owned RHS — a fresh producer, or a named owned
// binding under the explicit transfer marker (`x := move y;`)". An unmarked read
// of a binding is a borrow. The rule names no scope. The method-body arm
// (`handleAssignment`) rejects it ("cannot assign `v` to `u`"); a lambda body's
// assignments reach only a deliberately narrow mirror of that arm, so the store
// compiled (rc=0) and core-dumped at run time (MEASURED on 227d95a17): `u`'s
// drop-on-overwrite and the scope-end drops disposed `v`'s value twice.
//
// Each row is red on the base, asserts the specific diagnostic AND its location
// (the right-hand side `v`), and is the issue's repro shape (row 1) or its
// borrowed-RHS siblings (rows 2, 3). The admitting twins (`u := move v`, a
// borrow assigned to a borrow, a fresh producer, a primitive) are corpus
// candidates.
//
// per §28.3, v0.359, #8782

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

constexpr const char* kStateMatch = "is in the OWNED state and the right-hand side is an unmarked read of";

const char* kCell = R"(
module M;
class Cell
  public value: int := 0;
  public constructor() end constructor;
end Cell;
)";

// ROW 1 — the issue's repro: two owned locals inside a block lambda.
BEZEL_TEST(captured_owned_local_assign_borrow_in_closure_8782,
           lambda_owned_local_assigned_other_owned_local_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kCell) + R"(
class Program
  public static method Main(): int
    var f := lambda(): int => do var u := new Cell(); var v := new Cell(); u := v; return 0; end;
    return f();
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":10:81: cannot assign `v` to `u`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kStateMatch));
}

// ROW 2 — the RHS is a captured BORROWED parameter.
BEZEL_TEST(captured_owned_local_assign_borrow_in_closure_8782,
           lambda_owned_local_assigned_captured_borrowed_param_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kCell) + R"(
class Program
  public static method G(p: Cell): int
    var f := lambda(): int => do var u := new Cell(); u := p; return u.value; end;
    return f();
  end G;
  public static method Main(): int
    return G(new Cell());
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":10:60: cannot assign `p` to `u`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kStateMatch));
}

// ROW 3 — the RHS is a captured BORROWED local (`var b := c;`).
BEZEL_TEST(captured_owned_local_assign_borrow_in_closure_8782,
           lambda_owned_local_assigned_captured_borrowed_local_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kCell) + R"(
class Program
  public static method Main(): int
    var c := new Cell();
    var b := c;
    var f := lambda(): int => do var u := new Cell(); u := b; return u.value; end;
    return f();
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":12:60: cannot assign `b` to `u`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kStateMatch));
}

}  // namespace
