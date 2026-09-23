// fix #4405 / §5.2 / §17.14: the explicit-conversion constant-operand rule.
//
// THE RULE (§5.2, v0.300 ruling on #4405; spec text cited below):
//
//   "Constant operands are decided at compile time: where the operand of an
//    explicit conversion is a compile-time constant (§17.14) whose value
//    would raise `OverflowException` under any rule of this section —
//    integer narrowing, the same-width signedness change, the integer-to-
//    `char` code-point range, and the floating-point and `decimal`
//    conversions — the conversion is a **compile error at the conversion
//    site**, never a deferred runtime fault: `uint(-1)`, `byte(300)`,
//    `char(0xD800)`, and `int(1e30)` are rejected where they are written."
//
// This matches the posture §5.1 (constant narrowing), §5.4 (constant enum
// cast), §6.5 (constant shift count) and §17.14 (constant-evaluation faults)
// already take. The carve-outs the rule itself names are tested too:
//   - a SATURATING conversion is unaffected — `float(1e300)` is `Infinity`
//     and `float(0.0/0.0)` is `NaN` at a constant operand exactly as at
//     runtime; integer narrowing has no analogous saturation (it always
//     raises), so integer→integer constant rejection is the strict rule.
//   - the rule reads the OPERAND only — it does not make the conversion
//     expression a constant expression (§17.14's inductive set is
//     unchanged, so `const K := byte(1);` is still not a compile-time
//     `const`).
//   - a NON-CONSTANT operand keeps §5.2's literal text and raises
//     `OverflowException` at runtime — `byte(n)` where `n` is a local var
//     stays admitted at sema and faults at runtime.
//
// PRE-FIX BEHAVIOUR. The §5.2 primitive-conversion arm in
// ExprTypecheck_Calls.cpp (just below the bool↔numeric check at lines
// 2475–2532) typechecked `int(x)` for any primitive `x` and let codegen
// lower the constant through its checked-arithmetic path, so `uint(-1)`,
// `byte(300)` and `char(0xD800)` COMPILED CLEAN and faulted at runtime —
// the only checked operation in the language deferring a decidable fault.
// The fix runs `foldIntConstExprTC` on the operand, then checks the folded
// value against the target's range (the same `fitsInIntType` table
// ExprTypecheck.cpp uses for the §5.1 narrowing gate) and against §5.2's
// integer-to-`char` code-point range (U+0000–U+10FFFF excluding
// U+D800–U+DFFF). Diagnostics carry the word "constant" so a literal
// case / corpus grep can pin the rejection.
//
// FLOATING/DECIMAL LITERAL FOLDING is intentionally NOT covered here. The
// spec text names `int(1e30)` as a compile error, but `foldIntConstExprTC`
// folds integer constants only — folding a `FloatLiteralExpr` would need a
// parallel `foldFloatConstExprTC` helper that parses the text into a
// `double`. Filed as a follow-up issue alongside this fix; the corpus
// rows that exercise this fix (s5_2_0031, s5_2_0032) are integer-only.
//
// Authority: spec §5.2 / §17.14 (v0.300, ruling on #4405); CLI §6.5 / §17.14.

#include <string_view>

#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndTypecheckGated;
using bezel::test::runAot;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff a diagnostic carries the §5.2/§17.14 constant-operand word
// AND names `OverflowException` — the two brand-new markers this fix
// emits. Their joint presence pins the rejection to the new arm; a
// pre-fix silent accept has neither.
bool hasConstantOverflowDiag(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("constant") != std::string::npos &&
            d.message.find("OverflowException") != std::string::npos)
            return true;
    return false;
}

// True iff a diagnostic carries the integer-to-`char` variant — the
// (b) arm of the fix, which uses the code-point-range wording rather than
// the integer-narrowing wording.
bool hasCharCodePointDiag(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("constant") != std::string::npos &&
            d.message.find("Unicode code point") != std::string::npos)
            return true;
    return false;
}

const char* kHeader =
    "module M; class C public static method Main(): int ";
const char* kFooter = " return 0; end Main; end C;";

bezel::test::SemaResult check(const std::string& body) {
    return parseSemaAll(std::string(kHeader) + body + kFooter);
}

}  // namespace

// =====================================================================
// REJECT — §5.2 compile-error arm at a literal integer constant. Each
// row corresponds to a spelled-out example from the spec text.
// =====================================================================

// #4405 (s5_2_0031): the spec's own first example — `uint(-1)` is a
// same-width signedness change, the only form §5.2 names for `uint`.
// Pre-fix this COMPILED CLEAN and faulted at runtime.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           uint_negative_one_rejected_at_conversion_site) {
    auto r = check("var u: uint := uint(-1); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// #4405 (s5_2_0032): the §17.14 posture — a faulting constant operand is
// rejected WHERE IT IS WRITTEN, not where it is reached. `if false then
// … uint(-1) …` is unreachable code, and the rejection fires anyway.
// Pre-fix the constant path lowered to a runtime check that never ran.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           uint_negative_one_rejected_in_unreachable_code) {
    auto r = check(
        "if false then "
        "  var u: uint := uint(-1); "
        "  Console.WriteLine(u.ToString()); "
        "end; "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// #4405: the spec's second example — `byte(300)` is an integer narrowing
// at a literal constant; §5.1 already rejects this implicitly, and §5.2
// now does too at the explicit site.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           byte_three_hundred_rejected_at_conversion_site) {
    auto r = check("var b: byte := byte(300); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// #4405: same narrowing, signed target. `short(70000)` overflows short's
// 16-bit range; pre-fix it lowered through codegen's checked narrow.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           short_seventy_thousand_rejected_at_conversion_site) {
    auto r = check("var s: short := short(70000); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// #4405: the same-width signedness change from the unsigned side —
// `int(uint.MaxValue)` is 4294967295, which does not fit in `int`'s
// [-2147483648, 2147483647]. Same arm, opposite direction. The
// operand is a `const` (§17.14 — a local const read is a constant
// expression); a `var` read at the same site is the runtime path
// (`uint_of_local_var_admitted_at_sema` below covers the carve-out).
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           int_of_uint_max_value_rejected_at_conversion_site) {
    auto r = check(
        "const UMAX: uint := 4294967295U; "
        "var i: int := int(UMAX); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// #4405: the spec's third example — `char(0xD800)` is the leading
// surrogate code point; §5.2's integer-to-`char` rule rejects it.
// Pre-fix the surrogate rejection was a runtime `OverflowException`; now
// it is a compile error AT THE CONVERSION SITE.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           char_surrogate_rejected_at_conversion_site) {
    auto r = check("var c: char := char(0xD800); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasCharCodePointDiag(r));
}

// #4405: trailing surrogate too — `char(0xDFFF)` sits at the end of the
// surrogate block, same rejection, same message.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           char_trailing_surrogate_rejected_at_conversion_site) {
    auto r = check("var c: char := char(0xDFFF); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasCharCodePointDiag(r));
}

// #4405: a NEGATIVE integer at a `char` target — `char(-1)` is below
// U+0000; §5.2 rejects with the same code-point-range wording.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           char_negative_rejected_at_conversion_site) {
    auto r = check("var c: char := char(-1); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasCharCodePointDiag(r));
}

// #4405: a code point PAST U+10FFFF — `char(0x110000)` is the smallest
// value §5.2 rejects above the surrogate block.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           char_above_max_code_point_rejected_at_conversion_site) {
    auto r = check("var c: char := char(0x110000); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasCharCodePointDiag(r));
}

// =====================================================================
// ACCEPT — the carve-outs and the negative-control shapes the rule
// leaves alone. A false REJECT here is the over-rejection hazard
// (rule reads the operand; runtime path is unchanged; saturating
// conversions are unaffected; a non-constant operand keeps §5.2's
// literal text).
// =====================================================================

// #4405 (s5_2_0033, positive control): a constant operand that FITS in
// the target type is admitted — `byte(200)` is well-typed and converts
// at compile time. Same §5.2 call, the constant narrows in-range.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           byte_two_hundred_constant_accepted) {
    auto r = check("var b: byte := byte(200); return (if b = 200 then 0 else 1);");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #4405: a positive signed-byte fit — `sbyte(127)` is in range, well-typed.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           sbyte_max_constant_accepted) {
    auto r = check("var b: sbyte := sbyte(127); return (if b = 127 then 0 else 1);");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #4405: same-type widening is not a narrowing — `long(1)` is admitted
// even though Long's range check is the same `fitsInIntType` table (the
// default-arm `return true` at Long covers widening).
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           long_literal_accepted) {
    auto r = check("var l: long := long(1); return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #4405: the same-type widening on the unsigned side — `ulong(0)` and
// `ulong(1)` are well-typed at compile time.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           ulong_literal_accepted) {
    auto r = check("var u: ulong := ulong(1); return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #4405: the largest legal code point — `char(0x10FFFF)` is at the top
// of §5.2's code-point range and is admitted.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           char_max_code_point_constant_accepted) {
    auto r = check("var c: char := char(0x10FFFF); return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #4405: the largest non-surrogate code point at the bottom of the
// range — `char(0xD7FF)` is one below the surrogate block and admitted.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           char_just_below_surrogate_block_accepted) {
    auto r = check("var c: char := char(0xD7FF); return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #4405: the first code point past the surrogate block — `char(0xE000)`
// is the first non-surrogate, non-private-use code point.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           char_just_above_surrogate_block_accepted) {
    auto r = check("var c: char := char(0xE000); return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #4405: a NON-CONSTANT operand keeps §5.2's literal text and the
// runtime path. `byte(n)` where `n` is a local var is admitted at sema
// and faults at runtime if `n` happens to overflow byte's range. This
// is the corpus case s5_2_0002's shape (var n := 70000; short(n)).
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           byte_of_local_var_admitted_at_sema) {
    auto r = check(
        "var n: int := 70000; "
        "var b: byte := byte(n); "
        "return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #4405: the same carve-out for `char` — `char(n)` where n is a var is
// admitted at sema; the runtime `OverflowException` for a surrogate
// operand is the corpus case s5_2_0008's shape.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           char_of_local_var_admitted_at_sema) {
    auto r = check(
        "var n: int := 0xD800; "
        "var c: char := char(n); "
        "return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #4405: the same carve-out for `uint` — `uint(n)` where n is a var is
// admitted at sema (corpus case s5_2_0004's shape).
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           uint_of_local_var_admitted_at_sema) {
    auto r = check(
        "var n: int := -1; "
        "var u: uint := uint(n); "
        "return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #4405: a constant bool source — `int(true)` was admitted by the
// pre-fix compiler (#1322 ruling) and stays admitted here. The new
// constant-operand arm keys on the source being an INTEGER, so the bool
// direction is unchanged.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           int_of_bool_true_still_admitted) {
    auto r = check(
        "var b: bool := true; "
        "var n: int := int(b); "
        "return (if n = 1 then 0 else 1);");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #4405: a saturating conversion at a constant operand — `float(1e300)`
// is `Infinity` at runtime, and §5.2's carve-out says "a conversion that
// saturates rather than raises is unaffected". The fix's filter excludes
// float targets (saturating), so this stays admitted.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           float_one_e_three_hundred_saturates_accepted) {
    auto r = check("var f: float := float(1e30); return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #4405: int-to-float — `float(1)` is exact, no overflow, no precision
// loss at the i32→f32 width.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           float_of_int_literal_accepted) {
    auto r = check("var f: float := float(1); return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #4405: the negative-control for the (a) arm — `sbyte(-128)` is at the
// bottom of sbyte's range and is admitted. Pinned to the same-width
// signedness-change wording the rejection uses.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           sbyte_min_constant_accepted) {
    auto r = check("var b: sbyte := sbyte(-128); return (if b = -128 then 0 else 1);");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =====================================================================
// AOT WITNESSES — fork-isolated compile + link + native exec, the strict
// oracle. The sema-only accept-controls above are necessary but not
// sufficient — a fix that keeps sema happy but breaks codegen would
// pass them and fail these.
// =====================================================================

// #4405 (s5_2_0031): the issue's own program, end-to-end. Pre-fix this
// compiled, linked, and faulted at runtime; post-fix it must fail at
// compile time.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           uint_negative_one_rejected_end_to_end) {
    auto r = runAot(
        "module Conf; "
        "public class Program "
        "  public static method Run(): int "
        "    var u: uint := uint(-1); "
        "    return 0; "
        "  end Run; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// #4405 (s5_2_0032): the unreachable-code posture, end-to-end.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           uint_negative_one_rejected_in_unreachable_code_end_to_end) {
    auto r = runAot(
        "module Conf; "
        "public class Program "
        "  public static method Run(): int "
        "    if false then "
        "      var u := uint(-1); "
        "      Console.WriteLine(u.ToString()); "
        "    end; "
        "    return 0; "
        "  end Run; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// #4405 (s5_2_0033): the positive control, end-to-end. `byte(200)`
// must compile AND produce the right runtime answer (0 = correct). A
// sema-only accept-control cannot catch a fix that keeps sema happy but
// breaks the constant-narrowing lowering.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           byte_two_hundred_runs_and_produces_200_end_to_end) {
    auto r = runAot(
        "module Conf; "
        "public class Program "
        "  public static method Main(): int "
        "    var b: byte := byte(200); "
        "    Console.WriteLine(b.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// #4405: the non-constant shape from s5_2_0002 — `byte(n)` where n is
// a local var. Sema admits it; runtime faults on 70000; the catch
// recovers the overflow.
BEZEL_TEST(s5_2_constant_operand_overflow_4405,
           byte_of_local_var_overflows_at_runtime_end_to_end) {
    auto r = runAot(
        "module Conf; "
        "public class Program "
        "  public static method Main(): int "
        "    try "
        "      var n: int := 70000; "
        "      var b: byte := byte(n); "
        "      Console.WriteLine(\"no overflow\"); "
        "    catch "
        "      var ex: OverflowException => Console.WriteLine(\"overflow\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
