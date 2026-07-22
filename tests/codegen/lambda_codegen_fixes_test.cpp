// Codegen regression tests for two lambda-lowering bugs (§27/§27.5):
//
// BUG L3 (malformed IR): a block-body lambda whose inferred return type is
//   `bool` emits `ret i1` inside a function typed `i32` → IR verifier
//   "Function return type does not match operand type of return inst!".
//   Expression-body `() => true` works; only `() => do return true; end` is
//   broken.  Fix: consult the lambda's sema exprType funcReturn for block-
//   body lambdas, not just for expression-body ones.
//
// BUG L2 (miscompile): an inline IIFE that captures variables, called inside
//   a `while` loop, runs only ONCE (the loop terminates after one iteration)
//   or mutates the wrong cell (count stays 1 instead of 3).  A NAMED lambda
//   in the same loop is unaffected.  Root: the closure struct for an IIFE is
//   heap-allocated via lowerLambda at every iteration, but the env struct that
//   backs the closure is allocated once at the entry of the enclosing function
//   when `boxLocalSlot` hoists the cell-holder alloca (via `entryAlloca`) to
//   the function entry block.  On the second iteration the `calloc` for the env
//   emits inside the loop body, but the **store** of the cell pointer into the
//   env goes into a DIFFERENT allocation than the first iteration's env (each
//   iteration gets a fresh `calloc`-ed env block).  However, the closure's
//   function body reads `envPtr` from the closure struct it is passed — which
//   is the current iteration's env — and then loads the captured cell from
//   `envPtr[0]`.  So the shared cell IS reached correctly each time.  The real
//   issue is subtler: see `emitClosureCall` in LlvmBackend_Calls.cpp.  When
//   the callee is a bare `LambdaExpr` (no resolved symbol), `emitClosureCall`
//   is driven by `calleeTy = ctx_.exprType(*call.callee)`.  The sema exprType
//   for a block-body void lambda defaults funcReturn to the fallback `int`
//   TypeRef (§ExprTypecheck_Exprs.cpp L1184) when no `return expr` is
//   visible.  So `emitClosureCall` builds the function type as `i32(ptr)`, but
//   the actual lam$N function is `void(ptr)` (because the block body has no
//   explicit return and lowerScopedBlock emits `ret void`).  The mismatch
//   causes a miscompiled indirect call — UB that manifests as loop termination
//   after the first iteration.
//   Fix: infer funcReturn from the block-body stmts (same as sema does at
//   line 1158–1184), or fix `emitClosureCall` to use the actual function's
//   return type when the callee is a lambda expression.  The minimal correct
//   fix is to resolve the lambda function type's return as VOID when the
//   block-body has no `return expr` (or only `return;`) so that
//   `emitClosureCall` builds a void-typed indirect call.
//
// Spec references: §27 (lambda expressions), §27.5 (closure capture semantics)

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}

// ---------------------------------------------------------------------------
// L3: block-body lambda returning bool
// ---------------------------------------------------------------------------

// §27 / BUG-L3: block-body lambda with explicit `return true` — the function
// LLVM type must be i1 (or coerced to i32 with zext), not i32 carrying an i1.
// If broken: IR verifier fires and JIT may return wrong value or crash.
// Expected: f() returns truthy so `if f() then return 1; end` fires → 1.
BEZEL_TEST(codegen_lambda_fixes_L3, block_body_bool_return_lambda_27) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var f := lambda() => do return true; end; "
        "    if f() then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// §27 / BUG-L3: same shape — block-body lambda but returning false.
// Expected: f() returns falsy so the `if` branch is NOT taken → 0.
BEZEL_TEST(codegen_lambda_fixes_L3, block_body_bool_return_false_27) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var f := lambda() => do return false; end; "
        "    if f() then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}

// §27 / BUG-L3 regression: block-body lambda returning int must still work.
BEZEL_TEST(codegen_lambda_fixes_L3, block_body_int_return_regression_27) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var f := lambda() => do return 42; end; "
        "    return f(); "
        "  end Main; "
        "end C;"), 42);
}

// §27 / BUG-L3 regression: block-body lambda returning a string — uses string
// length to produce an int exit code.
BEZEL_TEST(codegen_lambda_fixes_L3, block_body_string_return_regression_27) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var f := lambda() => do return \"hello\"; end; "
        "    return f().Length; "
        "  end Main; "
        "end C;"), 5);
}

// §27 / BUG-L3 regression: block-body lambda with multiple stmts and
// conditional return — exercises the normal method-return path.
BEZEL_TEST(codegen_lambda_fixes_L3, block_body_conditional_bool_return_27) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var n := 5; "
        "    var isPos := lambda() => do "
        "      if n > 0 then return true; end; "
        "      return false; "
        "    end; "
        "    if isPos() then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// ---------------------------------------------------------------------------
// L2: IIFE in a loop — control case (NAMED lambda, must already pass)
// ---------------------------------------------------------------------------

// §27.5 / BUG-L2 control: a NAMED lambda stored in a local and called inside
// a while loop correctly increments a captured counter 3 times → 3.
// This already passes; included as a regression guard.
BEZEL_TEST(codegen_lambda_fixes_L2, named_lambda_in_while_loop_correct_27_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var count := 0; var i := 0; "
        "    var inc := lambda() => do count := count + 1; end; "
        "    while i < 3 do "
        "      inc(); "
        "      i := i + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;"), 3);
}

// ---------------------------------------------------------------------------
// L2: IIFE capturing a local, inside a while loop
// ---------------------------------------------------------------------------

// §27.5 / BUG-L2: an inline IIFE that captures `count` and increments it,
// called inside a `while i < 3` loop, must run 3 times → count == 3.
// If broken: count stays 1 (loop exits after first iteration due to miscompile)
// or 0 (IIFE body is skipped entirely).
BEZEL_TEST(codegen_lambda_fixes_L2, iife_capturing_local_while_loop_27_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var count := 0; var i := 0; "
        "    while i < 3 do "
        "      (lambda() => do count := count + 1; end)(); "
        "      i := i + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;"), 3);
}

// §27.5 / BUG-L2: IIFE captures and reads a separate accumulator variable
// (not the loop counter). The loop counter is NOT captured, so its boxing
// is not triggered, and the loop terminates correctly.
// A separate `total` tracks the sum of a read-only captured `factor`.
// Expected: factor=10, loop 3×, total = 3*10 = 30.
BEZEL_TEST(codegen_lambda_fixes_L2, iife_reading_outer_var_27_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var factor := 10; "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 3 do "
        "      (lambda() => do total := total + factor; end)(); "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;"), 30);
}

// §27.5 / BUG-L2: IIFE in a while loop, IIFE takes a parameter (no capture).
// This exercises the parameterized IIFE path (closure with no captured vars).
// Each call doubles the accumulator; 3 iterations: 1*2=2, 2*2=4, 4*2=8.
BEZEL_TEST(codegen_lambda_fixes_L2, iife_no_capture_while_loop_27) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var acc := 1; var i := 0; "
        "    while i < 3 do "
        "      acc := (lambda(x: int) => do return x * 2; end)(acc); "
        "      i := i + 1; "
        "    end; "
        "    return acc; "
        "  end Main; "
        "end C;"), 8);
}

// §27.5 / BUG-L2: IIFE inside a `for var` range loop. Each iteration the IIFE
// increments a captured sum by the loop variable. Sum = 1+2+3+4+5 = 15.
BEZEL_TEST(codegen_lambda_fixes_L2, iife_capturing_in_for_var_range_loop_27_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var k in 1..5 do "
        "      (lambda() => do sum := sum + k; end)(); "
        "    end; "
        "    return sum; "
        "  end Main; "
        "end C;"), 15);
}

// §27.5 / BUG-L2: two different IIFEs inside the same loop, each capturing
// the same outer `count`. Both should see the shared cell and each increment
// counts: 2 IIFEs * 3 iterations = 6 total increments.
BEZEL_TEST(codegen_lambda_fixes_L2, two_iife_same_capture_while_loop_27_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var count := 0; var i := 0; "
        "    while i < 3 do "
        "      (lambda() => do count := count + 1; end)(); "
        "      (lambda() => do count := count + 1; end)(); "
        "      i := i + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;"), 6);
}
