// fix #829 / §A.7 / §25.2: when a lambda's parameter types are INFERRED from
// its expected function type (`var f: (Val, Val) -> Val := (a, b) => a + b`),
// the params were left untyped, so the body's operator resolution fell back to
// primitives — a user-defined `operator +` / `operator <` on a record was
// missed and codegen emitted a raw `add` / `icmp` on the record struct
// (malformed IR / compiler abort). The inferred parameter type is now threaded
// from the expected function type (typed-local init, assignment RHS, and a
// function-typed call argument) so the body sees the real type.
//
// Value tests — runJit returns the raw int (kept in 0..255).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
// A record with a user-defined `operator +`.
const char* kAdd =
    "module M; record Val(N: int) "
    "  public static method operator +(a: Val, b: Val): Val => "
    "    new Val(a.N + b.N); "
    "end Val; class C ";
// A record with the full §25.2 comparison-operator group.
const char* kCmp =
    "module M; record Val(N: int) "
    "  public static method operator <(a: Val, b: Val): bool => a.N < b.N; "
    "  public static method operator >(a: Val, b: Val): bool => a.N > b.N; "
    "  public static method operator <=(a: Val, b: Val): bool => a.N <= b.N; "
    "  public static method operator >=(a: Val, b: Val): bool => a.N >= b.N; "
    "end Val; class C ";
}  // namespace

// Typed-local initializer: the untyped params a,b are inferred as Val, so a + b
// resolves the user `operator +` (was a raw `add` on %record.Val → malformed IR).
BEZEL_TEST(lambda_inferred_param_operator_829, vardecl_record_plus) {
    BEZEL_EXPECT_EQ(run(std::string(kAdd) +
        "public static method Main(): int "
        "  var f: (Val, Val) -> Val := (a, b) => a + b; "
        "  var r := f(new Val(10), new Val(5)); return r.N; "
        "end Main; end C;"), 15);
}

// Typed-local initializer, comparison operator (was a raw `icmp` on a struct →
// compiler abort).
BEZEL_TEST(lambda_inferred_param_operator_829, vardecl_record_less_than) {
    BEZEL_EXPECT_EQ(run(std::string(kCmp) +
        "public static method Main(): int "
        "  var f: (Val, Val) -> bool := (a, b) => a < b; "
        "  var r := f(new Val(3), new Val(5)); return (1 if r else 0); "
        "end Main; end C;"), 1);
}

// Assignment RHS: a lambda assigned to a function-typed local.
BEZEL_TEST(lambda_inferred_param_operator_829, assign_rhs_record_plus) {
    BEZEL_EXPECT_EQ(run(std::string(kAdd) +
        "public static method Main(): int "
        "  var f: (Val, Val) -> Val := (p, q) => new Val(0); "
        "  f := (a, b) => a + b; "
        "  var r := f(new Val(10), new Val(5)); return r.N; "
        "end Main; end C;"), 15);
}

// Call argument bound to a function-typed parameter: the inferred param types
// come from the callee's formal parameter type.
BEZEL_TEST(lambda_inferred_param_operator_829, call_arg_record_plus) {
    BEZEL_EXPECT_EQ(run(std::string(kAdd) +
        "public static method Apply(g: (Val, Val) -> Val, a: Val, b: Val): Val "
        "  => g(a, b); "
        "public static method Main(): int "
        "  var r := Apply((a, b) => a + b, new Val(10), new Val(5)); "
        "  return r.N; "
        "end Main; end C;"), 15);
}

// A mix of an explicitly-typed and an inferred parameter.
BEZEL_TEST(lambda_inferred_param_operator_829, mixed_explicit_and_inferred) {
    BEZEL_EXPECT_EQ(run(std::string(kAdd) +
        "public static method Main(): int "
        "  var f: (Val, Val) -> Val := (a: Val, b) => a + b; "
        "  var r := f(new Val(10), new Val(5)); return r.N; "
        "end Main; end C;"), 15);
}

// Regression: a primitive-typed inferred lambda still uses the primitive op.
BEZEL_TEST(lambda_inferred_param_operator_829, primitive_inferred_regression) {
    BEZEL_EXPECT_EQ(run(std::string(kAdd) +
        "public static method Main(): int "
        "  var f: (int, int) -> int := (a, b) => a + b; return f(3, 4); "
        "end Main; end C;"), 7);
}

// Member access through an inferred param (resolves a.N over the record).
BEZEL_TEST(lambda_inferred_param_operator_829, member_access_inferred) {
    BEZEL_EXPECT_EQ(run(std::string(kAdd) +
        "public static method Main(): int "
        "  var f: (Val) -> int := (a) => a.N; return f(new Val(7)); "
        "end Main; end C;"), 7);
}

// ── #1124: Aggregate fold with an inferred `(acc, x)` parameter list ──────────
//
// stdlib §5 / §27.4: an `Aggregate(seed, (acc, x) => …)` fold lambda is
// (TAccum, T) -> TAccum and its parameter annotations may be omitted. Before the
// fix the two-param fold was not seeded (the single-param LINQ seeding does not
// apply), so the inferred params stayed untyped — the body failed to resolve and
// codegen emitted `ret void` against the accumulator return type (IR_FAIL). The
// fix seeds `acc` from the seed argument's type and `x` from the source element.

// Headline #1124 repro: int accumulator, a method call on the inferred element.
// 0 + len("1") + len("2") = 2.
BEZEL_TEST(lambda_inferred_param_operator_829, aggregate_inferred_int_acc_1124) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var xs := new Array<int>(); xs.Add(1); xs.Add(2); "
        "  return xs.Aggregate(0, (acc, x) => acc + x.ToString().Length); "
        "end Main; end C;"), 2);
}

// A STRING accumulator (TAccum != the element type T): `acc` must be lowered as
// a string, not the i32 element default. "" + "1" + "2" = "12", Length 2.
BEZEL_TEST(lambda_inferred_param_operator_829, aggregate_inferred_string_acc_1124) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var xs := new Array<int>(); xs.Add(1); xs.Add(2); "
        "  var s := xs.Aggregate(\"\", (acc, x) => acc + x.ToString()); "
        "  return s.Length; "
        "end Main; end C;"), 2);
}

// A plain numeric fold with a non-zero seed: 100 + 2 + 3 = 105.
BEZEL_TEST(lambda_inferred_param_operator_829, aggregate_inferred_nonzero_seed_1124) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var xs := new Array<int>(); xs.Add(2); xs.Add(3); "
        "  return xs.Aggregate(100, (acc, x) => acc + x); "
        "end Main; end C;"), 105);
}

// Explicit parameter annotations remain correct (regression guard).
BEZEL_TEST(lambda_inferred_param_operator_829, aggregate_explicit_params_1124) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var xs := new Array<int>(); xs.Add(5); xs.Add(10); "
        "  return xs.Aggregate(0, (acc: int, x: int) => acc + x); "
        "end Main; end C;"), 15);
}
