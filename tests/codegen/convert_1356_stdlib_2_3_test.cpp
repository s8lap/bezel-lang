// Codegen tests for Convert stdlib alignment — issue #1356 / stdlib §2.3.
//
// New members added in v0.165 (enumerated in stdlib §2.3):
//   - Convert.ToDecimal(value: string): decimal
//   - Convert.TryToInt64(value: string, out result: long): bool
//   - Convert.TryToDouble(value: string, out result: double): bool
//   - Convert.TryToDecimal(value: string, out result: decimal): bool
//   - Convert.TryToBoolean(value: string, out result: bool): bool
//
// Each test cites the stdlib section and specific behaviour it verifies.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// stdlib §2.3 / #1356: Convert.ToDecimal(value: string): decimal
// Simple value-round-trip: parse "3" and compare to decimal literal 3.0m.
// Note: Bezel equality operator is `=` (not `==`); decimal literals require a
// decimal point in expression context (e.g. `3.0m` not `3m`).
BEZEL_TEST(codegen_convert_1356_stdlib_2_3, to_decimal_parses_integer) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := Convert.ToDecimal(\"3\"); "
        "if d = 3.0m then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.3 / #1356: Convert.ToDecimal — fractional value.
BEZEL_TEST(codegen_convert_1356_stdlib_2_3, to_decimal_parses_fraction) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := Convert.ToDecimal(\"1.5\"); "
        "if d = 1.5m then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.3 / #1356: Convert.TryToInt64 — valid input → true, correct value.
BEZEL_TEST(codegen_convert_1356_stdlib_2_3, try_to_int64_valid) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: long := 0L; "
        "var ok := Convert.TryToInt64(\"9876543210\", out result); "
        "if ok and result = 9876543210L then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.3 / #1356: Convert.TryToInt64 — invalid input → false.
BEZEL_TEST(codegen_convert_1356_stdlib_2_3, try_to_int64_invalid) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: long := 0L; "
        "var ok := Convert.TryToInt64(\"not-a-number\", out result); "
        "if (not ok) and result = 0L then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.3 / #1356: Convert.TryToDouble — valid input → true, correct value.
BEZEL_TEST(codegen_convert_1356_stdlib_2_3, try_to_double_valid) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: double := 0.0; "
        "var ok := Convert.TryToDouble(\"3.14\", out result); "
        "if ok and result > 3.13 and result < 3.15 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.3 / #1356: Convert.TryToDouble — invalid input → false.
BEZEL_TEST(codegen_convert_1356_stdlib_2_3, try_to_double_invalid) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: double := 0.0; "
        "var ok := Convert.TryToDouble(\"abc\", out result); "
        "if not ok then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.3 / #1356: Convert.TryToDecimal — valid input → true.
BEZEL_TEST(codegen_convert_1356_stdlib_2_3, try_to_decimal_valid) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: decimal := 0.0m; "
        "var ok := Convert.TryToDecimal(\"42\", out result); "
        "if ok and result = 42.0m then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.3 / #1356: Convert.TryToDecimal — invalid → false.
BEZEL_TEST(codegen_convert_1356_stdlib_2_3, try_to_decimal_invalid) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: decimal := 0.0m; "
        "var ok := Convert.TryToDecimal(\"notdecimal\", out result); "
        "if not ok then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.3 / #1356: Convert.TryToBoolean — "true" → true, returns true.
BEZEL_TEST(codegen_convert_1356_stdlib_2_3, try_to_boolean_true_string) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: bool := false; "
        "var ok := Convert.TryToBoolean(\"true\", out result); "
        "if ok and result then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.3 / #1356: Convert.TryToBoolean — "false" → stores false, returns true.
BEZEL_TEST(codegen_convert_1356_stdlib_2_3, try_to_boolean_false_string) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: bool := true; "
        "var ok := Convert.TryToBoolean(\"false\", out result); "
        "if ok and (not result) then return 1; end; "
        "return 0;"
    )), 1);
}

// fix #5263 (§22.4 / stdlib §2.3, v0.313): Convert.TryToDouble with an
// inline `out var` — the out-var slot was defaulted to i32 (4 bytes) instead
// of double (8 bytes), so strtod wrote 8 bytes through a 4-byte slot:
// stack-buffer-overflow (ASan).  Same pattern for Convert.TryToInt64.
// The pre-declared form (out result: double) was always correct.
BEZEL_TEST(codegen_convert_1356_stdlib_2_3, try_to_double_out_var_inline) {
    BEZEL_EXPECT_EQ(run(prog(
        "var ok := Convert.TryToDouble(\"3.14\", out var d); "
        "if ok and d > 3.13 and d < 3.15 then return 1; end; "
        "return 0;"
    )), 1);
}

BEZEL_TEST(codegen_convert_1356_stdlib_2_3, try_to_int64_out_var_inline) {
    BEZEL_EXPECT_EQ(run(prog(
        "var ok := Convert.TryToInt64(\"9876543210\", out var n); "
        "if ok and n = 9876543210L then return 1; end; "
        "return 0;"
    )), 1);
}
