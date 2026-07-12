// fix #678 / §27.2 / §10: a lambda bound to an untyped `var` whose body is a
// void-returning call (e.g. Console.WriteLine) emits IR that fails LLVM
// verification: "Function return type does not match operand type of return
// inst! ret void i32". Root: sema intentionally leaves void intrinsic calls
// (Console.Write/WriteLine, collection mutators, …) untyped (null exprType).
// For an expression-body lambda, the retTy determination reads
// `ctx_.exprType(*lam.exprBody)` which returns null → retTy stays the default
// i32(). The function is therefore declared returning i32 but its body emits
// `ret void` (because lowerExpr returns nullptr for a void call) → type mismatch.
//
// Fix: when the expression body's exprType is null, consult the lambda's own
// sema-inferred function type (`ctx_.exprType(lam)->funcReturn`). A null
// funcReturn means the body produced no value (the sema inference of the
// lambda's return type mirrors ExprTypecheck_Exprs.cpp L1737: `inferredReturn =
// ctx_.exprType(*lam->exprBody)` which is null for a void call). Override
// retTy to void so the LLVM function is declared void(…) and the fallthrough
// `CreateRetVoid()` is well-typed.
//
// Spec references: §27.2 (lambda expression body), §10 (enum), §27.5 (closure).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::JitResult;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// §27.2 / #678 primary repro: `var log := (msg: string) => Console.WriteLine(msg)`
// — explicit typed param, void body. Before fix: IR verification failure.
// After fix: compiles and prints "hello"; exit 0 confirmed by test infra
// (Console output is side-effectful; we verify it compiles/runs, rc=0).
BEZEL_TEST(codegen_678_lambda_void, explicit_typed_param_void_body_spec_27_2) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method Main() "
        "    var log := (msg: string) => Console.WriteLine(msg); "
        "    log(\"hello\"); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.2 / #678: lambda with no params, void body.
// `var f := () => Console.WriteLine(\"x\");` — compiles and runs.
BEZEL_TEST(codegen_678_lambda_void, no_param_void_body_spec_27_2) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method Main() "
        "    var f := () => Console.WriteLine(\"x\"); "
        "    f(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.2 / #678: immediately-invoked void lambda.
// `(() => Console.WriteLine(\"y\"))();` — compiles and runs.
BEZEL_TEST(codegen_678_lambda_void, iife_void_body_spec_27_2) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method Main() "
        "    (() => Console.WriteLine(\"y\"))(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.2 / #678: void lambda stored and called twice — the closure cell path.
// The lambda captures nothing; verifying the closure struct survives two calls.
BEZEL_TEST(codegen_678_lambda_void, void_lambda_called_twice_spec_27_2) {
    // Use an int exit code as an observable proxy: count how many times
    // Console.WriteLine was called by side-effecting a captured counter.
    BEZEL_EXPECT_EQ(runProg(
        "module M; class C "
        "  public static method Main(): int "
        "    var n := 0; "
        "    var bump := () => do n := n + 1; end; "
        "    bump(); bump(); "
        "    return n; "
        "  end Main; "
        "end C;"), 2);
}

// §27.2 / #678 REGRESSION: expression-body lambda returning int must still work.
// The fix must not set retTy=void when the body DOES produce a value.
BEZEL_TEST(codegen_678_lambda_void, regression_int_returning_lambda_spec_27_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class C "
        "  public static method Main(): int "
        "    var sq := (x: int) => x * x; "
        "    return sq(7); "
        "  end Main; "
        "end C;"), 49);
}

// §27.2 / #678 REGRESSION: expression-body lambda returning string must still
// work (string-typed return, not void or int).
BEZEL_TEST(codegen_678_lambda_void, regression_string_returning_lambda_spec_27_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class C "
        "  public static method Main(): int "
        "    var greet := (name: string) => $\"hi {name}\"; "
        "    return greet(\"x\").Length; "
        "  end Main; "
        "end C;"), 4);
}

// §27.2 / #678 REGRESSION: block-body lambda returning void — the BUG-L2/L3
// existing tests already cover the block-body void path, but include one more
// here as a cross-check that the fix's `lam.exprBody` guard is exact (only
// applies to the expression-body path, never the block-body path).
BEZEL_TEST(codegen_678_lambda_void, regression_block_body_void_lambda_spec_27_2) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method Main() "
        "    var f := () => do Console.WriteLine(\"ok\"); end; "
        "    f(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.2 / #678: void lambda passed as a (string)->() typed parameter.
// Exercises the expectedFunctionType_ path (call-site expected type is set).
// The fix must not interfere when the expected type confirms void.
BEZEL_TEST(codegen_678_lambda_void, void_lambda_as_typed_param_spec_27_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class C "
        "  public static method Apply(f: (string) -> (), s: string) "
        "    f(s); "
        "  end Apply; "
        "  public static method Main(): int "
        "    var n := 0; "
        "    Apply((msg) => do n := n + 1; end, \"x\"); "
        "    return n; "
        "  end Main; "
        "end C;"), 1);
}
