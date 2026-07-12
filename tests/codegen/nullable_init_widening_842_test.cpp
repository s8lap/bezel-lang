// fix #842 / §5.1 / §7: a NARROWER numeric initializer assigned to a WIDER
// nullable value-type local was stored UNWIDENED into the `{i8, T}` nullable
// slot — `var x: long? := 42` wrapped the i32 literal into a `{i8, i64}` slot
// without the i32→i64 widening, corrupting the value (read back as garbage).
// The local-decl nullable wrap only fired on an EXACT inner-type match; it now
// coerces the value to the nullable's inner type first (int→long via SExt/ZExt,
// int→double via SIToFP, int→decimal via the decimal conversion).
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

// `long? := <int literal>`: the int 42 is widened to i64 before wrapping (was
// stored unwidened → x! read back as garbage).
BEZEL_TEST(nullable_init_widening_842, long_nullable_from_int_literal) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: long? := 42; var v: long := x!; "
        "if v = 42L then return 1; end; return 2;")), 1);
}

// A larger value that does not fit in the low bits trivially (still widened).
BEZEL_TEST(nullable_init_widening_842, long_nullable_from_int_value) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var n: int := 100; var x: long? := n; var v: long := x!; "
        "return int(v);")), 100);
}

// `double? := <int>`: int→double widening (SIToFP) before wrapping.
BEZEL_TEST(nullable_init_widening_842, double_nullable_from_int) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: double? := 7; var v: double := x!; "
        "if v = 7.0 then return 1; end; return 2;")), 1);
}

// `decimal? := <int>`: int→decimal conversion before wrapping.
BEZEL_TEST(nullable_init_widening_842, decimal_nullable_from_int) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: decimal? := 3; var v: decimal := x!; "
        "if v = 3m then return 1; end; return 2;")), 1);
}

// Regression: `int? := <int>` (exact inner-type match) still works.
BEZEL_TEST(nullable_init_widening_842, int_nullable_exact_match) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: int? := 5; var v: int := x!; return v;")), 5);
}

// Regression: `long? := null` still produces a null nullable.
BEZEL_TEST(nullable_init_widening_842, long_nullable_null) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: long? := null; if x = null then return 1; end; return 2;")), 1);
}

// Regression: `long? := <long literal>` (already i64) still works.
BEZEL_TEST(nullable_init_widening_842, long_nullable_from_long_literal) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: long? := 42L; var v: long := x!; "
        "if v = 42L then return 1; end; return 2;")), 1);
}
