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
        "class V "
        "  public X: int := 0; "
        "  public static method operator <(a: V, b: V): bool => a.X < b.X; "
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
