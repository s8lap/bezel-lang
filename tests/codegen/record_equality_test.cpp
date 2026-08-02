// Codegen slice CG29b — structural equality for records (§8.3, §17.3).
//
// §8.3 auto-generates memberwise = / <> for positional records.
// §17.3 notes records (unlike classes) can have = / <> operator overloads;
// the auto-generated form is the default when no overload is declared.
//
// Implementation note: a record value lowers to a first-class LLVM struct
// (not a pointer). A struct-typed operand on = / <> uniquely identifies the
// record case (no other Bezel value lowers to an LLVM struct), so we dispatch
// to emitStructEquality before the scalar integer/float paths.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §8.3 positional record: two identical values compare equal.
BEZEL_TEST(codegen_cg29b, equal_records_are_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new Point(1, 2); var b := new Point(1, 2); "
        "  return (if a = b then 1 else 0); "
        "end Main; end C;"), 1);
}

// Different values do not compare equal.
BEZEL_TEST(codegen_cg29b, different_records_are_not_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new Point(1, 2); var b := new Point(1, 3); "
        "  return (if a = b then 1 else 0); "
        "end Main; end C;"), 0);
}

// <> is the negation of =.
BEZEL_TEST(codegen_cg29b, neq_operator) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new Point(1, 2); var b := new Point(9, 9); "
        "  return (if a <> b then 1 else 0); "
        "end Main; end C;"), 1);
}

// Every field is checked — first field differs.
BEZEL_TEST(codegen_cg29b, first_field_mismatch) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new Point(7, 2); var b := new Point(1, 2); "
        "  return (if a = b then 1 else 0); "
        "end Main; end C;"), 0);
}

// Three-field record — equality must check all three.
BEZEL_TEST(codegen_cg29b, three_field_equality) {
    BEZEL_EXPECT_EQ(run(
        "module M; record T(a: int, b: int, c: int); "
        "class C public static method Main(): int "
        "  var p := new T(1, 2, 3); var q := new T(1, 2, 3); "
        "  return (if p = q then 1 else 0); "
        "end Main; end C;"), 1);
}

// Nested record: Outer(Inner(3,4), 5) = Outer(Inner(3,4), 5) is true.
BEZEL_TEST(codegen_cg29b, nested_record_equality) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Inner(a: int, b: int); "
        "record Outer(p: Inner, z: int); "
        "class C public static method Main(): int "
        "  var x := new Outer(new Inner(3, 4), 5); "
        "  var y := new Outer(new Inner(3, 4), 5); "
        "  return (if x = y then 1 else 0); "
        "end Main; end C;"), 1);
}

// Nested record: inequality detected on inner field.
BEZEL_TEST(codegen_cg29b, nested_record_inner_mismatch) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Inner(a: int, b: int); "
        "record Outer(p: Inner, z: int); "
        "class C public static method Main(): int "
        "  var x := new Outer(new Inner(3, 4), 5); "
        "  var y := new Outer(new Inner(3, 9), 5); "
        "  return (if x = y then 1 else 0); "
        "end Main; end C;"), 0);
}

// §9.2 / §4.4 / §4.3.1 (fix #1165): the auto-generated record `=` compares each
// field by its declared type's `=`. A double field must therefore use the
// SAME NaN-aware reflexive equality as a scalar double (§4.3.1: "NaN = NaN is
// true"), not raw IEEE oeq. Two records with equal NaN fields compare equal.
BEZEL_TEST(codegen_cg29b, double_field_nan_records_are_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record R(v: double); "
        "class C public static method Main(): int "
        "  var nan := 0.0 / 0.0; "
        "  var a := new R(nan); var b := new R(nan); "
        "  return (if a = b then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.2 / §4.4: ordinary equal double fields still compare equal.
BEZEL_TEST(codegen_cg29b, double_field_equal_records) {
    BEZEL_EXPECT_EQ(run(
        "module M; record R(v: double); "
        "class C public static method Main(): int "
        "  var a := new R(1.5); var b := new R(1.5); "
        "  return (if a = b then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.2 / §4.4: distinct (non-NaN) double fields are not equal — the NaN-aware
// path must not collapse different finite values.
BEZEL_TEST(codegen_cg29b, double_field_distinct_records_not_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record R(v: double); "
        "class C public static method Main(): int "
        "  var a := new R(1.0); var b := new R(2.0); "
        "  return (if a = b then 1 else 0); "
        "end Main; end C;"), 0);
}

// §9.2 / §4.4 (fix #1165 consistency): a record's 0.0 vs -0.0 field comparison
// must match the SCALAR `0.0 = -0.0` result (IEEE oeq → equal), so the record
// path and scalar path agree.
BEZEL_TEST(codegen_cg29b, double_field_signed_zero_matches_scalar) {
    BEZEL_EXPECT_EQ(run(
        "module M; record R(v: double); "
        "class C public static method Main(): int "
        "  var negz := -0.0; "
        "  var recEq := (if new R(0.0) = new R(negz) then 1 else 0); "
        "  var scalarEq := (if 0.0 = negz then 1 else 0); "
        "  return (if recEq = scalarEq then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.2 / §8.3: a record with mixed field types (double + int + string) still
// compares correctly field-by-field; the float path change is field-local.
BEZEL_TEST(codegen_cg29b, mixed_field_record_equality) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Mixed(d: double, n: int, s: string); "
        "class C public static method Main(): int "
        "  var a := new Mixed(3.5, 7, \"hi\"); "
        "  var b := new Mixed(3.5, 7, \"hi\"); "
        "  var eq := (if a = b then 1 else 0); "
        "  var c := new Mixed(3.5, 8, \"hi\"); "
        "  var neq := (if a = c then 1 else 0); "
        "  return (if eq = 1 and neq = 0 then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.2 / §8.3 / §35.2 (fix #2497): `this = other` (and `this <> other`) inside
// a record INSTANCE method must dispatch the auto-generated structural record
// equality, not abort the compiler. `this` in a value-type instance method
// lowers to a POINTER to the receiver storage (CG9; §35.2 states the choice
// form — "this in a choice instance method is a borrow of the receiver value"
// — and the record form follows the same receiver-ptr convention, see #575's
// lowerWith). Before the fix, the bare-`this` operand was a pointer while the
// by-value `other` operand was a struct, so the struct-equality dispatch
// (which keys on isStructTy()) skipped it and fell through to a raw ICmpEQ
// over mismatched operand types (ptr vs struct) — aborting in the LLVM IR
// verifier (ICmpInst::AssertOK operand-type-mismatch). The fix loads the
// receiver struct through the pointer before the equality dispatch. This test
// proves the VALUE: equal receivers compare true, a field mismatch compares
// false, and the result is symmetric (this on the right via the second call).
BEZEL_TEST(codegen_cg29b, this_eq_other_in_record_instance_method_2497) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pt(X: int, Y: int) "
        "  public method SelfEq(other: Pt): bool => this = other; "
        "end Pt; "
        "class C public static method Main(): int "
        "  var a := new Pt(1, 2); var b := new Pt(1, 2); "
        "  var c := new Pt(1, 9); var d := new Pt(3, 2); "
        "  var pass := 0; "
        "  if a.SelfEq(b) then pass := pass + 1; end if; "      // equal → true
        "  if not a.SelfEq(c) then pass := pass + 1; end if; "   // Y differs → false
        "  if not a.SelfEq(d) then pass := pass + 1; end if; "   // X differs → false
        "  if b.SelfEq(a) then pass := pass + 1; end if; "       // symmetric → true
        "  return (if pass = 4 then 1 else 0); "
        "end Main; end C;"), 1);
}
