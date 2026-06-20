// fix #840 / §5.1 / §7: an implicit nullable→nullable LIFTED numeric widening
// (`var d: double? := n` with `n: int?`) lost the value — the `{i8, i32}` int?
// struct was stored as-is into the `{i8, double}` double? slot. The local-decl
// nullable handling now performs the lifted conversion: keep hasValue, convert
// the inner value to the target inner type (int→double SIToFP, int→long
// SExt/ZExt), and re-wrap. Null still propagates as null.
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

// int? → double?: the value is widened (SIToFP), not lost. d! > 4.0 holds for 5.
BEZEL_TEST(nullable_lifted_widening_840, int_q_to_double_q) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var n: int? := 5; var d: double? := n; "
        "if d = null then return 99; end; "
        "return (1 if d! > 4.0 else 0);")), 1);
}

// int? → long?: integer widening (SExt) preserves the value.
BEZEL_TEST(nullable_lifted_widening_840, int_q_to_long_q) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var n: int? := 7; var d: long? := n; "
        "if d = null then return 99; end; "
        "var v: long := d!; if v = 7L then return 1; end; return 2;")), 1);
}

// A null int? widens to a null double? (null propagates).
BEZEL_TEST(nullable_lifted_widening_840, null_propagates) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var n: int? := null; var d: double? := n; "
        "if d = null then return 1; end; return 2;")), 1);
}

// The widened double? round-trips the exact value (5 → 5.0 → 5).
BEZEL_TEST(nullable_lifted_widening_840, value_round_trips) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var n: int? := 5; var d: double? := n; return int(d!);")), 5);
}

// Regression: int? → int? (same inner type) is unchanged.
BEZEL_TEST(nullable_lifted_widening_840, same_type_regression) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var n: int? := 5; var d: int? := n; return d!;")), 5);
}
