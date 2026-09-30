// fix #5054 (§3.5, v0.307): arithmetic (`+`, `-`, `*`, `/`) on `char` is
// rejected. §3.5 enumerates arithmetic intrinsic operators as defined on
// "byte, sbyte, short, ushort, int, uint, long, ulong, float, double,
// decimal" — `char` is explicitly absent from that list, and the "invalid on
// bool, char, and string" wording appears in the unary `+` rule.
//
// Root cause: `reportBoolOperandExcludedFromArithmeticOrOrdering` checked only
// `bool`; `char` fell through to the char→int widening path (§5.1), so
// `c * 2` was accepted as `int * int`. The fix extends the operand-kind
// check to also cover `char` for arithmetic/ordering operators (not ordering
// itself, since §3.5 says "ordering on every primitive except `bool`" and
// char is not excepted).
//
// What each row is for:
// T1  `c * 2`:   char arithmetic — must be rejected.
// T2  `c + 1`:   char + int (widening char→int would admit if char
//                  arithmetic were legal) — must be rejected.
// T3  `1 + c`:   int + char (symmetric) — must be rejected.
// T4  `c - c`:   char - char — must be rejected.
// T5  `c / c`:   char / char — must be rejected.
// T6  `+c`:      unary + on char — also invalid per §3.5 (unary + is the
//                  identity, defined exactly where a numeric operand is;
//                  invalid on bool, char, and string) — must be rejected.
// T7  ACCEPT:    `int(c) * 2` explicitly converts to int first, then
//                  multiplies — must compile and produce 194.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::diagnosticMessageCount;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// T1: `c * 2` — the issue repro.
BEZEL_TEST(char_arithmetic_rejected_5054,
           char_times_int_rejected_spec_3_5) {
    auto r = runAot(
        "module M5054A; "
        "class C "
        "  public static method Main(): int "
        "    var c: char := 'a'; "
        "    var u := c * 2; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`char`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not defined"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "3.5"));
}

// T2: `c + 1` — char + int.
BEZEL_TEST(char_arithmetic_rejected_5054,
           char_plus_int_rejected_spec_3_5) {
    auto r = runAot(
        "module M5054B; "
        "class C "
        "  public static method Main(): int "
        "    var c: char := 'a'; "
        "    var u := c + 1; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`char`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not defined"));
}

// T3: `1 + c` — int + char (symmetric).
BEZEL_TEST(char_arithmetic_rejected_5054,
           int_plus_char_rejected_spec_3_5) {
    auto r = runAot(
        "module M5054C; "
        "class C "
        "  public static method Main(): int "
        "    var c: char := 'a'; "
        "    var u := 1 + c; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`char`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not defined"));
}

// T4: `c - c` — char - char.
BEZEL_TEST(char_arithmetic_rejected_5054,
           char_minus_char_rejected_spec_3_5) {
    auto r = runAot(
        "module M5054D; "
        "class C "
        "  public static method Main(): int "
        "    var c: char := 'a'; "
        "    var u := c - c; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`char`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not defined"));
}

// T5: `c / c` — char / char.
BEZEL_TEST(char_arithmetic_rejected_5054,
           char_div_char_rejected_spec_3_5) {
    auto r = runAot(
        "module M5054E; "
        "class C "
        "  public static method Main(): int "
        "    var c: char := 'a'; "
        "    var u := c / c; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`char`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not defined"));
}

// T6: unary `+` on char — §3.5 "unary + is the identity, defined exactly
// where a numeric operand is; invalid on bool, char, and string".
BEZEL_TEST(char_arithmetic_rejected_5054,
           unary_plus_on_char_rejected_spec_3_5) {
    auto r = runAot(
        "module M5054F; "
        "class C "
        "  public static method Main(): int "
        "    var c: char := 'a'; "
        "    var u := +c; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`char`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not valid"));
}

// T7: ACCEPT — explicit `int(c) * 2` must compile and produce 194 (97 * 2).
BEZEL_TEST(char_arithmetic_rejected_5054,
           explicit_int_conversion_then_multiply_accepted_spec_5_2) {
    auto r = runAot(
        "module M5054G; "
        "class C "
        "  public static method Main(): int "
        "    var c: char := 'a'; "   // 'a' == 97
        "    var u := int(c) * 2; "
        "    Console.WriteLine(u.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("194\n"));
}

// T8: fix #5125 (§3.5 / stdlib §3.3-§4.16, v0.307) — the issue's exact
// repro: the char-arithmetic gate above only reached `char` operands whose
// declared type was resolved to Primitive::Char directly. A `for var c in sp`
// loop variable over a `ReadOnlyStringSpan` (stdlib §3.3) never got that
// type: ExprTypecheck.cpp's for-in loop-variable inference had a case for a
// string source (§4.6, char) and for Named types with generic args or a
// user `GetEnumerator`, but ReadOnlyStringSpan is a zero-arity Named type
// iterated via the §4.16 direct-indexed-loop shape, so none of those cases
// matched and the loop variable's type was left unset, quietly bypassing the
// §3.5 gate. `sum + c` therefore compiled to a plain int add rather than
// being rejected. Fixed by adding an explicit `declaration->name ==
// "ReadOnlyStringSpan"` case that assigns Primitive::Char, mirroring the
// element type the indexer already uses (`ReadOnlyStringSpan[i] → char`,
// ExprTypecheck_Exprs.cpp).
BEZEL_TEST(char_arithmetic_rejected_5054,
           span_for_in_loop_variable_char_arithmetic_rejected_5125) {
    auto r = runAot(
        "module M5125; "
        "class C "
        "  public static method Main(): int "
        "    var s := \"hello\"; "
        "    var sp := s.AsSpan(); "
        "    var sum := 0; "
        "    for var c in sp do sum := sum + c; end; "
        "    Console.WriteLine(sum.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`char`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not defined"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "3.5"));
}

// T9: fix #5150 (§6.6, v0.309) — `c *= 2` and `c /= 2` on a `char`
// target must be rejected. §6.6 (v0.309) is explicit: "`c += 1` is
// `c := char(int(c) + 1)`" but "`c *= 2`, `c /= 2` and `c mod= n` are
// compile errors — `char` is not an arithmetic type, so a code point is
// advanced, never scaled". The previous implementation admitted `*=` and
// `/=` because `allowCharWriteBack` used a broad `(isTrueInt(rTy) ||
// rTy == Char)` predicate; the desugaring would be `c := char(int(c) *
// int(r))` / `c := char(int(c) / int(r))`, which scales rather than
// advances the code point and violates the spec. Fix: narrow
// `allowCharWriteBack` to `+=` and `-=` only.
BEZEL_TEST(char_arithmetic_rejected_5054,
           char_times_eq_rejected_spec_6_6) {
    auto r = runAot(
        "module M5150_9A; "
        "class C "
        "  public static method Main(): int "
        "    var c: char := 'a'; "
        "    c *= 2; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`char`"));
}

BEZEL_TEST(char_arithmetic_rejected_5054,
           char_div_eq_rejected_spec_6_6) {
    auto r = runAot(
        "module M5150_9B; "
        "class C "
        "  public static method Main(): int "
        "    var c: char := 'a'; "
        "    c /= 2; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`char`"));
}

// T10: #5150 x #5211 interaction (§6.6, v0.313) — a `char` target under
// `*=`, `/=` or `mod=` is rejected by the §6.6 "`+=`/`-=` only" rule FIRST,
// with a single diagnostic, even when the right operand is a non-constant
// `uint` (which #5211's own arm, "typed as in `int op e`", would also
// reject for `+=`/`-=`). The #5211 wording must be ABSENT here: the operator,
// not the operand type, is the reason the compound form is inadmissible.
// `mod=` already reports two operator-level diagnostics (the `mod=` §6.1
// line plus the §3.5 operand line) for ANY right operand, e.g. `c mod= 2`;
// that pre-dates #5211 and is not pinned here, so its expected count is -1.
static void expectOnlyOperatorRejection(const char* moduleName,
                                        const char* op, int expectedErrors) {
    std::string src =
        std::string("module ") + moduleName + "; "
        "class C "
        "  public static method Main(): int "
        "    var c: char := 'a'; "
        "    var u: uint := uint(3); "
        "    c " + op + " u; "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`char`"));
    BEZEL_EXPECT_TRUE(!diagnosticMessageContains(r, "typed as in"));
    BEZEL_EXPECT_TRUE(!diagnosticMessageContains(r, "right operand"));
    if (expectedErrors >= 0)
        BEZEL_EXPECT_EQ(diagnosticMessageCount(r, "is not defined for `char` operands"), expectedErrors);
}

BEZEL_TEST(char_arithmetic_rejected_5054,
           char_times_eq_uint_rhs_single_operator_diagnostic_5150_5211) {
    expectOnlyOperatorRejection("M5150_10A", "*=", 1);
}

BEZEL_TEST(char_arithmetic_rejected_5054,
           char_div_eq_uint_rhs_single_operator_diagnostic_5150_5211) {
    expectOnlyOperatorRejection("M5150_10B", "/=", 1);
}

BEZEL_TEST(char_arithmetic_rejected_5054,
           char_mod_eq_uint_rhs_single_operator_diagnostic_5150_5211) {
    expectOnlyOperatorRejection("M5150_10C", "mod=", -1);
}
