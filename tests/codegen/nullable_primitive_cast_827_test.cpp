// fix #827 (bug10/bug12) / §5.3: an explicit cast of a NULLABLE value to a
// NON-NULLABLE primitive must unwrap the nullable first ("casting a nullable to
// its non-nullable form requires the value to be non-null at runtime", raising
// NullReferenceException when null). Before the fix the {i8, T} nullable struct
// flowed into the scalar conversion paths as if it were a bare T:
//   int(n: int?)      → llvm::cast<IntegerType> assertion abort (bug10)
//   double(n: double?) → `sitofp` on the struct → IR-verification failure (bug12)
// (The nullable-TARGET cast `long?(x)` and the implicit int?→double? lifted
// widening are distinct paths, tracked separately as #839 / #840.)
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
std::string mainOf(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}
}  // namespace

// bug10: int(n) where n: int? unwraps to 5 (was a cast<IntegerType> abort).
BEZEL_TEST(nullable_primitive_cast_827, int_of_nullable_int) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var n: int? := 5; var v: int := int(n); return v;")), 5);
}

// bug12: double(n) where n: double? unwraps to 3.0 (was sitofp on the struct).
BEZEL_TEST(nullable_primitive_cast_827, double_of_nullable_double) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var n: double? := 3.0; var d: double := double(n); return int(d);")), 3);
}

// §5.3: casting a NULL nullable raises NullReferenceException at runtime.
BEZEL_TEST(nullable_primitive_cast_827, null_cast_raises) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var n: int? := null; "
        "try var v: int := int(n); return v; "
        "catch Exception => return 7; end;")), 7);
}

// Cross-type: int(n) where n: long? unwraps and narrows to int.
BEZEL_TEST(nullable_primitive_cast_827, int_of_nullable_long) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var n: long? := 9L; return int(n);")), 9);
}

// Cross-type: int(n) where n: double? unwraps and FPToSI-truncates.
BEZEL_TEST(nullable_primitive_cast_827, int_of_nullable_double_truncates) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var n: double? := 7.9; return int(n);")), 7);
}

// Cross-type: double(n) where n: int? unwraps and SIToFP-widens.
BEZEL_TEST(nullable_primitive_cast_827, double_of_nullable_int) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var n: int? := 5; var d: double := double(n); return int(d);")), 5);
}

// Regression: a NON-nullable explicit cast is unaffected.
BEZEL_TEST(nullable_primitive_cast_827, non_nullable_cast_regression) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var d: double := 4.8; return int(d);")), 4);
}
