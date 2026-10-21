// Spec-derived tests for §25 (Operator Overloading) — missing-operator
// diagnostic for binary operators on user class/record operands.
//
// §25: an operator overload is a *member of the operand's own type*. A
// `static method operator <op>` declared inside an *unrelated* class is
// not applicable for `a op b` — the operand types are searched, not
// arbitrary classes. When no applicable overload is found on either
// operand's type, the compiler must emit a clean diagnostic rather than
// falling through to primitive numeric codegen (which would emit
// arithmetic on a struct and produce malformed IR).
//
// Error cases:
//   (a) record operands where `operator +` is declared in an unrelated
//       class (not in the record itself) → clean compile error citing §25.
//   (b) class operands with NO `operator +` anywhere → clean compile error.
//
// Control cases (must NOT produce an error):
//   (c) a record with `operator +` declared in its *own* type → no error.
//   (d) primitive `int + int`, `string + string` → no error.
//   (e) enum `=` / record positional `=` → no error.
//   (f) class with `operator <` in its own type → no error.
//   (g) class with `operator *` (non-additive) in its own type → no error.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSema(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool anyMessageMentions(const bezel::test::SemaResult& r,
                        const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// (a) §25: operator declared in an *unrelated* class — not applicable.
// The record Rec has no `operator +` in its own type; Ops declares one but
// it is not a member of Rec. `a + b` where a, b: Rec must be a compile error.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_missing_op, record_plus_in_foreign_class_is_error_spec_25) {
    auto r = parseSema(
        "module M; "
        "record Rec(X: int); "
        "static class Ops "
        "  public static method operator +(a: Rec, b: Rec): Rec => new Rec(a.X + b.X); "
        "end Ops; "
        "class C public static method M(a: Rec, b: Rec): int "
        "  var c := a + b; return 0; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The diagnostic must mention the operator name ('+') to be actionable.
BEZEL_TEST(sema_missing_op, error_message_mentions_operator_spec_25) {
    auto r = parseSema(
        "module M; "
        "record Rec(X: int); "
        "static class Ops "
        "  public static method operator +(a: Rec, b: Rec): Rec => new Rec(a.X + b.X); "
        "end Ops; "
        "class C public static method M(a: Rec, b: Rec): int "
        "  var c := a + b; return 0; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "+"));
}

// The diagnostic must mention the operand type name so the programmer
// knows what declaration site §25 expects.
BEZEL_TEST(sema_missing_op, error_message_mentions_type_name_spec_25) {
    auto r = parseSema(
        "module M; "
        "record Rec(X: int); "
        "static class Ops "
        "  public static method operator +(a: Rec, b: Rec): Rec => new Rec(a.X + b.X); "
        "end Ops; "
        "class C public static method M(a: Rec, b: Rec): int "
        "  var c := a + b; return 0; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "Rec"));
}

// ---------------------------------------------------------------------------
// (b) §25: class operands with NO operator + anywhere → clean compile error.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_missing_op, class_plus_no_overload_anywhere_is_error_spec_25) {
    auto r = parseSema(
        "module M; "
        "class Point public X: int := 0; end Point; "
        "class C public static method M(a: Point, b: Point): int "
        "  var c := a + b; return 0; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Subtraction on class type with no overload → compile error.
BEZEL_TEST(sema_missing_op, class_minus_no_overload_is_error_spec_25) {
    auto r = parseSema(
        "module M; "
        "class Point public X: int := 0; end Point; "
        "class C public static method M(a: Point, b: Point): int "
        "  var c := a - b; return 0; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Multiplication on class type with no overload → compile error.
BEZEL_TEST(sema_missing_op, class_mul_no_overload_is_error_spec_25) {
    auto r = parseSema(
        "module M; "
        "class Point public X: int := 0; end Point; "
        "class C public static method M(a: Point, b: Point): int "
        "  var c := a * b; return 0; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Division on class type with no overload → compile error.
BEZEL_TEST(sema_missing_op, class_div_no_overload_is_error_spec_25) {
    auto r = parseSema(
        "module M; "
        "class Point public X: int := 0; end Point; "
        "class C public static method M(a: Point, b: Point): int "
        "  var c := a / b; return 0; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Record with no operator + anywhere → compile error.
BEZEL_TEST(sema_missing_op, record_plus_no_overload_anywhere_is_error_spec_25) {
    auto r = parseSema(
        "module M; "
        "record Vec(X: int, Y: int); "
        "class C public static method M(a: Vec, b: Vec): int "
        "  var c := a + b; return 0; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (c) Control: record with operator + in its OWN type → no error (§25).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_missing_op, record_owns_operator_plus_no_error_spec_25) {
    auto r = parseSema(
        "module M; "
        "record Vec(X: int) "
        "  public static method operator +(a: Vec, b: Vec): Vec => new Vec(a.X + b.X); "
        "end Vec; "
        "class C public static method M(a: Vec, b: Vec): Vec "
        "  return a + b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (c2) Control: class with operator + in its own type → no error (§25).
BEZEL_TEST(sema_missing_op, class_owns_operator_plus_no_error_spec_25) {
    auto r = parseSema(
        "module M; "
        "class V "
        "  public X: int := 0; "
        "  public static method operator +(a: V, b: V): int => a.X + b.X; "
        "end V; "
        "class C public static method M(a: V, b: V): int "
        "  return a + b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (f) Control: class with operator < in its own type → no error.
BEZEL_TEST(sema_missing_op, class_owns_operator_lt_no_error_spec_25) {
    auto r = parseSema(
        "module M; "
        // §25.2: comparison operators are declared as a complete group.
        "class V "
        "  public X: int := 0; "
        "  public static method operator <(a: V, b: V): bool => a.X < b.X; "
        "  public static method operator >(a: V, b: V): bool => a.X > b.X; "
        "  public static method operator <=(a: V, b: V): bool => a.X <= b.X; "
        "  public static method operator >=(a: V, b: V): bool => a.X >= b.X; "
        "end V; "
        "class C public static method M(a: V, b: V): bool "
        "  return a < b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (g) Control: class with operator * in its own type → no error.
BEZEL_TEST(sema_missing_op, class_owns_operator_mul_no_error_spec_25) {
    auto r = parseSema(
        "module M; "
        "class V "
        "  public X: int := 0; "
        "  public static method operator *(a: V, b: V): int => a.X * b.X; "
        "end V; "
        "class C public static method M(a: V, b: V): int "
        "  return a * b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (d) Control: primitive int + int, string + string → no error.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_missing_op, primitive_int_plus_int_no_error) {
    auto r = parseSema(
        "module M; class C public static method M(a: int, b: int): int "
        "  return a + b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_missing_op, primitive_string_plus_string_no_error) {
    auto r = parseSema(
        "module M; class C public static method M(a: string, b: string): string "
        "  return a + b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_missing_op, primitive_int_mul_int_no_error) {
    auto r = parseSema(
        "module M; class C public static method M(a: int, b: int): int "
        "  return a * b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (e) Control: enum = and positional record = → no error.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_missing_op, enum_equality_no_error) {
    auto r = parseSema(
        "module M; "
        "enum Color Red, Green; end Color; "
        "class C public static method M(a: Color, b: Color): bool "
        "  return a = b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_missing_op, positional_record_equality_no_error) {
    auto r = parseSema(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C public static method M(a: Point, b: Point): bool "
        "  return a = b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// #738 / §6.5: bitwise `&`/`|`/`^`/`<<`/`>>` carry no meaning on FLOATING-POINT
// operands (previously produced garbage). Rejected.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_missing_op, bitwise_on_float_rejected_spec_6_5_738) {
    auto r = parseSema(
        "module M; class C public static method M(): int "
        "  var a: float := 3.0f; var b: float := 7.0f; "
        "  var c: float := a & b; return 0; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
BEZEL_TEST(sema_missing_op, bitwise_on_double_rejected_spec_6_5_738) {
    auto r = parseSema(
        "module M; class C public static method M(): int "
        "  var a: double := 3.0; var b: double := 7.0; "
        "  var c: double := a | b; return 0; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// #738 / §6.1: `mod` is not defined on float or double.
BEZEL_TEST(sema_missing_op, mod_on_double_rejected_spec_6_1_738) {
    auto r = parseSema(
        "module M; class C public static method M(): int "
        "  var x: double := 5.0; var y: double := 2.0; "
        "  var z: double := x mod y; return 0; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// CONTROL: integer `&`, integer `mod`, and `decimal mod` are valid and must NOT
// be rejected by the §6.5 bitwise / §6.1 mod guard. (bool/char bitwise is now a
// §6.5 error per the v0.143 §7.11 clarification — see lifted_operators_486.)
BEZEL_TEST(sema_missing_op, integer_bitwise_and_mod_decimal_ok) {
    auto r = parseSema(
        "module M; class C public static method M(): int "
        "  var ia: int := 6; var ic := ia & 3; var im := ia mod 4; "
        "  var da: decimal := 5m; var dm := da mod 2m; "
        "  return 0; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §25.2 (#746 R41-C): a relational operator on a record/class with no matching
// overload is rejected cleanly (was a struct-level icmp → malformed IR).
// ---------------------------------------------------------------------------

// `<=` used on a record that declares only `<` and `>` — §25.2 error.
BEZEL_TEST(sema_missing_op, record_relational_without_overload_25_2) {
    auto r = parseSema(
        "module P; record Val(N: int) "
        "  public static method operator <(a: Val, b: Val): bool => a.N < b.N; "
        "  public static method operator >(a: Val, b: Val): bool => a.N > b.N; "
        "end Val; "
        "class M public static method Main(): int "
        "  var v1 := new Val(5); var v2 := new Val(10); "
        "  if v1 <= v2 then return 1; else return 2; end; "
        "end Main; end M;");
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "25.2"));
}

// CONTROL: a declared relational overload (`<`) IS usable — no error.
BEZEL_TEST(sema_missing_op, record_declared_relational_ok_25_2) {
    auto r = parseSema(
        // §25.2: comparison operators are declared as a complete group.
        "module P; record Val(N: int) "
        "  public static method operator <(a: Val, b: Val): bool => a.N < b.N; "
        "  public static method operator >(a: Val, b: Val): bool => a.N > b.N; "
        "  public static method operator <=(a: Val, b: Val): bool => a.N <= b.N; "
        "  public static method operator >=(a: Val, b: Val): bool => a.N >= b.N; "
        "end Val; "
        "class M public static method Main(): int "
        "  var v1 := new Val(5); var v2 := new Val(10); "
        "  if v1 < v2 then return 0; else return 1; end; "
        "end Main; end M;");
    BEZEL_EXPECT_FALSE(anyMessageMentions(r, "25.2"));
}

// CONTROL: a primitive `<=` is the built-in relational — never rejected.
BEZEL_TEST(sema_missing_op, primitive_relational_ok_25_2) {
    auto r = parseSema(
        "module M; class C public static method Main(): int "
        "  var a := 5; var b := 10; "
        "  if a <= b then return 0; end; return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(anyMessageMentions(r, "25.2"));
}

// ---------------------------------------------------------------------------
// #1204 / §6.2 / §5.1: a relational/equality comparison between two TYPED
// primitives with no implicit conversion between them (signed ↔ unsigned same
// width, float vs decimal) must be rejected.  A typed `int` variable vs a
// typed `uint` variable has no common type (§5.1); the programmer must cast.
// An UNTYPED integer literal or a literal-derived expression is allowed (it
// "adopts" the other operand's type), so `uint > 1000` must not be rejected.
// ---------------------------------------------------------------------------

// Reject: typed int var `<` typed uint var — no common type.
BEZEL_TEST(typed_signed_unsigned_compare_rejected_1204,
           int_var_lt_uint_var_no_common_type) {
    auto r = parseSema(
        "module M; class C public static method Main(): int "
        "  var a: int := 0 - 1; var b: uint := 1; "
        "  return (if a < b then 1 else 0); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Reject: typed uint var `>` typed int var (operand order reversed).
BEZEL_TEST(typed_signed_unsigned_compare_rejected_1204,
           uint_var_gt_int_var_no_common_type) {
    auto r = parseSema(
        "module M; class C public static method Main(): int "
        "  var a: uint := 1; var b: int := 0 - 1; "
        "  return (if a > b then 1 else 0); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Reject: typed long var `=` typed ulong var.
BEZEL_TEST(typed_signed_unsigned_compare_rejected_1204,
           long_var_eq_ulong_var_no_common_type) {
    auto r = parseSema(
        "module M; class C public static method Main(): int "
        "  var a: long := 5; var b: ulong := 5; "
        "  return (if a = b then 1 else 0); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Reject: typed double var `<` typed decimal var (IR_FAIL case).
BEZEL_TEST(float_decimal_compare_rejected_1204,
           double_var_lt_decimal_var_no_common_type) {
    auto r = parseSema(
        "module M; class C public static method Main(): int "
        "  var x: double := 1.5; var y: decimal := 2.0m; "
        "  return (if x < y then 1 else 0); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Guard: typed uint var `>` an UNTYPED integer literal must NOT be rejected —
// the literal adopts the uint type for the comparison (§5.1 / §6.2 / #1204).
// This was the regression introduced by the prior fix attempt.
BEZEL_TEST(uint_vs_literal_compare_still_ok_1204,
           uint_var_gt_int_literal_not_rejected) {
    auto r = parseSema(
        "module M; class C public static method Main(): int "
        "  var u: uint := 3000000000; "
        "  return (if u > 1000 then 1 else 0); end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Guard: typed ulong var `>` a LITERAL-DERIVED binary expression (e.g.
// `10 - 5`) must NOT be rejected — both sides of the nested expression are
// literals and the value 5 is in ulong's range.
BEZEL_TEST(uint_vs_literal_compare_still_ok_1204,
           ulong_var_gt_literal_derived_expr_not_rejected) {
    auto r = parseSema(
        "module M; class C public static method Main(): int "
        "  var u: ulong := 10; "
        "  return (if u > 10 - 5 then 1 else 0); end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// fix #6868 (§5.1, v0.340): BEHAVIOR FLIP of the row that used to pin
// `ulong > 0 - 5` as accepted. The literal-derived expression's value is -5;
// §5.1: "A constant whose value does not fit the target's range is a compile
// error at the conversion site -- never a silent truncation" (the old accept
// compared against 18446744073709551611).
BEZEL_TEST(uint_vs_literal_compare_still_ok_1204,
           ulong_var_gt_negative_literal_derived_expr_rejected_5_1) {
    auto r = parseSema(
        "module M; class C public static method Main(): int "
        "  var u: ulong := 10; "
        "  return (if u > 0 - 5 then 1 else 0); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Guard: same-type comparison (int vs int) must remain valid.
BEZEL_TEST(uint_vs_literal_compare_still_ok_1204,
           same_type_compare_not_rejected) {
    auto r = parseSema(
        "module M; class C public static method Main(): int "
        "  var a: int := 5; var b: int := 10; "
        "  return (if a < b then 1 else 0); end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Guard: int `<` long (widening per §5.1) must remain valid.
BEZEL_TEST(uint_vs_literal_compare_still_ok_1204,
           int_lt_long_widening_not_rejected) {
    auto r = parseSema(
        "module M; class C public static method Main(): int "
        "  var a: int := 5; var b: long := 10; "
        "  return (if a < b then 1 else 0); end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §25 / §14.1 (#1270): a binary user-operator on a GENERIC type
// (`operator +(a,b: Box<T>)`) matched its operands by DECLARATION only, so a
// MISMATCHED instantiation (`Box<int> + Box<bool>`) was wrongly accepted — the
// single type parameter `T` cannot unify to both `int` and `bool`. The operator
// must not apply; the use is rejected.
BEZEL_TEST(generic_operator_mismatched_instantiation_1270,
           box_int_plus_box_bool_rejected) {
    auto r = parseSema(
        "module M; record Box<T>(value: T) "
        "  public static method operator +(a: Box<T>, b: Box<T>): Box<T> => "
        "    new Box<T>(a.value); end Box; "
        "class C public static method Main(): int "
        "  var a := new Box<int>(3); var b := new Box<bool>(true); "
        "  var r := a + b; return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Control: the MATCHING instantiation (`Box<int> + Box<int>`) still resolves —
// `T` unifies to `int` on both operands. Must NOT be rejected.
BEZEL_TEST(generic_operator_mismatched_instantiation_1270,
           box_int_plus_box_int_not_rejected) {
    auto r = parseSema(
        "module M; record Box<T>(value: T) "
        "  public static method operator +(a: Box<T>, b: Box<T>): Box<T> => "
        "    new Box<T>(a.value); end Box; "
        "class C public static method Main(): int "
        "  var a := new Box<int>(3); var b := new Box<int>(4); "
        "  var r := a + b; return r.value; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §25 (#1347, v0.160): operators must be declared `public`.
// Non-public (internal, private, protected) operator declarations are errors.
// ---------------------------------------------------------------------------

// §25 (#1347): `internal` operator → error citing §25.
BEZEL_TEST(operator_visibility_1347, internal_operator_rejected_spec_25) {
    auto r = parseSema(
        "module M; record Vec(X: int) "
        "  internal static method operator +(a: Vec, b: Vec): Vec => "
        "    new Vec(a.X + b.X); "
        "end Vec; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "25"));
}

// §25 (#1347): `private` operator → error citing §25.
BEZEL_TEST(operator_visibility_1347, private_operator_rejected_spec_25) {
    auto r = parseSema(
        "module M; record Vec(X: int) "
        "  private static method operator +(a: Vec, b: Vec): Vec => "
        "    new Vec(a.X + b.X); "
        "end Vec; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "25"));
}

// §25 (#1347) CONTROL: `public` operator → no visibility error.
BEZEL_TEST(operator_visibility_1347, public_operator_unaffected_spec_25) {
    auto r = parseSema(
        "module M; record Vec(X: int) "
        "  public static method operator +(a: Vec, b: Vec): Vec => "
        "    new Vec(a.X + b.X); "
        "end Vec; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §25.2 (#1347, v0.160): at least one parameter of an operator overload must
// be the declaring type.
// ---------------------------------------------------------------------------

// §25.2 (#1347): operator with no declaring-type parameter → error.
BEZEL_TEST(operator_declaring_type_1347, no_declaring_type_param_rejected_25_2) {
    auto r = parseSema(
        "module M; "
        "record A(X: int); "
        "record B(Y: int); "
        "record Ops(Z: int) "
        "  public static method operator +(a: A, b: B): int => a.X + b.Y; "
        "end Ops; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "25.2"));
}

// §25.2 (#1347) CONTROL: first param is declaring type → OK.
BEZEL_TEST(operator_declaring_type_1347, first_param_declaring_type_ok_25_2) {
    auto r = parseSema(
        "module M; record Vec(X: int) "
        "  public static method operator +(a: Vec, b: Vec): Vec => "
        "    new Vec(a.X + b.X); "
        "end Vec; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §25.2 (#1347) CONTROL: second param is declaring type (cross-type, like
// TimeSpan `operator +(duration, offset: TimeSpan)`) → OK.
BEZEL_TEST(operator_declaring_type_1347, second_param_declaring_type_ok_25_2) {
    auto r = parseSema(
        "module M; record Offset(N: int); "
        "record Duration(Ticks: int) "
        "  public static method operator +(a: Offset, b: Duration): Duration => "
        "    new Duration(a.N + b.Ticks); "
        "end Duration; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §25.2 (#1347, v0.160): ambiguity — distinct overloads on both operand types
// → compile error citing candidates.
// ---------------------------------------------------------------------------

// §25.2 (#1347): `a + b` where A and B each declare an applicable operator
// `+` → ambiguity error.
BEZEL_TEST(operator_ambiguity_1347, both_operand_types_have_operator_error_25_2) {
    auto r = parseSema(
        "module M; "
        "record A(X: int) "
        "  public static method operator +(a: A, b: B): int => a.X + b.Y; "
        "end A; "
        "record B(Y: int) "
        "  public static method operator +(a: A, b: B): int => a.X + b.Y; "
        "end B; "
        "class C public static method Main(): int "
        "  var a := new A(1); var b := new B(2); "
        "  var r := a + b; return r; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The error should mention ambiguity.
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "ambiguous"));
}

// §25.2 (#1347) CONTROL: same-type operands — both sides find the SAME
// operator symbol → NOT ambiguous (fromLeft == fromRight).
BEZEL_TEST(operator_ambiguity_1347, same_type_operands_not_ambiguous_25_2) {
    auto r = parseSema(
        "module M; record V(X: int) "
        "  public static method operator +(a: V, b: V): V => "
        "    new V(a.X + b.X); "
        "end V; "
        "class C public static method Main(): int "
        "  var a := new V(1); var b := new V(2); "
        "  var r := a + b; return r.X; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §25.2 (#1347) CONTROL: only left operand's type has matching operator →
// unambiguous, no error.
BEZEL_TEST(operator_ambiguity_1347, only_left_type_has_operator_ok_25_2) {
    auto r = parseSema(
        "module M; "
        "record Scale(F: int); "
        "record V(X: int) "
        "  public static method operator *(a: V, b: Scale): V => "
        "    new V(a.X * b.F); "
        "end V; "
        "class C public static method Main(): int "
        "  var v := new V(3); var s := new Scale(2); "
        "  var r := v * s; return r.X; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §25.2 (#1347) CONTROL: no auto-commutation — `4 - v` does not match
// `operator -(v: V, n: int)`. The expression is rejected (no int operator -
// on V).
BEZEL_TEST(operator_ambiguity_1347, no_auto_commutation_literal_minus_v_spec_25_2) {
    auto r = parseSema(
        "module M; record V(X: int) "
        "  public static method operator -(a: V, n: int): V => new V(a.X - n); "
        "end V; "
        "class C public static method Main(): int "
        "  var v := new V(10); "
        "  var r := 4 - v; return 0; "  // `4 - v` ≠ operator-(v: V, n: int)
        "end M; end C;");
    // `4 - v`: left=int, right=V. V's `operator -` expects (V, int) but we
    // have (int, V) — parameters don't match in order. No commutation. Error.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §25.2 (#1347) CONTROL: cross-type operator (DateTime/TimeSpan style) — both
// operand types' union is the candidate set, and one type's operator matches.
BEZEL_TEST(operator_ambiguity_1347, cross_type_operator_works_spec_25_2) {
    auto r = parseSema(
        "module M; "
        "record Span(N: int); "
        "record Moment(T: int) "
        "  public static method operator +(m: Moment, s: Span): Moment => "
        "    new Moment(m.T + s.N); "
        "end Moment; "
        "class C public static method Main(): int "
        "  var m := new Moment(10); var s := new Span(5); "
        "  var r := m + s; return r.T; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
