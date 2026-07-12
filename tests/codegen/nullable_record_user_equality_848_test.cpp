// fix #848 / §8.4: a record that overloads `operator =`/`<>`, compared through a
// nullable wrapper (`T?`) or against the `null` literal, emitted malformed IR —
// codegen passed the `{i8, T}` nullable struct (or `ptr null`) straight to the
// user operator, which takes a bare `T`. Per §8.4, `=`/`<>` is null-safe even for
// a user-overloaded record: `null = null` → true, `null = nonNull` → false, and
// both-present invokes the operator on the unwrapped payloads. The fix wraps the
// operator call in that null-safe protocol, branch-guarding the call so it never
// runs on a null/zero payload (a record with a reference-typed field would
// otherwise dereference a null field).
//
// fix #1248 / §25.2 / §8.4: when BOTH operands are nullable (`T? = T?`), the
// custom `operator =`/`<>` declared on the bare record type `T` was not dispatched
// — sema's operandMatchesParam rejected a `T?` operand against a non-nullable `T`
// parameter, so findBinaryOperator returned null and codegen fell through to
// structural equality.  For `=`/`<>` the nullable gate is now exempted (the
// null-safe equality wrapper unwraps T? → T before the call), so the user operator
// is correctly resolved and invoked for any combination of nullable / non-nullable
// T? / T operands.
//
// Value tests — runJit returns the raw int (kept in 0..255).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
// Point overloading `=`/`<>` (§25.2 requires both together, and — v0.158 — a
// consistent `Equals(T)` + `override GetHashCode`; the co-declared members match
// the operator's field-by-field semantics).
const char* kPoint =
    "module M; "
    "record Point(X: int, Y: int) "
    "  public static method operator =(a: Point, b: Point): bool => "
    "    a.X = b.X and a.Y = b.Y; "
    "  public static method operator <>(a: Point, b: Point): bool => "
    "    a.X <> b.X or a.Y <> b.Y; "
    "  public method Equals(other: Point): bool => X = other.X and Y = other.Y; "
    "  public override method GetHashCode(): int => X * 31 + Y; "
    "end Point; ";
// #1248: Pt overloads `=`/`<>` comparing ONLY X (ignoring Y) so tests can
// distinguish the custom operator from structural field-by-field equality: two
// Pt values with the same X but different Y compare equal by the custom operator
// but NOT equal structurally.  Both operators are declared per §25.2; the v0.158
// co-declared `Equals(T)`/`GetHashCode` keep the X-only semantics consistent.
const char* kPt =
    "module M; "
    "record Pt(X: int, Y: int) "
    "  public static method operator =(a: Pt, b: Pt): bool => a.X = b.X; "
    "  public static method operator <>(a: Pt, b: Pt): bool => a.X <> b.X; "
    "  public method Equals(other: Pt): bool => X = other.X; "
    "  public override method GetHashCode(): int => X; "
    "end Pt; ";
}  // namespace

// bug06: `Point? = Point?` — present-vs-null is false, null-vs-null is true (was
// an IR call-param mismatch).
BEZEL_TEST(nullable_record_user_equality_848, nullable_both_sides) {
    BEZEL_EXPECT_EQ(run(std::string(kPoint) +
        "class C public static method Main(): int "
        "  var a: Point? := new Point(1, 2); "
        "  var b: Point? := null; "
        "  var r1: bool := a = b; "       // present vs null → false
        "  var r2: bool := b = b; "       // null vs null → true
        "  if r1 then return 10; end; "
        "  if r2 then return 2; end; "
        "  return 0; "
        "end Main; end C;"), 2);
}

// bug07: comparing a nullable record to the `null` literal uses null-safe
// equality (never the user operator): p=null → false, q=null → true, p<>null →
// true (was an IR mismatch with `ptr null`).
BEZEL_TEST(nullable_record_user_equality_848, compare_to_null_literal) {
    BEZEL_EXPECT_EQ(run(std::string(kPoint) +
        "class C public static method Main(): int "
        "  var p: Point? := new Point(1, 2); "
        "  var q: Point? := null; "
        "  var r1: bool := p = null; "    // false
        "  var r2: bool := q = null; "    // true
        "  var r3: bool := p <> null; "   // true
        "  if not r1 and r2 and r3 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// Both present → the user operator IS invoked on the unwrapped payloads
// (equal payloads → equal; differing payloads → `<>` true).
BEZEL_TEST(nullable_record_user_equality_848, both_present_invokes_operator) {
    BEZEL_EXPECT_EQ(run(std::string(kPoint) +
        "class C public static method Main(): int "
        "  var a: Point? := new Point(5, 5); "
        "  var b: Point? := new Point(5, 5); "
        "  var c: Point? := new Point(9, 9); "
        "  if a = b and a <> c then return 8; end; "
        "  return 0; "
        "end Main; end C;"), 8);
}

// A mixed comparison (non-nullable `Point` vs `Point?`) lifts the non-nullable
// operand as present.
BEZEL_TEST(nullable_record_user_equality_848, mixed_nullable_nonnullable) {
    BEZEL_EXPECT_EQ(run(std::string(kPoint) +
        "class C public static method Main(): int "
        "  var a := new Point(3, 3); "
        "  var b: Point? := new Point(3, 3); "
        "  var c: Point? := null; "
        "  if a = b then return (6 if not (a = c) else 0); end; "
        "  return 0; "
        "end Main; end C;"), 6);
}

// Safety: a record with a reference-typed (string) field overloading `=`/`<>`,
// compared to null, must not dereference the null payload's string field — the
// operator is branch-guarded to the both-present path.
BEZEL_TEST(nullable_record_user_equality_848, reference_field_record_vs_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Named(Name: string, Id: int) "
        "  public static method operator =(a: Named, b: Named): bool => a.Id = b.Id; "
        "  public static method operator <>(a: Named, b: Named): bool => a.Id <> b.Id; "
        "  public method Equals(other: Named): bool => Id = other.Id; "
        "  public override method GetHashCode(): int => Id; "
        "end Named; "
        "class C public static method Main(): int "
        "  var a: Named? := new Named(\"alice\", 1); "
        "  var b: Named? := null; "
        "  if a = b then return 20; end; "        // false (present vs null)
        "  if not (b = b) then return 21; end; "  // b=b is true (null vs null)
        "  return 5; "
        "end Main; end C;"), 5);
}

// ── fix #1248 / §25.2 / §8.4: T? = T? must dispatch the user operator ─────────
//
// Core regression: two Pt? values with the same X but different Y must compare
// equal via the custom X-only `operator =` (not structural equality which would
// see different Y and return false).
BEZEL_TEST(nullable_record_user_equality_1248, both_nullable_eq_custom_op_dispatched) {
    // §25.2 / §8.4 (#1248): a = Pt(1,99), b = Pt(1,42) — same X, different Y.
    // Custom operator = compares X only → equal (true). Structural equality would
    // compare both fields → not equal (false). Expected rc=1.
    BEZEL_EXPECT_EQ(run(std::string(kPt) +
        "class C public static method Main(): int "
        "  var a: Pt? := new Pt(1, 99); "
        "  var b: Pt? := new Pt(1, 42); "
        "  return (1 if a = b else 0); "
        "end Main; end C;"), 1);
}

// Custom `operator <>` dispatched for T? <> T?: same X → not different → false.
// Structural <> would see different Y → true. So the discriminating value is 0.
BEZEL_TEST(nullable_record_user_equality_1248, both_nullable_neq_custom_op_dispatched) {
    // §25.2 / §8.4 (#1248): custom <> compares X only; same X=1 → false.
    // Structural would see different Y → true. Expected rc=0 (operator returns false).
    BEZEL_EXPECT_EQ(run(std::string(kPt) +
        "class C public static method Main(): int "
        "  var a: Pt? := new Pt(1, 99); "
        "  var b: Pt? := new Pt(1, 42); "
        "  return (1 if a <> b else 0); "
        "end Main; end C;"), 0);
}

// Different X → custom = gives false, custom <> gives true.
BEZEL_TEST(nullable_record_user_equality_1248, both_nullable_different_x) {
    // §25.2 / §8.4 (#1248): a.X=1, b.X=2 → a = b is false; a <> b is true.
    BEZEL_EXPECT_EQ(run(std::string(kPt) +
        "class C public static method Main(): int "
        "  var a: Pt? := new Pt(1, 99); "
        "  var b: Pt? := new Pt(2, 99); "
        "  if not (a = b) and (a <> b) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// Null-operand cases: null = non-null → false; null = null → true;
// null <> non-null → true.  The user operator is never invoked on a null side.
BEZEL_TEST(nullable_record_user_equality_1248, null_operand_handling) {
    // §8.4 (#1248): null-safe: (n = a) is false, (n = null) is true, (n <> a) is 1.
    BEZEL_EXPECT_EQ(run(std::string(kPt) +
        "class C public static method Main(): int "
        "  var n: Pt? := null; "
        "  var a: Pt? := new Pt(1, 99); "
        "  if n = a then return 10; end; "   // false: null vs non-null
        "  if not (n = null) then return 11; end; "  // true: null vs null
        "  if not (n <> a) then return 12; end; "    // true: null vs non-null
        "  return 1; "
        "end Main; end C;"), 1);
}

// Regression: a record WITHOUT a custom operator still uses structural equality
// for nullable comparison (not broken by the fix).
BEZEL_TEST(nullable_record_user_equality_1248, structural_record_nullable_regression) {
    // §9.1 / §8.4 (#1248 regression): no custom operator → structural equality.
    // Two Pt? with identical fields → equal; differing field → not equal.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pair(A: int, B: int) end Pair; "
        "class C public static method Main(): int "
        "  var p: Pair? := new Pair(3, 7); "
        "  var q: Pair? := new Pair(3, 7); "
        "  var r: Pair? := new Pair(3, 8); "
        "  if p = q and not (p = r) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// Regression: the non-nullable `Pt = Pt` custom-operator path still works.
BEZEL_TEST(nullable_record_user_equality_1248, nonnullable_regression) {
    // §25.2 (#1248 regression): non-nullable T = T must still invoke the operator.
    // Same X different Y → equal via custom op; different X → not equal.
    BEZEL_EXPECT_EQ(run(std::string(kPt) +
        "class C public static method Main(): int "
        "  var a := new Pt(1, 99); "
        "  var b := new Pt(1, 42); "
        "  var c := new Pt(2, 42); "
        "  if a = b and not (a = c) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}
