// Codegen slice CG50 — `ref` / `out` parameters (§22.3). A ref/out parameter
// is passed by reference: its LLVM type is a pointer to the value type, the
// caller passes the address of the argument lvalue, and reads/writes inside
// the callee go through that pointer — so mutations are visible to the
// caller. Observable via the int exit code. (`out var x` declaring forms
// are deferred.)

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

BEZEL_TEST(codegen_cg50, ref_param_mutation_visible) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Inc(ref x: int) x := x + 1; end Inc; "
        "  public static method Main(): int "
        "    var n := 5; Inc(ref n); return n; "
        "  end Main; "
        "end C;"), 6);
}

BEZEL_TEST(codegen_cg50, out_param_writes_back) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Get(out x: int) x := 42; end Get; "
        "  public static method Main(): int "
        "    var n := 0; Get(out n); return n; "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg50, two_ref_params_swap) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Swap(ref a: int, ref b: int) "
        "    var t := a; a := b; b := t; "
        "  end Swap; "
        "  public static method Main(): int "
        "    var x := 3; var y := 7; Swap(ref x, ref y); return x * 10 + y; "
        "  end Main; "
        "end C;"), 73);
}

BEZEL_TEST(codegen_cg50, ref_reads_and_writes) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Double(ref x: int) x := x * 2; end Double; "
        "  public static method Main(): int "
        "    var n := 21; Double(ref n); return n; "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg50, ref_to_a_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; class N public V: int := 0; end N; "
        "class C "
        "  public static method Inc(ref x: int) x := x + 1; end Inc; "
        "  public static method Main(): int "
        "    var n := new N(); n.V := 41; Inc(ref n.V); return n.V; "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg50, by_value_param_unchanged) {
    // A by-value parameter still receives a copy; mutating it does not
    // affect the caller's variable.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Bump(x: int) x := x + 100; end Bump; "
        "  public static method Main(): int "
        "    var n := 5; Bump(n); return n; "
        "  end Main; "
        "end C;"), 5);
}

// fix #747 R43 / §27.1: a `ref`/`out` parameter on a LAMBDA (function value)
// must also pass by reference — the lambda's mutation writes back to the
// caller's variable. Previously a ref lambda parameter was lowered by value, so
// the write-back was silently lost (the value semantics worked for named
// methods but not lambdas). §27.1: `(ref int) -> ()` is a valid function type
// and is equated with `method Foo(ref n: int)`.

// Stored function value with a ref param mutates the caller's variable.
BEZEL_TEST(codegen_cg50, ref_lambda_param_writes_back_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var modify: (ref int) -> () := (ref n: int) => do n := n * 2; end; "
        "  var x := 21; "
        "  modify(ref x); "
        "  return x; "  // 42
        "end Main; end C;"), 42);
}

// The ref lambda READS through the pointer too (n := n + n applied twice).
BEZEL_TEST(codegen_cg50, ref_lambda_param_reads_and_writes_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var dbl: (ref int) -> () := (ref n: int) => do n := n + n; end; "
        "  var x := 15; "
        "  dbl(ref x); dbl(ref x); "
        "  return x; "  // 15 → 30 → 60
        "end Main; end C;"), 60);
}

// An `out` lambda parameter writes the caller's (pre-declared) variable.
BEZEL_TEST(codegen_cg50, out_lambda_param_writes_back_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var init: (out int) -> () := (out n: int) => do n := 99; end; "
        "  var x: int; "
        "  init(out x); "
        "  return x; "  // 99
        "end Main; end C;"), 99);
}

// Higher-order: a method takes a `(ref int) -> ()` and a ref lvalue, and
// invokes the function value through its own ref parameter.
BEZEL_TEST(codegen_cg50, ref_lambda_passed_to_higher_order_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Apply(f: (ref int) -> (), ref v: int) "
        "    f(ref v); "
        "  end Apply; "
        "  public static method Main(): int "
        "    var x := 20; "
        "    C.Apply((ref n: int) => do n := n + 5; end, ref x); "
        "    return x; "  // 25
        "  end Main; "
        "end C;"), 25);
}

// CONTROL: a NON-ref lambda parameter still passes by value — mutating it does
// not affect the caller (the by-ref override must be gated on the modifier).
BEZEL_TEST(codegen_cg50, value_lambda_param_unchanged_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var bump: (int) -> () := (n: int) => do n := n + 100; end; "
        "  var x := 7; "
        "  bump(x); "
        "  return x; "  // 7 — unchanged
        "end Main; end C;"), 7);
}
