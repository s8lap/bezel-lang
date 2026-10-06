// fix #840 / §5.1 / §7: an implicit nullable→nullable LIFTED numeric widening
// (`var d: double? := n` with `n: int?`) lost the value — the `{i8, i32}` int?
// struct was stored as-is into the `{i8, double}` double? slot. The local-decl
// nullable handling now performs the lifted conversion: keep hasValue, convert
// the inner value to the target inner type (int→double SIToFP, int→long
// SExt/ZExt), and re-wrap. Null still propagates as null.
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

bool runs(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran;
}

}  // namespace

// int? → double?: the value is widened (SIToFP), not lost. d! > 4.0 holds for 5.
BEZEL_TEST(nullable_lifted_widening_840, int_q_to_double_q) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var n: int? := 5; var d: double? := n; "
        "if d = null then return 99; end; "
        "return (if d! > 4.0 then 1 else 0);")), 1);
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

// ── #1117: lifted nullable arithmetic with a WIDER non-nullable operand ───────
//
// `int? + long` must compute in the PROMOTED width (long, i64), not the narrow
// nullable operand's width (int, i32). Before the fix the result struct payload
// was sized from `int?` and the widened i64 sum was truncated to i32 → silent
// wrap. (The nullable operand is proven non-null here, so §7.11/#914 types the
// result as the non-nullable `long`; the result must still hold the full value.)

// §7.11 (v0.286, #3298) / BEHAVIOR FLIP (§7.4, spec:1061, read through §7.11,
// spec:1177; #4718 ruled YES, v0.309): each `a` below is a LOCAL initialized
// with a non-null literal, and the previous text here claimed that §7.4
// therefore "narrows it at the initializer" so §7.11 reads it at the
// non-nullable `int` — then called that claim FALSE and said init-narrowing
// "does NOT admit the §7.11 operator". That verdict was itself wrong, and the
// claim it rejected is correct: §7.4's establishment list names the
// declaration form ("`var x: T? := expr;` … which is the declaration's
// assignment"), so a non-null initializer DOES narrow, and §7.11 — which
// "reads the operand's static type" — then admits the ordinary operator
// unchanged. `int? + long` with `a` initialized to 2e9 compiles.
// The three rows are nevertheless respelled with an explicit §7.4 NULL-TEST
// narrowing (`if a <> null then`). That spelling is ADMISSIBLE, not required;
// it is kept because it keeps #1117's value assertions intact: what is pinned
// is that the sum computes in the PROMOTED width. The `param_*` rows below
// remain the control, and their reason is a GENUINELY nullable operand — a
// nullable PARAMETER `a: int?` that §7.4 narrows nothing for, so it IS
// rejected.

// int? + long: 2e9 + 2e9 = 4e9 must NOT wrap to a 32-bit value.
// BEHAVIOR FLIP (§7.4, spec:1061 + §7.11, spec:1177; #4718 / v0.309):
// respelled with the §7.4 NULL-TEST narrowing — ADMISSIBLE, not required,
// since a non-null initializer narrows at this gate too. The WIDTH is what
// #1117 pins, so these stay POSITIVE with their values asserted.
BEZEL_TEST(nullable_lifted_widening_840, int_q_plus_long_no_wrap_1117) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var a: int? := 2000000000; var b: long := 2000000000L; "
        "var c: long? := null; "
        "if a <> null then c := a + b; end if; "
        "if (c ?? 0L) = 4000000000L then return 1; else return 0; end;")), 1);
}

// int? * long: 100000 * 100000 = 1e10 (well beyond i32) computed in i64.
BEZEL_TEST(nullable_lifted_widening_840, int_q_times_long_wide_1117) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var a: int? := 100000; var b: long := 100000L; "
        "var c: long? := null; "
        "if a <> null then c := a * b; end if; "
        "if (c ?? 0L) = 10000000000L then return 1; else return 0; end;")), 1);
}

// Nested arithmetic `int? + long + long`: the inner narrowed sum is an i64
// scalar that the outer `+ long` consumes. 1 + 2 + 2 = 5.
BEZEL_TEST(nullable_lifted_widening_840, nested_int_q_plus_long_no_irfail_1117) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var a: int? := 1; var b: long := 2L; "
        "var c: long? := null; "
        "if a <> null then c := a + b + b; end if; "
        "return int(c ?? 0L);")), 5);
}

// A genuinely-nullable (non-narrowed) int? + long via params still lifts to
// long? and preserves the wide value when both operands are present.
BEZEL_TEST(nullable_lifted_widening_840, param_int_q_plus_long_wide_1117) {
    BEZEL_EXPECT_FALSE(runs(
        "module M; class C "
        "  private static method Add(a: int?, b: long): long? => a + b; "
        "  public static method Main(): int "
        "    var c: long? := Add(2000000000, 2000000000L); "
        "    if (c ?? 0L) = 4000000000L then return 1; else return 0; end; "
        "  end Main; end C;"));
}

// Null still propagates through the widened lifted op (a is null → result null).
BEZEL_TEST(nullable_lifted_widening_840, wide_lifted_null_propagates_1117) {
    BEZEL_EXPECT_FALSE(runs(
        "module M; class C "
        "  private static method Add(a: int?, b: long): long? => a + b; "
        "  public static method Main(): int "
        "    var c: long? := Add(null, 2000000000L); "
        "    if c = null then return 1; else return 0; end; "
        "  end Main; end C;"));
}
