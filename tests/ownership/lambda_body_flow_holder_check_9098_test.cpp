// Regression tests for #9098 -- the residuals of #9085. A LAMBDA body's ownership
// pre-pass (`applyUsingVarBorrowPinInLambdaStmt`) walks a body once, in source
// order, so four shapes the byte-identical METHOD twin rejects at compile time
// were still ADMITTED in a lambda body (two of them crash under ASan):
//
//   1. a `finally` that disposes `x`, then a use of its alias `y` after the
//      `try` (the post-`try` state dropped what `finally` did);
//   2. a loop whose first pass discards `x` and whose next pass invokes a
//      closure over it / reads it at the loop head (no back edge);
//   3. a lambda nested in a lambda (the enclosing lambda's capture set named
//      the nested lambda's own locals "captured", so the discard that kills
//      the holder's source was skipped).
//
// Language spec §28.2 (v0.359) "Use after a dominating explicit disposal":
//   "where a local holds a borrow of that value -- an alias (`var b := a;`)
//    ... -- taken while the variable held that value, and every value in the
//    holder's source set (K2.2) has been so disposed on every path to the use,
//    a use of the holder ... is the same compile error"; a read of a binding
//    disposed on only some paths is the conditional-discharge error (§28.3 /
//    §28.2). §28.3.10 (v0.359): "The rule is uniform across body kinds."
//
// The rows are COMPILE-TIME (`parseAndCheckOwnership`). Every REJECT row is RED
// on the base; the two ADMIT rows are base-green controls (a use BEFORE the kill
// that a back edge / `finally` must not turn into an error).

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

bool hasError(const OwnershipResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

const std::string kHead =
    "module Probe;\n"
    "public class Res\n"
    "  public tag: int;\n"
    "  public constructor(t: int) this.tag := t; end constructor;\n"
    "end Res;\n"
    "public class C\n"
    "  public static method F(flag: bool): int\n"
    "    var f := lambda(flag: bool): int => do\n";

const std::string kTail =
    "    end;\n"
    "    return f(flag);\n"
    "  end F;\n"
    "  public static method Main(): int\n"
    "    return F(true);\n"
    "  end Main;\n"
    "end C;\n";

std::string lambdaProg(const std::string& body) { return kHead + body + kTail; }

}  // namespace


// REJECT: `finally` disposes the source, the holder is used after the `try`.
BEZEL_TEST(ownership_lambda_body_flow_holder_check_9098,
           finally_dispose_then_read_alias_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var x := new Res(1);\n"
        "      var y := x;\n"
        "      var r := 0;\n"
        "      try r := 1; finally x.Dispose(); end;\n"
        "      return r + y.tag;\n"));
    BEZEL_EXPECT_TRUE(hasError(r, "use of `y` after a dominating explicit "
                                  "Dispose() of `x`"));
}

// REJECT: the same, the variable itself read after the `try` (§28.2 first
// sentence).
BEZEL_TEST(ownership_lambda_body_flow_holder_check_9098,
           finally_dispose_then_read_variable_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var x := new Res(1);\n"
        "      var r := 0;\n"
        "      try r := 1; finally x.Dispose(); end;\n"
        "      return r + x.tag;\n"));
    BEZEL_EXPECT_TRUE(hasError(r, "use of `x` after a dominating explicit "
                                  "Dispose()"));
}

// REJECT: a `finally` kill after a `try` body that returns is still a kill on
// the paths that reach the code after the `try` (the excluded-operand join).
BEZEL_TEST(ownership_lambda_body_flow_holder_check_9098,
           finally_dispose_after_returning_catch_then_read_alias_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var x := new Res(1);\n"
        "      var y := x;\n"
        "      var r := 0;\n"
        "      try r := 1; catch e: Exception => return 7; finally x.Dispose(); end;\n"
        "      return r + y.tag;\n"));
    BEZEL_EXPECT_TRUE(hasError(r, "use of `y` after a dominating explicit "
                                  "Dispose() of `x`"));
}

// REJECT: a loop discards `x` in its first pass and invokes a closure over `x`
// in the next (ASan heap-use-after-free on the base).
BEZEL_TEST(ownership_lambda_body_flow_holder_check_9098,
           loop_discard_then_invoke_closure_next_pass_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var x := new Res(1);\n"
        "      var g := lambda(): int => x.tag;\n"
        "      var s := 0;\n"
        "      for var i in [0, 1] do\n"
        "        s := s + g();\n"
        "        if i = 0 then _ := x; end;\n"
        "      end;\n"
        "      return s;\n"));
    BEZEL_EXPECT_TRUE(hasError(r, "local `x` may have already been disposed "
                                  "on another branch"));
}

// REJECT: a loop kills `x` at its end, the alias `y` is read at the loop head.
BEZEL_TEST(ownership_lambda_body_flow_holder_check_9098,
           loop_kill_at_end_then_read_alias_at_head_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var x := new Res(1);\n"
        "      var y := x;\n"
        "      var i := 0;\n"
        "      var s := 0;\n"
        "      while i < 2 do\n"
        "        s := s + y.tag;\n"
        "        if i = 1 then x.Dispose(); end;\n"
        "        i := i + 1;\n"
        "      end;\n"
        "      return s;\n"));
    BEZEL_EXPECT_TRUE(hasError(r, "local `x` may have already been disposed "
                                  "on another branch"));
}

// REJECT: a lambda nested in a lambda -- the inner body's discard kills the
// alias's source exactly as in a lambda directly in a method.
BEZEL_TEST(ownership_lambda_body_flow_holder_check_9098,
           nested_lambda_discard_then_read_alias_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var h := lambda(): int => do\n"
        "        var x := new Res(1);\n"
        "        var y := x;\n"
        "        _ := x;\n"
        "        return y.tag;\n"
        "      end;\n"
        "      return h();\n"));
    BEZEL_EXPECT_TRUE(hasError(r, "use of `y` after a dominating explicit "
                                  "Dispose() of `x`"));
}

// ADMIT (base-green control): the holder is read in the `finally` and the
// source is disposed AFTER the `try`, so no kill precedes a use.
BEZEL_TEST(ownership_lambda_body_flow_holder_check_9098,
           finally_reads_alias_then_dispose_after_try_admitted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var x := new Res(1);\n"
        "      var y := x;\n"
        "      var r := 0;\n"
        "      try r := 1; finally r := r + y.tag; end;\n"
        "      x.Dispose();\n"
        "      return r;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ADMIT (base-green control): the alias is read in every pass of a loop and
// the source is disposed after the loop -- the back edge carries no kill.
BEZEL_TEST(ownership_lambda_body_flow_holder_check_9098,
           loop_reads_alias_then_dispose_after_loop_admitted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var x := new Res(1);\n"
        "      var y := x;\n"
        "      var s := 0;\n"
        "      for var i in [0, 1] do s := s + y.tag; end;\n"
        "      x.Dispose();\n"
        "      return s;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- fix-forward: no back edge from a body that cannot reach its end ------
// §28.2 (v0.359) rejects a use reached by a disposal on some path. After an
// unconditional trailing `break` (also one inside a `do` block, or a `try`
// whose `finally` ends in one) no path returns to the loop head, so a kill
// before it reaches no later pass: the METHOD twin admits every program
// below, and the first #9098 commit falsely rejected them ("may have already
// been disposed on another branch"). Each row is RED on that commit.

// ADMIT: `for` body kills the source, then breaks unconditionally.
BEZEL_TEST(ownership_lambda_body_flow_holder_check_9098,
           for_kill_then_trailing_break_admitted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var x := new Res(1);\n"
        "      var y := x;\n"
        "      var s := 0;\n"
        "      for var i in [0, 1] do s := s + y.tag; x.Dispose(); break; end;\n"
        "      return s;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ADMIT: the same shape in a `while` loop.
BEZEL_TEST(ownership_lambda_body_flow_holder_check_9098,
           while_kill_then_trailing_break_admitted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var x := new Res(1);\n"
        "      var y := x;\n"
        "      var i := 0;\n"
        "      var s := 0;\n"
        "      while i < 2 do s := s + y.tag; x.Dispose(); break; end;\n"
        "      return s;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ADMIT: the kill is a `try ... finally x.Dispose(); end`, then `break`.
BEZEL_TEST(ownership_lambda_body_flow_holder_check_9098,
           loop_try_finally_kill_then_trailing_break_admitted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var x := new Res(1);\n"
        "      var y := x;\n"
        "      var s := 0;\n"
        "      while true do\n"
        "        s := s + y.tag;\n"
        "        try s := s + 1; finally x.Dispose(); end;\n"
        "        break;\n"
        "      end;\n"
        "      return s;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ADMIT: the trailing `break` sits inside a `do` block.
BEZEL_TEST(ownership_lambda_body_flow_holder_check_9098,
           loop_kill_then_break_in_do_block_admitted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var x := new Res(1);\n"
        "      var y := x;\n"
        "      var s := 0;\n"
        "      while true do s := s + y.tag; do x.Dispose(); break; end; end;\n"
        "      return s;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ADMIT: an inner loop's kill+break inside an outer loop that itself breaks.
BEZEL_TEST(ownership_lambda_body_flow_holder_check_9098,
           nested_loops_inner_kill_break_outer_break_admitted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var x := new Res(1);\n"
        "      var y := x;\n"
        "      var s := 0;\n"
        "      for var i in [0, 1] do\n"
        "        s := s + y.tag;\n"
        "        for var j in [0, 1] do x.Dispose(); break; end;\n"
        "        break;\n"
        "      end;\n"
        "      return s;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ADMIT: a loop inside a `finally`, ending in kill+break.
BEZEL_TEST(ownership_lambda_body_flow_holder_check_9098,
           finally_loop_kill_then_trailing_break_admitted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(lambdaProg(
        "      var x := new Res(1);\n"
        "      var y := x;\n"
        "      var r := 0;\n"
        "      try r := 1; finally while true do r := r + y.tag; x.Dispose(); break; end; end;\n"
        "      return r;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
