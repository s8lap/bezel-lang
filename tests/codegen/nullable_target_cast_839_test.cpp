// fix #839 / §5.3: a cast to a NULLABLE target where the source is already
// nullable mis-lowered. The lexer produces `?(` as a 2-char punctuator, so
// `long?(x)` is parsed as NullCondCallExpr{callee=IdentifierExpr{"long"},
// args=[x]} rather than a CallExpr. The codegen fell through to the null-
// conditional function-call path which returned nullptr (not a function
// callee), leaving the destination local uninitialized — null stays null,
// value is lost.
//
// Fix: detect the primitive-callee case in the NullCondCallExpr handler
// BEFORE the function-call check, and lower it as a §5.3 lifted cast:
//   extract (hasValue, payload) from the source nullable struct,
//   convert the payload from source primitive to target primitive via the
//   same §5.2 scalar rules used by the non-nullable cast,
//   and re-wrap as the target {i8 hasValue, targetScalar} nullable struct.
// Null stays null (hasValue=0 propagates), value stays value.
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
std::string mainOf(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}
}  // namespace

// §5.3 / #839: identity nullable cast long?(x) where x: long? := 42 → 42.
// Was rc=0 (wrong value — uninitialized slot from nullptr lowerExpr).
BEZEL_TEST(nullable_target_cast_839, identity_value_preserved) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: long? := 42; "
        "var y: long? := long?(x); "
        "if y = null then return 99; end; "
        "if y! = 42 then return 42; end; "
        "return 0;")), 42);
}

// §5.3 / #839: null stays null — long?(null) → null.
BEZEL_TEST(nullable_target_cast_839, null_stays_null) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: long? := null; "
        "var y: long? := long?(x); "
        "if y = null then return 7; end; "
        "return 0;")), 7);
}

// §5.3 / #839: cross-type widening nullable cast: int? → long?.
// Source payload (i32) must be sign-extended to i64.
BEZEL_TEST(nullable_target_cast_839, cross_type_widen_int_to_long) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var i: int? := 5; "
        "var y: long? := long?(i); "
        "if y = null then return 99; end; "
        "if y! = 5 then return 5; end; "
        "return 0;")), 5);
}

// §5.3 / #839: cross-type narrowing nullable cast: long? → int?.
// Source payload (i64) must be Trunc'd to i32 with overflow check.
BEZEL_TEST(nullable_target_cast_839, cross_type_narrow_long_to_int) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var l: long? := 5; "
        "var y: int? := int?(l); "
        "if y = null then return 99; end; "
        "if y! = 5 then return 5; end; "
        "return 0;")), 5);
}

// §5.3 / #839: null propagates through a cross-type nullable cast.
BEZEL_TEST(nullable_target_cast_839, cross_type_null_propagates) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var i: int? := null; "
        "var y: long? := long?(i); "
        "if y = null then return 7; end; "
        "return 0;")), 7);
}

// §5.3 / #839: double? → int? nullable cast (FP → int with range check).
BEZEL_TEST(nullable_target_cast_839, cross_type_fp_to_int_nullable) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var d: double? := 7.9; "
        "var y: int? := int?(d); "
        "if y = null then return 99; end; "
        "if y! = 7 then return 7; end; "
        "return 0;")), 7);
}

// §5.3 / #839: int? → double? nullable cast (int → FP).
BEZEL_TEST(nullable_target_cast_839, cross_type_int_to_fp_nullable) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var i: int? := 5; "
        "var d: double? := double?(i); "
        "if d = null then return 99; end; "
        "return int(d!);")), 5);
}

// Regression §5.3 / #827: the NON-nullable-target cast (x: long? → long)
// must still unwrap and raise NullReferenceException on null. The #839 fix
// must NOT disturb the #827 path.
BEZEL_TEST(nullable_target_cast_839, regression_827_nonnullable_target_unwrap) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: long? := 42; "
        "var z: long := long(x); "
        "if z = 42 then return 42; end; "
        "return 0;")), 42);
}

// Regression §5.3 / #827: null → non-nullable cast raises NullReferenceException.
BEZEL_TEST(nullable_target_cast_839, regression_827_null_raises) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: long? := null; "
        "try var z: long := long(x); return int(z); "
        "catch Exception => return 7; end;")), 7);
}

// Regression: a plain non-nullable scalar cast is unaffected.
BEZEL_TEST(nullable_target_cast_839, regression_scalar_cast_unaffected) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var d: double := 4.8; return int(d);")), 4);
}
