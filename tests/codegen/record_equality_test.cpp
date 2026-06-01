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

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §8.3 positional record: two identical values compare equal.
BEZEL_TEST(codegen_cg29b, equal_records_are_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new Point(1, 2); var b := new Point(1, 2); "
        "  return (1 if a = b else 0); "
        "end Main; end C;"), 1);
}

// Different values do not compare equal.
BEZEL_TEST(codegen_cg29b, different_records_are_not_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new Point(1, 2); var b := new Point(1, 3); "
        "  return (1 if a = b else 0); "
        "end Main; end C;"), 0);
}

// <> is the negation of =.
BEZEL_TEST(codegen_cg29b, neq_operator) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new Point(1, 2); var b := new Point(9, 9); "
        "  return (1 if a <> b else 0); "
        "end Main; end C;"), 1);
}

// Every field is checked — first field differs.
BEZEL_TEST(codegen_cg29b, first_field_mismatch) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new Point(7, 2); var b := new Point(1, 2); "
        "  return (1 if a = b else 0); "
        "end Main; end C;"), 0);
}

// Three-field record — equality must check all three.
BEZEL_TEST(codegen_cg29b, three_field_equality) {
    BEZEL_EXPECT_EQ(run(
        "module M; record T(a: int, b: int, c: int); "
        "class C public static method Main(): int "
        "  var p := new T(1, 2, 3); var q := new T(1, 2, 3); "
        "  return (1 if p = q else 0); "
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
        "  return (1 if x = y else 0); "
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
        "  return (1 if x = y else 0); "
        "end Main; end C;"), 0);
}
