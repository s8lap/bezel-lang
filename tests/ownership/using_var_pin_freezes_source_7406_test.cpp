// Spec-derived regression tests for issue #7406 (campaign
// `using-heads`, U1 soundness: the `using var` pin and explicit Dispose).
//
// #7406 -- a `using var` borrow-pin did not freeze its source. §28.10 *`using
//   var` initialized from a borrow* (v0.350): the `using var` "still **pins**
//   the borrow's source for the duration of the scope ... the borrow stays
//   **live** until the using-var's scope ends, exactly as if it were used at
//   every point up to that end, and the consume-site freeze then governs
//   every consume of the source in that range -- the frozen kills (a move, a
//   transfer, a reassignment of the binding) are compile errors". Before the
//   fix `var b := a; using var p := b; R.Take(move a); p.Use();` compiled and
//   `p.Use()` read freed memory (heap-use-after-free in `R$Use`).
//
// The pin's scope is its INNERMOST enclosing statement list -- an `if` /
// `else` branch, a `try` body, a `catch` / `case` arm, a loop body, a lambda
// body -- not only a `do ... end` (§28.10 "until the using-var's scope
// ends"), and the pinned source is the one the pin's initializer named AT
// THE PIN, not the one a later rebind of the alias names.
//
// Every REJECT row asserts the specific diagnostic (never "did not crash",
// which is vacuous for a use-after-free). Every ACCEPT row runs the program
// under the AOT/ASan oracle and asserts its stdout.

#include <string>
#include <string_view>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// The issue bodies' module, verbatim, with the method `Body` swapped out.
// With `twoWay`, `Body` takes `k: int` and `Main` runs `Body(0)` then
// `Body(1)`, so a row whose branches key on `k` executes both of them.
// `extra` adds members to `R` before `Body` (a value-typed field or
// property the value-type rows read).
std::string probe(const std::string& body, bool twoWay = false,
                  const std::string& extra = "") {
    return std::string(R"(module Probe;

public sealed class Q
  private _id: int;
  public constructor(id: int)
    _id := id;
  end constructor;
  destructor
    Console.WriteLine($"D:{_id}");
  end destructor;
  public method Use()
    Console.WriteLine($"U:{_id}");
  end Use;
end Q;

public sealed class R
  private _id: int;
  public owned f: Q;
  public constructor(id: int, owned q: Q)
    _id := id;
    f := q;
  end constructor;
  destructor
    Console.WriteLine($"D:{_id}");
  end destructor;
  public method Use()
    Console.WriteLine($"U:{_id}");
  end Use;
  public static method Take(owned r: R)
    using var k := move r;
  end Take;
  public static method Pick<T>(x: T, y: T): T
    return x;
  end Pick;
)" + extra + R"(  public static method Body)") + (twoWay ? "(k: int)" : "()") + R"(
)" + body + R"(
  end Body;
end R;

public class Program
  public static method Main(): int
)" + (twoWay ? "    R.Body(0);\n    R.Body(1);\n" : "    R.Body();\n") + R"(
    Console.WriteLine("END");
    return 0;
  end Main;
end Program;
)";
}

constexpr std::string_view kFreezeHead = "cannot consume `a` here";
constexpr std::string_view kFreezeHolder = "borrow held by `p`";

}  // namespace

// ---------------------------------------------------------------------------
// #7406 -- REJECT (§28.10 *initialized from a borrow*, K5.1/K5.5, v0.350)
// ---------------------------------------------------------------------------

// The issue's literal repro: the pin is over a borrow `b` of `a`.
BEZEL_TEST(using_var_pin_freeze_7406,
           move_of_pinned_source_while_pin_in_scope_rejected_spec_28_10) {
    auto r = parseAndCheckOwnership(probe(R"(
    var a := new R(2, new Q(1));
    var b := a;
    using var p := b;
    R.Take(move a);
    p.Use();)"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHead));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
}

// "exactly as if it were used at every point up to that end": the pin is not
// read after the transfer and the transfer is still a frozen kill.
BEZEL_TEST(using_var_pin_freeze_7406,
           move_of_pinned_source_without_later_use_of_pin_rejected_spec_28_10) {
    auto r = parseAndCheckOwnership(probe(R"(
    var a := new R(2, new Q(1));
    var b := a;
    using var p := b;
    R.Take(move a);)"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHead));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
}

// K5.5: liveness is traced "through `using var` pins" across a chain of
// borrow-state aliases (`c` -> `b` -> `a`).
BEZEL_TEST(using_var_pin_freeze_7406,
           move_of_pinned_source_through_two_alias_hops_rejected_spec_28_10) {
    auto r = parseAndCheckOwnership(probe(R"(
    var a := new R(2, new Q(1));
    var b := a;
    var c := b;
    using var p := c;
    R.Take(move a);
    p.Use();)"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHead));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
}

// §28.10 names a field read as a borrow shape: the pin of `a.f` pins `a`.
BEZEL_TEST(using_var_pin_freeze_7406,
           move_of_holder_of_pinned_field_read_rejected_spec_28_10) {
    auto r = parseAndCheckOwnership(probe(R"(
    var a := new R(2, new Q(1));
    using var p := a.f;
    R.Take(move a);
    p.Use();)"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHead));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
}

// ---------------------------------------------------------------------------
// #7406 -- ACCEPT mirrors (the fix must not over-reject), run under ASan
// ---------------------------------------------------------------------------

// The pin's scope has ended: the source is movable again.
BEZEL_TEST(using_var_pin_freeze_7406,
           move_of_source_after_pin_scope_ends_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var a := new R(2, new Q(1));
    var b := a;
    do
      using var p := b;
      p.Use();
    end;
    R.Take(move a);)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("U:2\nD:2\nD:1\nEND\n"));
}

// A pin of a DIFFERENT source does not freeze `a`.
BEZEL_TEST(using_var_pin_freeze_7406,
           pin_of_other_source_leaves_this_source_movable_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var a := new R(2, new Q(1));
    var c := new R(4, new Q(3));
    var b := c;
    using var p := b;
    R.Take(move a);
    p.Use();)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A `using var` initialized by `move` is an adoption, not a pin: it freezes
// nothing, and a different owned local stays movable.
BEZEL_TEST(using_var_pin_freeze_7406,
           using_var_initialized_by_move_is_no_pin_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var a := new R(2, new Q(1));
    var c := new R(4, new Q(3));
    using var p := move c;
    R.Take(move a);
    p.Use();)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A borrow read of the source while the pin is in scope is no consume; the
// move after the pin's scope ends is admitted.
BEZEL_TEST(using_var_pin_freeze_7406,
           borrow_read_of_source_while_pin_in_scope_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var a := new R(2, new Q(1));
    var b := a;
    do
      using var p := b;
      a.Use();
      p.Use();
    end;
    R.Take(move a);)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("U:2\nU:2\nD:2\nD:1\nEND\n"));
}

// ---------------------------------------------------------------------------
// #7406 -- the pin's scope is its innermost block (review of 34cdde3c).
// REJECT: a consume INSIDE the block that holds the pin.
// ---------------------------------------------------------------------------

BEZEL_TEST(using_var_pin_freeze_7406,
           move_inside_if_branch_holding_pin_rejected_spec_28_10) {
    auto r = parseAndCheckOwnership(probe(R"(
    var a := new R(2, new Q(1));
    var b := a;
    var k := 1;
    if k = 1 then
      using var p := b;
      R.Take(move a);
      p.Use();
    end;)"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHead));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
}

BEZEL_TEST(using_var_pin_freeze_7406,
           move_inside_else_branch_holding_pin_rejected_spec_28_10) {
    auto r = parseAndCheckOwnership(probe(R"(
    var a := new R(2, new Q(1));
    var b := a;
    var k := 1;
    if k = 1 then
      Console.WriteLine("then");
    else
      using var p := b;
      R.Take(move a);
    end;)"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHead));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
}

BEZEL_TEST(using_var_pin_freeze_7406,
           move_inside_try_body_holding_pin_rejected_spec_28_10) {
    auto r = parseAndCheckOwnership(probe(R"(
    var a := new R(2, new Q(1));
    var b := a;
    try
      using var p := b;
      R.Take(move a);
      p.Use();
    finally
      Console.WriteLine("F");
    end;)"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHead));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
}

BEZEL_TEST(using_var_pin_freeze_7406,
           move_inside_catch_arm_holding_pin_rejected_spec_28_10) {
    auto r = parseAndCheckOwnership(probe(R"(
    var a := new R(2, new Q(1));
    var b := a;
    try
      throw new Exception("x");
    catch e: Exception => do
      using var p := b;
      R.Take(move a);
      p.Use();
    end
    end;)"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHead));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
}

BEZEL_TEST(using_var_pin_freeze_7406,
           move_inside_case_arm_holding_pin_rejected_spec_28_10) {
    auto r = parseAndCheckOwnership(probe(R"(
    var a := new R(2, new Q(1));
    var b := a;
    var k := 0;
    case k of
      0 => do
        using var p := b;
        R.Take(move a);
        p.Use();
      end,
      _ => Console.WriteLine("other")
    end case;)"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHead));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
}

BEZEL_TEST(using_var_pin_freeze_7406,
           move_inside_if_in_lambda_body_holding_pin_rejected_spec_28_10) {
    auto r = parseAndCheckOwnership(probe(R"(
    var f := lambda() =>
      do
        var a := new R(2, new Q(1));
        var b := a;
        var k := 1;
        if k = 1 then
          using var p := b;
          R.Take(move a);
          p.Use();
        end;
      end;
    f();)"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHead));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
}

// ACCEPT: a consume AFTER the block that holds the pin has closed. Each
// two-way row runs both branches (`Body(0)`, then `Body(1)`).

BEZEL_TEST(using_var_pin_freeze_7406,
           move_after_if_without_else_holding_pin_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var a := new R(2, new Q(1));
    var b := a;
    if k = 0 then
      using var p := b;
      p.Use();
    end;
    R.Take(move a);)", true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("U:2\nD:2\nD:1\nD:2\nD:1\nEND\n"));
}

BEZEL_TEST(using_var_pin_freeze_7406,
           move_after_if_else_holding_pin_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var a := new R(2, new Q(1));
    var b := a;
    if k = 0 then
      using var p := b;
      p.Use();
    else
      R.Take(move a);
      return;
    end;
    R.Take(move a);)", true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("U:2\nD:2\nD:1\nD:2\nD:1\nEND\n"));
}

BEZEL_TEST(using_var_pin_freeze_7406,
           move_after_try_body_holding_pin_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var a := new R(2, new Q(1));
    var b := a;
    try
      using var p := b;
      p.Use();
    finally
      Console.WriteLine("F");
    end;
    R.Take(move a);)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("U:2\nF\nD:2\nD:1\nEND\n"));
}

BEZEL_TEST(using_var_pin_freeze_7406,
           move_after_case_arm_holding_pin_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var a := new R(2, new Q(1));
    var b := a;
    case k of
      0 => do
        using var p := b;
        p.Use();
      end,
      _ => Console.WriteLine("other")
    end case;
    R.Take(move a);)", true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("U:2\nD:2\nD:1\nother\nD:2\nD:1\nEND\n"));
}

BEZEL_TEST(using_var_pin_freeze_7406,
           move_after_if_in_lambda_body_holding_pin_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var f := lambda() =>
      do
        var a := new R(2, new Q(1));
        var b := a;
        var k := 1;
        if k = 1 then
          using var p := b;
          p.Use();
        end;
        R.Take(move a);
      end;
    f();)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("U:2\nD:2\nD:1\nEND\n"));
}

// The review's `u_if_pin_then_consume_in_sibling_else`: the consume is in the
// SIBLING branch, which the pin's scope does not reach. The explicit
// `a.Dispose()` under the pin is the site-announced kill §28.10 leaves
// unfrozen ("an explicit `Dispose()` of the source is permitted").
BEZEL_TEST(using_var_pin_freeze_7406,
           move_in_sibling_else_of_if_holding_pin_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var a := new R(2, new Q(1));
    var b := a;
    if k = 0 then
      using var p := b;
      p.Use();
      a.Dispose();
    else
      R.Take(move a);
    end;)", true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("U:2\nD:2\nD:1\nD:2\nD:1\nEND\n"));
}

// The `if` half of the review's `r_pin_arr`: a pin over a borrow of a
// built-in collection, inside an `if`, then a transfer after the `if`.
BEZEL_TEST(using_var_pin_freeze_7406,
           move_of_collection_after_if_holding_pin_accepted_spec_28_10) {
    auto r = runAotChecked(R"(module Probe;
public class P
  public static method Take(owned a: Array<int>)
    using var k := move a;
    Console.WriteLine($"T:{k.Count}");
  end Take;
  public static method Main(): int
    var arr := new Array<int>(3, 7);
    var b := arr;
    if arr.Count > 1 then
      using var it := b;
      Console.WriteLine($"{it.Count}");
    end;
    P.Take(move arr);
    Console.WriteLine("END");
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nT:3\nEND\n"));
}

// ---------------------------------------------------------------------------
// #7406 -- the pinned source is the one named AT THE PIN (review of 34cdde3c)
// ---------------------------------------------------------------------------

// The review's `b_rebind_after_pin`: `p` pinned `c`; rebinding the alias `b`
// to `a` afterwards does not make `p` pin `a`.
BEZEL_TEST(using_var_pin_freeze_7406,
           move_of_alias_rebind_target_after_pin_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var a := new R(2, new Q(1));
    var c := new R(4, new Q(3));
    var b := c;
    using var p := b;
    b := a;
    R.Take(move a);
    p.Use();)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("D:2\nD:1\nU:4\nD:4\nD:3\nEND\n"));
}

// The reject mirror: the source `p` DID pin stays frozen after the rebind.
BEZEL_TEST(using_var_pin_freeze_7406,
           move_of_pinned_source_after_alias_rebind_rejected_spec_28_10) {
    auto r = parseAndCheckOwnership(probe(R"(
    var a := new R(2, new Q(1));
    var c := new R(4, new Q(3));
    var b := c;
    using var p := b;
    b := a;
    R.Take(move c);
    p.Use();)"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "cannot consume `c` here"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
}

// A rebind BEFORE the pin: `p` pins `c`, the alias's source at the pin.
BEZEL_TEST(using_var_pin_freeze_7406,
           move_of_alias_source_rebound_away_before_pin_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var a := new R(2, new Q(1));
    var c := new R(4, new Q(3));
    var b := a;
    b := c;
    using var p := b;
    R.Take(move a);
    p.Use();)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("D:2\nD:1\nU:4\nD:4\nD:3\nEND\n"));
}

// "a reassignment of the binding" is a frozen kill too.
BEZEL_TEST(using_var_pin_freeze_7406,
           reassignment_of_pinned_source_rejected_spec_28_10) {
    auto r = parseAndCheckOwnership(probe(R"(
    var a := new R(2, new Q(1));
    var b := a;
    using var p := b;
    a := new R(4, new Q(3));
    p.Use();)"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHead));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
}

// A bare-type-parameter call pin (#1801 S4) pins every T-reaching argument's
// source -- here with no later read of the pin, so only the pin keeps the
// borrow live.
BEZEL_TEST(using_var_pin_freeze_7406,
           move_of_bare_type_param_call_pin_source_rejected_spec_28_10) {
    auto r = parseAndCheckOwnership(probe(R"(
    var a := new R(2, new Q(1));
    var c := new R(4, new Q(3));
    using var p := R.Pick(a, c);
    R.Take(move c);)"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "cannot consume `c` here"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
}

// A CONDITIONAL rebind before the pin may have made `b` alias `a` by the pin.
BEZEL_TEST(using_var_pin_freeze_7406,
           move_of_conditional_rebind_source_before_pin_rejected_spec_28_10) {
    auto r = parseAndCheckOwnership(probe(R"(
    var a := new R(2, new Q(1));
    var c := new R(4, new Q(3));
    var b := c;
    if k = 0 then
      b := a;
    end;
    using var p := b;
    R.Take(move a);
    p.Use();)", true));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHead));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
}

// ...but a conditional rebind AFTER the pin leaves the pin on `c`.
BEZEL_TEST(using_var_pin_freeze_7406,
           move_of_conditional_rebind_source_after_pin_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var a := new R(2, new Q(1));
    var c := new R(4, new Q(3));
    var b := c;
    using var p := b;
    if k = 0 then
      b := a;
    end;
    R.Take(move a);
    p.Use();)", true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("D:2\nD:1\nU:4\nD:4\nD:3\n"
                                "D:2\nD:1\nU:4\nD:4\nD:3\nEND\n"));
}

// A field read through the alias pins the alias's source.
BEZEL_TEST(using_var_pin_freeze_7406,
           move_of_source_of_alias_whose_field_is_pinned_rejected_spec_28_10) {
    auto r = parseAndCheckOwnership(probe(R"(
    var a := new R(2, new Q(1));
    var b := a;
    using var p := b.f;
    R.Take(move a);)"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHead));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
}

// A drop-on-overwrite of the owner of a pinned field read is frozen.
BEZEL_TEST(using_var_pin_freeze_7406,
           reassignment_of_owner_of_pinned_field_read_rejected_spec_28_10) {
    auto r = parseAndCheckOwnership(probe(R"(
    var a := new R(2, new Q(1));
    using var p := a.f;
    a := new R(4, new Q(3));
    p.Use();)"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHead));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
}

// Rebinding the MIDDLE of an alias chain after the pin does not unpin `a`.
BEZEL_TEST(using_var_pin_freeze_7406,
           move_of_chain_root_after_middle_alias_rebind_rejected_spec_28_10) {
    auto r = parseAndCheckOwnership(probe(R"(
    var a := new R(2, new Q(1));
    var d := new R(4, new Q(3));
    var b := a;
    var c := b;
    using var p := c;
    b := d;
    R.Take(move a);)"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHead));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
}

// ---------------------------------------------------------------------------
// #7406 -- a `using var` whose type holds no borrow pins nothing (review of
// c666ce43). §28.10: "`using var` accepts a value-type or runtime-managed
// right-hand side ... the only effect is the reassignability restriction";
// §28.3.9: a value-typed read "is no borrow". ACCEPT, run under ASan.
// ---------------------------------------------------------------------------

// The review's B03: an `int` field read is a copy, not a borrow of `a`.
BEZEL_TEST(using_var_pin_freeze_7406,
           int_field_read_using_var_leaves_source_movable_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var a := new R(2, new Q(1));
    using var n := a.num;
    R.Take(move a);
    Console.WriteLine($"{n}");)", false, "  public num: int;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("D:2\nD:1\n0\nEND\n"));
}

// The review's B08: a user getter returning an `int` is no borrow of `a`.
BEZEL_TEST(using_var_pin_freeze_7406,
           int_property_read_using_var_leaves_source_movable_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var a := new R(2, new Q(1));
    using var n := a.Num;
    R.Take(move a);
    Console.WriteLine($"{n}");)", false,
                                 "  public property Num: int\n"
                                 "    get => _id;\n"
                                 "  end Num;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("D:2\nD:1\n2\nEND\n"));
}

// The review's B05b: a `string?` field read is runtime-managed (§28.9), no
// borrow of `a`.
BEZEL_TEST(using_var_pin_freeze_7406,
           string_field_read_using_var_leaves_source_movable_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var a := new R(2, new Q(1));
    using var s := a.nm;
    R.Take(move a);
    Console.WriteLine(s ?? "nil");)", false,
                                 "  public nm: string? := null;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("D:2\nD:1\nnil\nEND\n"));
}

// The review's B06: an `Array<int>` element read copies the `int`; the array
// stays movable.
BEZEL_TEST(using_var_pin_freeze_7406,
           int_array_element_using_var_leaves_array_movable_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var xs := new Array<int>(3, 7);
    using var n := xs[0];
    using var ys := move xs;
    Console.WriteLine($"{n}");)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\nEND\n"));
}

// The review's B12: the displacement row too -- reassigning `a` under an
// `int`-typed `using var` read from it is not a frozen kill.
BEZEL_TEST(using_var_pin_freeze_7406,
           reassignment_of_source_under_int_field_using_var_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var a := new R(2, new Q(1));
    using var n := a.num;
    a := new R(3, new Q(4));
    Console.WriteLine($"{n}");)", false, "  public num: int;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("D:2\nD:1\n0\nD:3\nD:4\nEND\n"));
}

// ---------------------------------------------------------------------------
// #7406 -- only a rebind that REACHES the pin names a pinned source (review
// of c666ce43): a rebind killed by a later rebind that dominates the pin, or
// one in a branch disjoint from the pin, is not what the alias holds at the
// pin. ACCEPT, run under ASan.
// ---------------------------------------------------------------------------

// The review's A26: the conditional `b := a` is killed by the unconditional
// `b := c` before the pin, so `p` pins `c` only.
BEZEL_TEST(using_var_pin_freeze_7406,
           conditional_rebind_killed_by_unconditional_rebind_before_pin_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var a := new R(2, new Q(1));
    var c := new R(3, new Q(4));
    var b := c;
    if k = 0 then
      b := a;
    end;
    b := c;
    using var p := b;
    R.Take(move a);
    p.Use();)", true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("D:2\nD:1\nU:3\nD:3\nD:4\n"
                                "D:2\nD:1\nU:3\nD:3\nD:4\nEND\n"));
}

// The review's A28: a rebind inside a loop body before the pin is likewise
// killed by the unconditional rebind that follows the loop.
BEZEL_TEST(using_var_pin_freeze_7406,
           loop_rebind_killed_by_unconditional_rebind_before_pin_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var a := new R(2, new Q(1));
    var c := new R(3, new Q(4));
    var b := c;
    for var i in 0..<1 do
      b := a;
    end;
    b := c;
    using var p := b;
    R.Take(move a);
    p.Use();)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("D:2\nD:1\nU:3\nD:3\nD:4\nEND\n"));
}

// The review's A46: the killed rebind's right-hand side is a field read
// (`a.f`), whose root `a` must not be pinned either.
BEZEL_TEST(using_var_pin_freeze_7406,
           conditional_field_rebind_killed_before_pin_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var a := new R(2, new Q(1));
    var c := new R(3, new Q(4));
    var b := c.f;
    if k = 0 then
      b := a.f;
    end;
    b := c.f;
    using var p := b;
    R.Take(move a);
    p.Use();)", true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("D:2\nD:1\nU:4\nD:3\nD:4\n"
                                "D:2\nD:1\nU:4\nD:3\nD:4\nEND\n"));
}

// The review's A27b: the rebind `b := a` sits in the `if` branch, the pin in
// the `else` branch -- one run of the `if` takes one of them, so the rebind
// never reaches the pin and `p` pins `c` only.
BEZEL_TEST(using_var_pin_freeze_7406,
           rebind_in_sibling_branch_of_pin_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    var a := new R(2, new Q(1));
    var c := new R(3, new Q(4));
    var b := c;
    if k = 0 then
      b := a;
      b.Use();
      a.Dispose();
    else
      using var p := b;
      R.Take(move a);
      p.Use();
    end;)", true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("U:2\nD:2\nD:1\nD:3\nD:4\n"
                                "D:2\nD:1\nU:3\nD:3\nD:4\nEND\n"));
}
