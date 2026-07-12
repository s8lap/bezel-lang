// fix #747 R43 / §27.2 / §14.1: a lambda whose expression body calls a user
// method on a class-typed parameter — `(x) => x.N()` passed as a `(Foo) -> int`
// function argument — emitted `ret void` into the i32-returning lambda function
// (IR verification failure: "Function return type does not match operand type
// of return inst").
//
// Root cause: the lambda parameter `x` is unannotated, so sema leaves it
// untyped and does NOT resolve the method call `x.N()` (no MethodSymbol on the
// CallExpr's callee). Codegen injects the concrete parameter type via
// `lambdaParamTypeHint_` (which already lets `x.field` / `x.ToString()`
// resolve), but the user-method dispatch path did not consult that hint, so
// `lowerCall(x.N())` returned null and the expr-body lambda emitted `ret void`.
//
// Fix: when the call's callee symbol is unresolved and the receiver is a
// hinted lambda parameter of a concrete class/record, resolve the MethodSymbol
// by name from the hinted type's members so the ordinary instance-dispatch path
// handles the call.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §27.2: `(x) => x.N()` where x: Foo and N(): int — the canonical repro. Was
// IR_FAIL (ret void in an i32 function); now compiles and returns 42.
BEZEL_TEST(codegen_lambda_method_call_on_param_747, method_no_args_returns_int) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Foo "
        "  public method N(): int return 42; end N; "
        "end Foo; "
        "class C "
        "  public static method Apply(fn: (Foo) -> int, f: Foo): int "
        "    return fn(f); "
        "  end Apply; "
        "  public static method Main(): int "
        "    using var foo := new Foo(); "
        "    return C.Apply((x) => x.N(), foo); "
        "  end Main; "
        "end C;"), 42);
}

// §27.2: the method takes arguments — `(x) => x.Bump(5)` → 105.
BEZEL_TEST(codegen_lambda_method_call_on_param_747, method_with_args) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Foo "
        "  public method Bump(d: int): int return 100 + d; end Bump; "
        "end Foo; "
        "class C "
        "  public static method Apply(fn: (Foo) -> int, f: Foo): int "
        "    return fn(f); "
        "  end Apply; "
        "  public static method Main(): int "
        "    using var foo := new Foo(); "
        "    return C.Apply((x) => x.Bump(5), foo); "
        "  end Main; "
        "end C;"), 105);
}

// §14.1: the original report's generic `Apply<U>` scenario — the lambda body
// method call drives the return type through monomorphization (U=int) → 7.
BEZEL_TEST(codegen_lambda_method_call_on_param_747, generic_apply_method_call) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Foo "
        "  public method N(): int return 7; end N; "
        "end Foo; "
        "class C "
        "  public static method Apply<U>(fn: (Foo) -> U, f: Foo): U "
        "    return fn(f); "
        "  end Apply; "
        "  public static method Main(): int "
        "    using var foo := new Foo(); "
        "    return C.Apply<int>((x) => x.N(), foo); "
        "  end Main; "
        "end C;"), 7);
}

// CONTROL §27.2: a lambda body with NO method call — `(x) => 42` — already
// worked; the fix must not regress it.
BEZEL_TEST(codegen_lambda_method_call_on_param_747, control_no_method_call) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Foo "
        "  public method N(): int return 1; end N; "
        "end Foo; "
        "class C "
        "  public static method Apply(fn: (Foo) -> int, f: Foo): int "
        "    return fn(f); "
        "  end Apply; "
        "  public static method Main(): int "
        "    using var foo := new Foo(); "
        "    return C.Apply((x) => 42, foo); "
        "  end Main; "
        "end C;"), 42);
}
