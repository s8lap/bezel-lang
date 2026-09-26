// Codegen slice CG60 — lambda expressions (§A.7, §17.14). Lambdas lower to
// nested LLVM functions and are passed as function-pointer values. The slice
// covers:
//   - expression-body lambdas `(p: T) => expr`
//   - explicit parameter type annotations (inferred params are later)
//   - passing lambdas as function-typed parameters `(T) -> R`
//   - storing lambdas in locals and passing them later
// Captures (closures over outer variables) are deferred.

#include "support/aot_support.hpp"   // runAot — strict AOT oracle for ported rows
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
// Rows ported off the deprecated lenient JIT oracle (#4419). Only rows that
// pass the strict AOT path are ported; the file-wide switch also turns
// method_reference_stored_in_local, once_pattern, partial_application and
// function_typed_field_call red under runAot, which is out of #4419's scope.
int runA(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}

// §17.14 / §A.7: apply a lambda to an integer argument.
BEZEL_TEST(codegen_cg60, lambda_apply_basic) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Apply(n: int, f: (int) -> int): int "
        "    return f(n); "
        "  end Apply; "
        "  public static method Main(): int "
        "    return Apply(5, lambda(x: int) => x * 2); "
        "  end Main; "
        "end C;"), 10);
}

BEZEL_TEST(codegen_cg60, lambda_expression_body) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Map(n: int, f: (int) -> int): int "
        "    return f(n); "
        "  end Map; "
        "  public static method Main(): int "
        "    return Map(7, lambda(x: int) => x + 3); "
        "  end Main; "
        "end C;"), 10);
}

BEZEL_TEST(codegen_cg60, lambda_stored_in_local) {
    // A lambda value can be stored in a local and invoked later.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Apply(n: int, f: (int) -> int): int "
        "    return f(n); "
        "  end Apply; "
        "  public static method Main(): int "
        "    var sq := lambda(a: int) => a * a; "
        "    return Apply(4, sq); "
        "  end Main; "
        "end C;"), 16);
}

BEZEL_TEST(codegen_cg60, lambda_multi_param) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Combine(a: int, b: int, "
        "                               f: (int, int) -> int): int "
        "    return f(a, b); "
        "  end Combine; "
        "  public static method Main(): int "
        "    return Combine(3, 4, lambda(x: int, y: int) => x + y); "
        "  end Main; "
        "end C;"), 7);
}

BEZEL_TEST(codegen_cg60, lambda_uses_outer_constant) {
    // Lambda captures a compile-time constant; capture of mutable outer
    // locals is deferred, but compile-time values work as expressions.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Apply(n: int, f: (int) -> int): int "
        "    return f(n); "
        "  end Apply; "
        "  public static method Main(): int "
        "    return Apply(6, lambda(x: int) => x * 3); "
        "  end Main; "
        "end C;"), 18);
}

BEZEL_TEST(codegen_cg60, lambda_passed_twice) {
    // Same lambda variable passed to two different calls.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Apply(n: int, f: (int) -> int): int "
        "    return f(n); "
        "  end Apply; "
        "  public static method Main(): int "
        "    var dbl := lambda(x: int) => x * 2; "
        "    return Apply(3, dbl) + Apply(4, dbl); "
        "  end Main; "
        "end C;"), 14);
}

// ── Closures: lambdas that capture outer variables by value ─────────────────
// Each lambda is stored as a heap-allocated { fn_ptr, env_ptr } struct.
// The env contains a copy of any outer variables referenced in the body.

BEZEL_TEST(codegen_cg60, closure_captures_param) {
    // §A.7: lambda captures a method parameter by value.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method MakeAdder(n: int): (int) -> int "
        "    return lambda(x: int) => x + n; "
        "  end MakeAdder; "
        "  public static method Main(): int "
        "    var add5 := MakeAdder(5); "
        "    return add5(3); "
        "  end Main; "
        "end C;"), 8);
}

BEZEL_TEST(codegen_cg60, closure_captures_local) {
    // Capture a local variable by value.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Apply(f: (int) -> int): int "
        "    return f(10); "
        "  end Apply; "
        "  public static method Main(): int "
        "    var offset := 7; "
        "    return Apply(lambda(x: int) => x + offset); "
        "  end Main; "
        "end C;"), 17);
}

BEZEL_TEST(codegen_cg60, closure_independent_captured_value) {
    // Two closures from the same factory share the captured value
    // independently (each closure has its own copy).
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method MakeMul(n: int): (int) -> int "
        "    return lambda(x: int) => x * n; "
        "  end MakeMul; "
        "  public static method Apply(f: (int) -> int, v: int): int "
        "    return f(v); "
        "  end Apply; "
        "  public static method Main(): int "
        "    var mul3 := MakeMul(3); "
        "    var mul5 := MakeMul(5); "
        "    return Apply(mul3, 4) + Apply(mul5, 4); "
        "  end Main; "
        "end C;"), 32);
}

BEZEL_TEST(codegen_cg60, closure_call_through_local) {
    // §A.7: a lambda stored in a local can be called via the local.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var n := 7; "
        "    var f := lambda(x: int) => x + n; "
        "    return f(3); "
        "  end Main; "
        "end C;"), 10);
}

BEZEL_TEST(codegen_cg60, closure_in_loop) {
    // Closures created inside a loop each capture the current value.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Apply(f: (int) -> int, v: int): int "
        "    return f(v); "
        "  end Apply; "
        "  public static method Main(): int "
        "    var k := 3; "
        "    var total := 0; "
        "    for var i in 1..3 do "
        "      var fi := lambda(x: int) => x * k; "
        "      total := total + fi(i); "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;"), 18);
}

// ── Method references ────────────────────────────────────────────────────────
// §17.14: A named method can be used as a function value by naming it
// without calling it. The compiler creates a closure struct wrapping a
// thunk function.

BEZEL_TEST(codegen_cg60, static_method_reference) {
    // Static method reference passed directly.
    BEZEL_EXPECT_EQ(run(
        "module M; class H "
        "  public static method Square(x: int): int => x * x; "
        "end H; "
        "class C "
        "  public static method Apply(f: (int) -> int, n: int): int "
        "    return f(n); "
        "  end Apply; "
        "  public static method Main(): int "
        "    return Apply(H.Square, 5); "
        "  end Main; "
        "end C;"), 25);
}

BEZEL_TEST(codegen_cg60, method_reference_stored_in_local) {
    // Method reference stored in a local, then called.
    BEZEL_EXPECT_EQ(run(
        "module M; class H "
        "  public static method Negate(x: int): int => -x; "
        "end H; "
        "class C "
        "  public static method Apply(f: (int) -> int, n: int): int "
        "    return f(n); "
        "  end Apply; "
        "  public static method Main(): int "
        "    var neg := H.Negate; "
        "    return Apply(neg, 7); "
        "  end Main; "
        "end C;"), -7);
}

// ── Block-body lambda with for loop and captured array ────────────────────
// The previous bug: block-body lambdas had funcReturn=null in their
// Function TypeRef, causing emitClosureCall to use void return type.
// This produced invalid IR (ud2 opcode) when the result was stored in
// a local variable.

BEZEL_TEST(codegen_cg60, block_body_result_in_local) {
    // Block-body lambda's return value stored in a `var result :=` local.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var f := lambda(n: int) => do "
        "      var r := n * 2; return r; "
        "    end; "
        "    var result := f(5); "
        "    return result; "
        "  end Main; end C;"), 10);
}

BEZEL_TEST(codegen_cg60, block_body_lambda_with_for_and_capture) {
    // Block-body lambda capturing an array and looping over it.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var f := lambda(threshold: int) => do "
        "      var c := 0; "
        "      for var i in 0..4 do "
        "        if i > threshold then c := c + 1; end; "
        "      end; "
        "      return c; "
        "    end; "
        "    var result := f(2); "
        "    return result; "
        "  end Main; end C;"), 2);
}

// ── Curried functions and nested lambdas ─────────────────────────────────
BEZEL_TEST(codegen_cg60, curried_add) {
    // `(a) => (b) => a + b` — outer returns inner closure.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var adder := lambda(x: int) => lambda(y: int) => x + y; "
        "    var add5 := adder(5); "
        "    return add5(3); "
        "  end Main; end C;"), 8);
}

BEZEL_TEST(codegen_cg60, array_element_call) {
    // Calling a closure stored in an inline array element: `handlers[i]()`.
    // Explicit initializer: a function type is not defaultable (§17.11,
    // v0.307), so an uninitialised `[3](int) -> int` is rejected (§15.1 /
    // §7.9, v0.307; #4419).
    BEZEL_EXPECT_EQ(runA(
        "module M; class C "
        "  public static method Main(): int "
        "    var fns: [3](int) -> int := [lambda(x: int) => 0, lambda(x: int) => 0, lambda(x: int) => 0]; "
        "    fns[0] := lambda(x: int) => x + 1; "
        "    fns[1] := lambda(x: int) => x * 2; "
        "    fns[2] := lambda(x: int) => x - 1; "
        "    return fns[0](3) + fns[1](4) + fns[2](5); "
        "  end Main; end C;"), 16);
}

// ── Advanced closure patterns ─────────────────────────────────────────────
BEZEL_TEST(codegen_cg60, once_pattern) {
    // A `once` function: call compute only on first invocation.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cache public _v: int := 0; public _done: bool := false; destructor end destructor; end Cache; "
        "class Acc public _c: int := 0; destructor end destructor; end Acc; "
        "class C "
        "  public static method Once(f: () -> int, ca: Cache): () -> int "
        "    return lambda() => do "
        "      if ca._done then return ca._v; end; "
        "      ca._v := f(); ca._done := true; return ca._v; "
        "    end; "
        "  end Once; "
        "  public static method Main(): int "
        "    using var ca := new Cache(); "
        "    using var acc := new Acc(); "
        "    var f := Once(lambda() => do acc._c := acc._c + 1; return 99; end, ca); "
        "    var r1 := f(); var r2 := f(); var r3 := f(); "
        "    return r1 + r2 + r3 - acc._c; "
        "  end Main; "
        "end C;"), 296);  // 99+99+99-1=296
}

BEZEL_TEST(codegen_cg60, partial_application) {
    // Partial application: bind first arg of (int,int)->int.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Partial(f: (int, int) -> int, a: int): (int) -> int "
        "    return lambda(b: int) => f(a, b); "
        "  end Partial; "
        "  public static method Main(): int "
        "    var add := lambda(x: int, y: int) => x + y; "
        "    var add10 := Partial(add, 10); "
        "    return add10(5) + add10(3); "
        "  end Main; "
        "end C;"), 28);  // 15 + 13 = 28
}

// ── Function-typed field calls ────────────────────────────────────────────
// Previously `obj._callback()` where `_callback: () -> int` silently
// dropped the call. Now dispatches through emitClosureCall.

BEZEL_TEST(codegen_cg60, function_typed_field_call) {
    // Class with a function-typed field; calling it should invoke the closure.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Acc public _v: int := 0; destructor end destructor; end Acc; "
        "class T "
        "  public _cb: (int) -> int; "
        "  public constructor(f: (int) -> int) _cb := f; end constructor; "
        "  destructor end destructor; "
        "end T; "
        "class C public static method Main(): int "
        "  using var acc := new Acc(); "
        "  using var t := new T(lambda(x: int) => do acc._v := acc._v + x; return acc._v; end); "
        "  var r1 := t._cb(10); "
        "  var r2 := t._cb(5); "
        "  return r1 + r2; "
        "end Main; end C;"), 25);
}

// fix #747 R43 / §9.1: calling a function value directly through a POSITIONAL
// record field — `box.fn()` where `record Box(fn: () -> int)`. The field's type
// is on its positional param, not a FieldDecl, so the closure-call path was
// skipped and the combined load+call returned garbage (extracting `b.fn` to a
// local first was the only working form). Now dispatches through emitClosureCall.
BEZEL_TEST(codegen_cg60, positional_record_fn_field_call_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Box(fn: () -> int); "
        "class C public static method Main(): int "
        "  var b := new Box(lambda() => 42); "
        "  return b.fn(); "
        "end Main; end C;"), 42);
}

// §9.1: the positional function field takes arguments.
BEZEL_TEST(codegen_cg60, positional_record_fn_field_with_args_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Box(fn: (int) -> int); "
        "class C public static method Main(): int "
        "  var b := new Box(lambda(x: int) => x * 7); "
        "  return b.fn(6); "  // 42
        "end Main; end C;"), 42);
}

// §9.1: the function field is NOT the first positional field — the field offset
// must be correct, not assumed to be slot 0.
BEZEL_TEST(codegen_cg60, positional_record_fn_field_second_slot_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pair(n: int, fn: (int) -> int); "
        "class C public static method Main(): int "
        "  var p := new Pair(10, lambda(x: int) => x + 5); "
        "  return p.fn(p.n); "  // 15
        "end Main; end C;"), 15);
}

// ── Function-typed array elements — return type propagation ───────────────
BEZEL_TEST(codegen_cg60, array_element_string_return) {
    // Verify that calling a `(int)->string` through an array element correctly
    // types the result as string (enabling .Length etc.).
    BEZEL_EXPECT_EQ(runA(
        "module M; class C "
        "  public static method Main(): int "
        "    var fns: [2](int) -> string := [lambda(n: int) => \"\", lambda(n: int) => \"\"]; "
        "    fns[0] := lambda(n: int) => (if n > 0 then \"abc\" else \"xy\"); "
        "    fns[1] := lambda(n: int) => \"hello\"; "
        "    var r0 := fns[0](5); "   // = "abc", length 3
        "    var r1 := fns[1](0); "   // = "hello", length 5
        "    return r0.Length + r1.Length; "  // 3+5=8
        "  end Main; end C;"), 8);
}

// ── §27.6 / issue #436: instance method references in value position ─────────
// A bare `obj.Method` (no parens) is a method reference — a `() -> R` value.
// The CORRECT forms below must still compile and produce the right result.

BEZEL_TEST(codegen_cg60, instance_method_ref_stored_then_called) {
    // §27.6: `var f: () -> int := a.GetVal; return f();` — store instance
    // method reference in a function-typed local, then call it.  This is the
    // canonical idiom and must produce the method's return value (42).
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class A "
        "  public _v: int := 0; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  destructor end destructor; "
        "  public method GetVal(): int => _v; "
        "end A; "
        "class C public static method Main(): int "
        "  using var a := new A(42); "
        "  var f: () -> int := a.GetVal; "
        "  return f(); "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_cg60, instance_method_ref_passed_as_arg) {
    // §27.6: pass `a.GetVal` where a `() -> int` parameter is expected.
    // The method reference must be bound to the receiver `a` and the
    // invocation inside Apply must return the receiver's value (7).
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class A "
        "  public _v: int := 0; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  destructor end destructor; "
        "  public method GetVal(): int => _v; "
        "end A; "
        "class C "
        "  public static method Apply(f: () -> int): int return f(); end Apply; "
        "  public static method Main(): int "
        "    using var a := new A(7); "
        "    return Apply(a.GetVal); "
        "  end Main; "
        "end C;"), 7);
}

// #719 / §31.2: a `throw` inside a lambda body propagates out of the lambda
// invocation instead of being silently swallowed. The closure-call path now
// runs emitPendingCheck after the call (like a normal method call).
BEZEL_TEST(codegen_cg60, lambda_body_throw_is_caught_719) {
    BEZEL_EXPECT_EQ(run(
        "module M; class AppErr: Exception end AppErr; "
        "class C public static method Main(): int "
        "  var fn: () -> int := lambda() => throw new AppErr(); "
        "  try "
        "    var v := fn(); "
        "    return v; "
        "  catch AppErr => return 55; "
        "  end; "
        "end Main; end C;"), 55);
}

// #719: a non-throwing lambda still returns its value normally (no regression).
BEZEL_TEST(codegen_cg60, lambda_body_no_throw_returns_value_719) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var fn: () -> int := lambda() => 42; "
        "  return fn(); "
        "end Main; end C;"), 42);
}

// #731 / §A.7: a nested lambda (a lambda returning a lambda) under an explicit
// higher-order function-type annotation. The inner lambda must see ITS expected
// return type (the outer's funcReturn), not the outer's full type. Previously
// IR-failed (ret i32 vs ptr). lowerLambda now descends expectedFunctionType_ to
// funcReturn before body emit. The curried application f(10)(5) = 15.
BEZEL_TEST(codegen_cg60, nested_higher_order_lambda_annotation_731) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var f: (int) -> (int) -> int := lambda(x: int) => lambda(y: int) => x + y; "
        "  var g := f(10); "
        "  return g(5); "  // 15
        "end Main; end C;"), 15);
}
