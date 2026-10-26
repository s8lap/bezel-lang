// Regression tests for #7262 — `ISteppable<T>.Step` over an integer `T`
// SILENTLY WRAPPED when the advance left T's range.
//
// WHAT THE DOCUMENTS REQUIRE.
//
// spec §6.1 "Integer arithmetic" (v0.344): "Operators `+`, `-`, … are
// checked for overflow. An operation whose mathematical result is not
// representable in the result type raises `OverflowException`." The second
// sentence is GENERAL — it says "an OPERATION", not "an operator".
//
// spec §11.8 "Static interface members" (v0.344): "the compiler bridges the
// intrinsic/declared split by treating each primitive as conforming with its
// intrinsic operations as the member bodies". So at a primitive `T`,
// `ISteppable<T>.Step`'s member body is the primitive's own intrinsic
// operation, and §6.1 governs its overflow behaviour.
//
// stdlib §2.19 (v0.344) declares
//   public interface ISteppable<T> where T: ISteppable<T>
//     static method Step(value: T, by: long): owned T;   // advance in T's space
// and stdlib §5 records the principle for such a member: its arithmetic in
// `T`'s space "is overflow-checked in it (spec §6.1)" — the same rule the
// banked corpus row l5_0012 pins for `bytes.Sum()` over an `Array<byte>`
// (which raises `OverflowException` past 255).
//
// `char` is NOT an integer type (spec §3.1) and has NO intrinsic arithmetic
// (spec §4.4), so `ISteppable<char>.Step` is a CODE-POINT ADVANCE. Its
// domain is NOT contiguous: spec §4.5 / §5.2 (v0.344) make the surrogate
// block U+D800–U+DFFF invalid `char` values ("Casting an integer to `char`
// requires the integer to fall in U+0000–U+10FFFF excluding the surrogate
// range; otherwise `OverflowException`"), and spec §16.3 (v0.344) says the
// same of `char`'s own value sequence ("code points U+D800 through U+DFFF are
// surrogate code points and not valid `char` values"). Spec §6.6 (v0.344)
// gives the language's OWN char advance — the compound assignment and the
// `c++` / `c--` statements: "the write-back applies the §5.2 code-point
// rules: `c += 1` is `c := char(int(c) + 1)`" — so a char advance that lands
// on a surrogate RAISES there too.
//
// WHAT THE DEFECT WAS.
//
// The `Step` lowering TRUNCATED `by` to T's width BEFORE the add, so the
// add could never overflow: the wrap entered through the TRUNCATION, not
// through the add, and a check placed on the truncated add could never fire.
// Every integer `T` wrapped silently:
//
//   T.Step(1, 4000000000L)  over int  ->  -294967295   exit 0
//
// WHAT THE FIX IS.
//
// The mathematical sum is computed in i128 — `by` is an i64 and the value is
// T's width, so the exact sum can leave i64 itself (T = int, by =
// long.MaxValue) — and the RESULT is checked against T's REPRESENTABLE
// RANGE before it is narrowed back to T's width. The check is on the result's
// representability, not on the sign of `by`: a NEGATIVE `by` is a legitimate
// advance downwards and is rejected only when the result leaves T's range.
// `char` gets its own domain, the surrogate block excluded.
//
// THE SCOPE OF THE CHANGE.
//
//   * The COMPILER-PROVIDED conformance is what is fixed (spec §11.8's
//     bridge). A USER's own `Step` witness is user code and is untouched —
//     the `user_conformer_step_is_unchanged` row pins that.
//   * `float` / `double` are NOT checked (spec §6.3: floating-point overflow
//     produces `±Infinity`, and `+` on a floating type is not
//     overflow-checked). The control rows pin that.
//   * `decimal` was ALREADY checked (spec §6.4, the runtime's decimal add
//     raises). The decimal rows CONFIRM it and are unchanged.
//   * The RANGE's advance (spec §16.3, `emitRangeAdvance` in
//     `src/codegen/LlvmBackend_Stmts.cpp`) is a DIFFERENT member with a
//     DIFFERENT shape: a range's iteration stops SILENTLY at the end of its
//     domain. It is untouched here, and `range_char_surrogate_gap_1586`
//     stays green.
//
// THE PROOF HIERARCHY.
//
// Every PIN row below FAILS pre-fix: the program printed a wrapped value
// instead of raising, so the expected text was never produced. Each pin row
// therefore pins THE EXCEPTION (through the house `try`/`catch` shape of the
// corpus row l5_0012 — `catch OverflowException => Console.WriteLine(
// "caught")`), never merely "the program did not crash". The CONTROL rows
// are green on both arms and show this change did not widen its reach.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

using bezel::test::diagnosticMessageContains;

namespace {

// The generic helper whose BODY performs the `ISteppable<T>.Step` call —
// the shape that reaches the compiler-provided primitive conformance
// (spec §11.8, stdlib §2.19).
//
//   params  — the helper's FULL parameter list, e.g. "v: T" or "v: T, by: long"
//   byExpr  — the `by` argument written at the `T.Step` call
//   mainBody— the statements of `Main`
std::string stepProg(const std::string& params, const std::string& byExpr,
                     const std::string& mainBody) {
    return "module M; static class F "
           "  public static method Nxt<T>(" + params + "): T "
           "      where T: ISteppable<T> "
           "    return T.Step(v, " + byExpr + "); "
           "  end Nxt; "
           "end F; "
           "class C public static method Main(): int " + mainBody +
           "  return 0; "
           "end Main; end C;";
}

// The house try/catch shape (the corpus row l5_0012's): the advance is
// inside the `try` and the catch prints "caught", so a row can assert THE
// EXCEPTION rather than "the program did not crash".
std::string caughtStep(const std::string& params, const std::string& byExpr,
                       const std::string& args) {
    return stepProg(params, byExpr,
                    "  try "
                    "    Console.WriteLine(F.Nxt(" + args + ").ToString()); "
                    "  catch OverflowException => Console.WriteLine(\"caught\"); "
                    "  end; ");
}

// The program's stdout with ONE trailing line-ending stripped (the same
// rule `runAotOut` applies), so the expectation is platform-independent.
std::string stdoutOf(const JitResult& r) {
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();
    }
    return out;
}

// The accepting shape: the program RAN, it produced exactly this text, and
// it did not fail the compiler.
void expectStdout(const std::string& program, const std::string& expected) {
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_EQ(stdoutOf(r), expected);
}

// The UNCAUGHT shape: the exception reaches the process boundary
// (spec §33.3), which reports it and exits 70.
void expectUncaughtOverflow(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 70);
    BEZEL_EXPECT_EQ(stdoutOf(r), "");
}

}  // namespace

// ===========================================================================
// THE ISSUE'S OWN REPRO — the advance that leaves `int`'s range.
//
// Pre-fix this printed -294967295 (a silent modular wrap) and exited 0.
// Post-fix it raises `OverflowException` (spec §6.1, v0.344).
// ===========================================================================
BEZEL_TEST(steppable_overflow_7262, step_int_overflow_raises_6_1) {
    expectStdout(caughtStep("v: T", "4000000000L", "1"), "caught");
}

// The same overflow with the `by` a RUNTIME value (a `long` PARAMETER),
// so the check is made on a value the compiler cannot fold.
BEZEL_TEST(steppable_overflow_7262, step_int_runtime_by_overflow_raises_6_1) {
    expectStdout(stepProg("v: T, by: long", "by",
                          "  var b: long := 4000000000L; "
                          "  try "
                          "    Console.WriteLine(F.Nxt(1, b).ToString()); "
                          "  catch OverflowException => "
                          "    Console.WriteLine(\"caught\"); "
                          "  end; "),
                "caught");
}

// ===========================================================================
// THE CONTROLS — green on BOTH arms.
//
// An in-range advance still advances, and the floating `T`s are NOT
// checked (spec §6.3: floating-point overflow produces `±Infinity`, and `+`
// on a floating type is not overflow-checked).
// ===========================================================================
BEZEL_TEST(steppable_overflow_7262, step_int_in_range_still_advances_6_1) {
    expectStdout(caughtStep("v: T", "2L", "1"), "3");
}

BEZEL_TEST(steppable_overflow_7262, step_double_control_is_unchanged_6_3) {
    expectStdout(caughtStep("v: T", "4000000000L", "1.0"), "4000000001");
}

BEZEL_TEST(steppable_overflow_7262, step_float_control_is_unchanged_6_3) {
    expectStdout(caughtStep("v: T", "4000000000L", "1.0f"), "4e+09");
}

// ===========================================================================
// THE MATRIX — every integer type, in range / top / bottom.
//
// The bottom row is the NEGATIVE `by` case: a negative advance is a
// legitimate advance DOWNWARDS, and it is rejected only because the RESULT
// leaves T's range — the case a naive add-overflow flag misses.
// ===========================================================================

// byte: 0..255
BEZEL_TEST(steppable_overflow_7262, step_byte_in_range_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by", "byte(200), 50L"), "250");
}
BEZEL_TEST(steppable_overflow_7262, step_byte_top_overflow_raises_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by", "byte(200), 56L"),
                 "caught");
}
BEZEL_TEST(steppable_overflow_7262, step_byte_bottom_overflow_raises_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by", "byte(0), -1L"), "caught");
}

// sbyte: -128..127
BEZEL_TEST(steppable_overflow_7262, step_sbyte_in_range_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by", "sbyte(100), 27L"), "127");
}
BEZEL_TEST(steppable_overflow_7262, step_sbyte_top_overflow_raises_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by", "sbyte(100), 28L"),
                 "caught");
}
BEZEL_TEST(steppable_overflow_7262, step_sbyte_bottom_overflow_raises_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by", "sbyte(-128), -1L"),
                 "caught");
}

// short: -32768..32767
BEZEL_TEST(steppable_overflow_7262, step_short_in_range_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by", "short(30000), 2767L"),
                 "32767");
}
BEZEL_TEST(steppable_overflow_7262, step_short_top_overflow_raises_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by", "short(30000), 2768L"),
                 "caught");
}
BEZEL_TEST(steppable_overflow_7262, step_short_bottom_overflow_raises_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by", "short(-32768), -1L"),
                 "caught");
}

// ushort: 0..65535
BEZEL_TEST(steppable_overflow_7262, step_ushort_in_range_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by", "ushort(60000), 5535L"),
                 "65535");
}
BEZEL_TEST(steppable_overflow_7262, step_ushort_top_overflow_raises_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by", "ushort(60000), 5536L"),
                 "caught");
}
BEZEL_TEST(steppable_overflow_7262, step_ushort_bottom_overflow_raises_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by", "ushort(0), -1L"), "caught");
}

// int: -2147483648..2147483647
BEZEL_TEST(steppable_overflow_7262, step_int_max_still_advances_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by", "2147483647, 0L"),
                 "2147483647");
}
BEZEL_TEST(steppable_overflow_7262, step_int_top_overflow_raises_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by", "2147483647, 1L"),
                 "caught");
}
BEZEL_TEST(steppable_overflow_7262, step_int_bottom_overflow_raises_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by", "-2147483648, -1L"),
                 "caught");
}

// The MATHEMATICAL sum overflows i64 itself (T = int, by = long.MaxValue):
// the check cannot be made on an i64 sum.
BEZEL_TEST(steppable_overflow_7262, step_int_sum_beyond_i64_raises_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by",
                            "1, 9223372036854775807L"),
                 "caught");
}

// uint: 0..4294967295
BEZEL_TEST(steppable_overflow_7262, step_uint_max_still_advances_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by", "4294967295u, 0L"),
                 "4294967295");
}
BEZEL_TEST(steppable_overflow_7262, step_uint_top_overflow_raises_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by", "4294967295u, 1L"),
                 "caught");
}
BEZEL_TEST(steppable_overflow_7262, step_uint_bottom_overflow_raises_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by", "0u, -1L"), "caught");
}

// long: -9223372036854775808..9223372036854775807
BEZEL_TEST(steppable_overflow_7262, step_long_max_still_advances_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by",
                            "9223372036854775807L, 0L"),
                 "9223372036854775807");
}
BEZEL_TEST(steppable_overflow_7262, step_long_top_overflow_raises_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by",
                            "9223372036854775807L, 1L"),
                 "caught");
}
BEZEL_TEST(steppable_overflow_7262, step_long_bottom_overflow_raises_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by",
                            "-9223372036854775808L, -1L"),
                 "caught");
}

// ulong: 0..18446744073709551615
BEZEL_TEST(steppable_overflow_7262, step_ulong_max_still_advances_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by",
                            "18446744073709551615UL, 0L"),
                 "18446744073709551615");
}
BEZEL_TEST(steppable_overflow_7262, step_ulong_top_overflow_raises_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by",
                            "18446744073709551615UL, 1L"),
                 "caught");
}
BEZEL_TEST(steppable_overflow_7262, step_ulong_bottom_overflow_raises_6_1) {
    expectStdout(caughtStep("v: T, by: long", "by", "0UL, -1L"), "caught");
}

// ===========================================================================
// THE `char` ROWS — the domain is NOT the i32 range and NOT contiguous.
//
// spec §5.2 (v0.344): "Casting an integer to `char` requires the integer to
// fall in U+0000–U+10FFFF excluding the surrogate range; otherwise
// `OverflowException`." spec §16.3 (v0.344): the surrogate block is not part
// of `char`'s value sequence.
//
// The surrogate row is the load-bearing one: U+D7FF + 1 = U+D800 is INSIDE
// the i32 range and inside U+0000–U+10FFFF, so a plain range check would
// admit it. The language's own char advance agrees — spec §6.6 (v0.344):
// "`c += 1` is `c := char(int(c) + 1)`" with §5.2's checked code-point
// write-back, so `char(0xD7FF) += 1` raises (pinned by
// `char_compound_codepoint_1961.plus_equals_into_surrogate_throws`).
// ===========================================================================
BEZEL_TEST(steppable_overflow_7262, step_char_in_range_16_3) {
    expectStdout(caughtStep("v: T, by: long", "by", "'a', 2L"), "c");
}
BEZEL_TEST(steppable_overflow_7262, step_char_above_max_raises_5_2) {
    expectStdout(caughtStep("v: T, by: long", "by", "'\\U0010FFFF', 1L"),
                 "caught");
}
BEZEL_TEST(steppable_overflow_7262, step_char_below_min_raises_5_2) {
    expectStdout(caughtStep("v: T, by: long", "by", "'\\u0000', -1L"),
                 "caught");
}
BEZEL_TEST(steppable_overflow_7262, step_char_surrogate_block_raises_16_3) {
    expectStdout(caughtStep("v: T, by: long", "by", "'\\uD7FF', 1L"),
                 "caught");
}

// ===========================================================================
// THE `+` CONSISTENCY ROW — the point of the fix is that `Step` and `+`
// AGREE. `a + b` over `int` raises (spec §6.1) and MUST KEEP doing so.
// ===========================================================================
BEZEL_TEST(steppable_overflow_7262, plus_over_int_still_raises_6_1) {
    expectUncaughtOverflow(
        "module M; class C public static method Main(): int "
        "  var a: int := 2000000000; "
        "  var b: int := 2000000000; "
        "  var c := a + b; "
        "  Console.WriteLine(c.ToString()); "
        "  return 0; "
        "end Main; end C;");
}

// ===========================================================================
// THE `decimal` ROW — CONFIRM, DO NOT CHANGE.
//
// spec §6.4 (v0.344) already governs decimal arithmetic and the runtime's
// decimal add ALREADY raised on overflow; this change does not touch that
// path. The row pins that the raise is still there.
// ===========================================================================
BEZEL_TEST(steppable_overflow_7262, step_decimal_in_range_6_4) {
    expectStdout(caughtStep("v: T, by: long", "by", "1.5m, 2L"), "3.5");
}
BEZEL_TEST(steppable_overflow_7262, step_decimal_overflow_still_raises_6_4) {
    expectStdout(caughtStep("v: T, by: long", "by",
                            "79228162514264337593543950335m, "
                            "9223372036854775807L"),
                 "caught");
}

// ===========================================================================
// THE USER-DEFINED CONFORMER — the user path is UNCHANGED.
//
// stdlib §2.19 (v0.344): "`ISteppable<T>` is user-conformable". A user's
// own `Step` witness is USER CODE: the compiler-provided conformance is
// what this fix changed, and a user's `Step` is lowered by the ordinary
// user-static path. The row pins that the user's own advance is untouched —
// including that it does NOT gain the §6.1 check (the user's witness is
// their own business).
// ===========================================================================
BEZEL_TEST(steppable_overflow_7262, user_conformer_step_is_unchanged_2_19) {
    expectStdout(
        "module M; "
        "public record Counter(N: int): ISteppable<Counter> "
        "  public static method Step(value: Counter, by: long): Counter "
        "    => new Counter(value.N + int(by)); "
        "  public static method operator <(a: Counter, b: Counter): bool "
        "    => a.N < b.N; "
        "  public static method operator >(a: Counter, b: Counter): bool "
        "    => a.N > b.N; "
        "  public static method operator <=(a: Counter, b: Counter): bool "
        "    => a.N <= b.N; "
        "  public static method operator >=(a: Counter, b: Counter): bool "
        "    => a.N >= b.N; "
        "end Counter; "
        "static class F "
        "  public static method Nxt<T>(v: T, by: long): T "
        "      where T: ISteppable<T> "
        "    return T.Step(v, by); "
        "  end Nxt; "
        "end F; "
        "class C public static method Main(): int "
        "  Console.WriteLine(F.Nxt(new Counter(10), 5L).N.ToString()); "
        "  return 0; "
        "end Main; end C;",
        "15");
}
