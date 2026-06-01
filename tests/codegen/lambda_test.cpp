// Codegen slice CG60 — lambda expressions (§A.7, §17.14). Lambdas lower to
// nested LLVM functions and are passed as function-pointer values. The slice
// covers:
//   - expression-body lambdas `(p: T) => expr`
//   - explicit parameter type annotations (inferred params are later)
//   - passing lambdas as function-typed parameters `(T) -> R`
//   - storing lambdas in locals and passing them later
// Captures (closures over outer variables) are deferred.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
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
        "    return Apply(5, (x: int) => x * 2); "
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
        "    return Map(7, (x: int) => x + 3); "
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
        "    var sq := (a: int) => a * a; "
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
        "    return Combine(3, 4, (x: int, y: int) => x + y); "
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
        "    return Apply(6, (x: int) => x * 3); "
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
        "    var dbl := (x: int) => x * 2; "
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
        "    return (x: int) => x + n; "
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
        "    return Apply((x: int) => x + offset); "
        "  end Main; "
        "end C;"), 17);
}

BEZEL_TEST(codegen_cg60, closure_independent_captured_value) {
    // Two closures from the same factory share the captured value
    // independently (each closure has its own copy).
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method MakeMul(n: int): (int) -> int "
        "    return (x: int) => x * n; "
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
        "    var f := (x: int) => x + n; "
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
        "      var fi := (x: int) => x * k; "
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
        "    var f := (n: int) => do "
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
        "    var f := (threshold: int) => do "
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
        "    var adder := (x: int) => (y: int) => x + y; "
        "    var add5 := adder(5); "
        "    return add5(3); "
        "  end Main; end C;"), 8);
}

BEZEL_TEST(codegen_cg60, array_element_call) {
    // Calling a closure stored in an inline array element: `handlers[i]()`.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var fns: [3](int) -> int; "
        "    fns[0] := (x: int) => x + 1; "
        "    fns[1] := (x: int) => x * 2; "
        "    fns[2] := (x: int) => x - 1; "
        "    return fns[0](3) + fns[1](4) + fns[2](5); "
        "  end Main; end C;"), 16);
}
