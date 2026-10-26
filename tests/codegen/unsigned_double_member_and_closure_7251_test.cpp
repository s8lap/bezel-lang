// Regression tests for #7251 — an UNSIGNED operand at a `double` PARAMETER
// was SIGN-EXTENDED at two sites, so the VALUE was destroyed.
//
// WHAT THE DOCUMENTS REQUIRE.
//
// spec §5.1 "Implicit conversions" (v0.344) states:
//
//   "A value of one type implicitly converts to another when no programmer-
//    visible information is lost"
//
// and its widening table admits an unsigned integer at a binary-float
// parameter:
//
//   | `byte`   | `short`, `ushort`, `int`, `uint`, `long`, `ulong`, `float`, `double`, `decimal` |
//   | `ushort` | `int`, `uint`, `long`, `ulong`, `float`, `double`, `decimal` |
//   | `uint`   | `long`, `ulong`, `float`, `double`, `decimal` |
//   | `ulong`  | `float`, `double`, `decimal` |
//   | `char`   | `int`, `uint`, `long`, `ulong`, `float`, `double`, `decimal` |
//
// §5.1 is a table of VALUES. An integer conversion that fits the target
// carries the source's NUMERIC value across; it does not reinterpret the
// source's bit pattern. (spec §5.2 makes the same point about the explicit
// form: "Such a cast is a *value* conversion, not a reinterpretation of bits:
// it preserves the numeric value or fails.")
//
// stdlib §9.2 (v0.344) declares, for `Double`:
//
//   public static pure method IeeeEquals(a: double, b: double): bool;    // strict IEEE
//   public static pure method BitwiseEquals(a: double, b: double): bool; // bit-pattern equal
//
// and, for `Float`, the analogous pair. §22.6's applicability stage reads
// the parameter types "under the implicit conversions (§5.1)", so an unsigned
// argument IS admitted at a `double` parameter, and it converts THERE.
//
// WHAT THE DEFECT WAS.
//
// Two sites converted an integer operand with LLVM's `sitofp`, which
// SIGN-EXTENDS the source's bit pattern:
//
//   - `Double`/`Float`.`IeeeEquals` / `BitwiseEquals`
//     (src/codegen/LlvmBackend_Calls2.cpp) — the site converts its two
//     operands separately, and each was `sitofp`'d.
//
//   - a CLOSURE call's argument coercion
//     (src/codegen/LlvmBackend_Calls2.cpp) — an argument coerced to the
//     DECLARED PARAMETER TYPE of the function type.
//
// The `uint` 4000000000 (bit pattern 0xEE6B2800) was read as the `int32`
// -294967296, so:
//
//   Double.IeeeEquals(4000000000u, 4e9)  printed False   (the two are the
//                                                          same number!)
//   f(4000000000u)  /* f: (double) -> double */  printed -294967296
//
// while the `long` spelling of the same number printed True and 4e+09.
//
// WHAT THE FIX DOES.
//
// The integer-to-floating conversion is SIGNEDNESS-AWARE: an unsigned source
// uses `uitofp` (the instruction that preserves the VALUE), a signed source
// keeps the `sitofp` it always had. The signedness is read through the ONE
// house predicate, `isUnsignedTargetExpr` (src/codegen/LlvmBackend_impl.hpp),
// so a signed operand keeps its sign and every signed result is bit-for-bit
// what it was.
//
// THE `BitwiseEquals` QUESTION — AND ITS ANSWER.
//
// `BitwiseEquals` is a BIT-PATTERN comparison, so it is not obvious that the
// same conversion is right there. The answer is that stdlib §9.2 declares
// `BitwiseEquals(a: double, b: double)` — its parameters are `double`. The
// §5.1 VALUE conversion therefore happens FIRST, and the member then compares
// the two `double`s' bit patterns:
//
//   BitwiseEquals(4000000000u, 4e9)
//     §5.1:  the `uint` 4000000000 converts to the `double` 4e9
//     §9.2:  the two `double`s 4e9 and 4e9 have identical bit patterns
//     => True
//
// which is the same answer as `BitwiseEquals(4e9, 4e9)`. The distinctions the
// member draws — `+0.0` from `-0.0`, signaling from quiet NaN payloads — are
// distinctions between the CONVERTED doubles, and the conversion does not
// touch them. So the SAME value-preserving conversion is correct for both
// members, and the rows below pin it.
//
// THE PROOF HIERARCHY.
//
// Every accepting row below that carries an unsigned operand FAILS pre-fix:
// the program printed a wrong value, so it never produced the value §5.1's
// widening implies. Each such row therefore pins the VALUE, not merely "it
// compiled". The control rows are green on both arms and show that this
// change is a widening of the UNSIGNED path and not a change to the signed one
// — and, for the closure, that the ordinary (non-closure) call path was
// already correct, so the closure path was the odd one out.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

// The program under test. `extraMembers` is spliced into the class body (used
// by the control row that shows an ORDINARY call already converted the
// `uint` correctly).
std::string prog(const std::string& body, const std::string& extraMembers) {
    return "module M; import System; "
           "class C " +
           extraMembers +
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

std::string prog(const std::string& body) { return prog(body, ""); }

// The accepting shape: the program RAN, it produced exactly this text, and
// it did not fail the compiler.
void expectValue(const std::string& body, const std::string& expected) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    // The value channel: the program's stdout with ONE trailing line-ending
    // stripped (the same rule `runAotOut` applies), so the expectation is
    // platform-independent.
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();
    }
    BEZEL_EXPECT_EQ(out, expected);
}

}  // namespace

// ===========================================================================
// (a) `Double.IeeeEquals` / `Double.BitwiseEquals` — BOTH operands.
// ===========================================================================

// THE ISSUE'S OWN REPRO, operand `a`. stdlib §9.2 declares
// `IeeeEquals(a: double, b: double): bool`. §5.1 (v0.344) admits the `uint`
// 4000000000 at a `double` parameter and converts it BY VALUE, so the two
// operands are the same number and the answer is `True`. Pre-fix the `uint`
// was `sitofp`'d to -294967296.0 and the member printed `False`.
BEZEL_TEST(unsigned_double_member_and_closure_7251,
           double_ieee_equals_uint_operand_a_is_value_preserving_5_1) {
    expectValue("    Console.WriteLine(Double.IeeeEquals(4000000000u, 4e9));",
                "True");
}

// The SAME member, operand `b`. The site converts its two operands
// SEPARATELY, so a fix that repaired only the first side would leave this
// row wrong.
BEZEL_TEST(unsigned_double_member_and_closure_7251,
           double_ieee_equals_uint_operand_b_is_value_preserving_5_1) {
    expectValue("    Console.WriteLine(Double.IeeeEquals(4e9, 4000000000u));",
                "True");
}

// `BitwiseEquals` — the bit-pattern member. stdlib §9.2 declares its
// parameters as `double`, so §5.1's VALUE conversion happens first and the
// member then compares the two `double`s' bit patterns. The `uint`
// 4000000000 becomes the `double` 4e9, whose bit pattern is the double's,
// so the answer is `True` — the same as `BitwiseEquals(4e9, 4e9)`.
BEZEL_TEST(unsigned_double_member_and_closure_7251,
           double_bitwise_equals_uint_operand_a_is_value_preserving_5_1) {
    expectValue("    Console.WriteLine(Double.BitwiseEquals(4000000000u, 4e9));",
                "True");
}

// The bit-pattern member's SECOND operand — the same "the site converts its
// two operands separately" reason.
BEZEL_TEST(unsigned_double_member_and_closure_7251,
           double_bitwise_equals_uint_operand_b_is_value_preserving_5_1) {
    expectValue("    Console.WriteLine(Double.BitwiseEquals(4e9, 4000000000u));",
                "True");
}

// THE CONTROL: `BitwiseEquals` really does compare BIT PATTERNS, so the
// fix must not have turned it into a numeric comparison. `IeeeEquals(NaN,
// NaN)` is `False` (stdlib §9.2: "returns `false` for any pair involving
// `NaN`, including `NaN`-vs-`NaN`") while `BitwiseEquals(NaN, NaN)` is
// `True` (the two NaNs have the same bit pattern). This row is green on
// BOTH arms — it pins the member's identity, not the fix.
BEZEL_TEST(unsigned_double_member_and_closure_7251,
           double_bitwise_equals_still_compares_bit_patterns_9_2) {
    expectValue("    Console.WriteLine(Double.IeeeEquals(Double.NaN, Double.NaN)); "
                "    Console.WriteLine(Double.BitwiseEquals(Double.NaN, Double.NaN));",
                "False\nTrue");
}

// THE CONTROL: the `uint` arrives through a BINDING, not as a literal, so
// the row exercises the value as it is stored in its `uint` slot rather than
// a constant the compiler may fold. §5.1 still converts it by value.
BEZEL_TEST(unsigned_double_member_and_closure_7251,
           double_ieee_equals_uint_binding_is_value_preserving_5_1) {
    expectValue("    var u: uint := 4000000000u; "
                "    Console.WriteLine(Double.IeeeEquals(u, 4e9));",
                "True");
}

// THE `ulong` PATH — the `uint` case alone does not prove it. The value is
// BEYOND `int64`, so its bit pattern read as a signed integer is a
// different wrong number (-1 rather than -294967296). The second operand
// is the explicit `double(...)` cast, which §5.2 makes a value conversion
// and which the compiler already performs unsigned-aware, so the row asks
// the MEMBER's conversion to agree with the cast.
BEZEL_TEST(unsigned_double_member_and_closure_7251,
           double_ieee_equals_ulong_beyond_int64_agrees_with_the_cast_5_1) {
    expectValue("    var ul: ulong := 18446744073709551615UL; "
                "    Console.WriteLine(Double.IeeeEquals(ul, double(ul)));",
                "True");
}

// The same `ulong` value at the bit-pattern member.
BEZEL_TEST(unsigned_double_member_and_closure_7251,
           double_bitwise_equals_ulong_beyond_int64_agrees_with_the_cast_5_1) {
    expectValue("    var ul: ulong := 18446744073709551615UL; "
                "    Console.WriteLine(Double.BitwiseEquals(ul, double(ul)));",
                "True");
}

// THE NARROWER UNSIGNED WIDTHS — `byte` and `ushort` are in §5.1's table
// too, and their pre-fix `sitofp` was wrong in the same way (the `byte`
// 200 became -56, the `ushort` 60000 became -5536).
BEZEL_TEST(unsigned_double_member_and_closure_7251,
           double_ieee_equals_byte_and_ushort_are_value_preserving_5_1) {
    expectValue("    var b: byte := 200; "
                "    var s: ushort := 60000; "
                "    Console.WriteLine(Double.IeeeEquals(b, 200.0)); "
                "    Console.WriteLine(Double.IeeeEquals(s, 60000.0));",
                "True\nTrue");
}

// THE SIGNED CONTROLS — these are green on BOTH arms. A signed `int`/`long`
// operand keeps the `sitofp` it always had, and a NEGATIVE signed operand
// keeps its sign.
BEZEL_TEST(unsigned_double_member_and_closure_7251,
           double_ieee_equals_signed_operands_are_unchanged_5_1) {
    expectValue("    Console.WriteLine(Double.IeeeEquals(-3.0, -3.0)); "
                "    Console.WriteLine(Double.IeeeEquals(-3, -3.0)); "
                "    Console.WriteLine(Double.IeeeEquals(-3L, -3.0)); "
                "    Console.WriteLine(Double.BitwiseEquals(-3.0, -3.0));",
                "True\nTrue\nTrue\nTrue");
}

// ===========================================================================
// (b) A CLOSURE CALL's argument coercion — a DIFFERENT mechanism.
// ===========================================================================

// THE ISSUE'S OWN REPRO. The argument is coerced to the DECLARED PARAMETER
// TYPE of the function type (§27.6), which is `double`; the `uint`
// 4000000000 must convert by VALUE (§5.1), so the callee must receive
// 4e+09. Pre-fix it received -294967296.0 and the callee printed
// -294967296.
//
// The value is also checked as a NUMBER (not only as text): `f(u) =
// 4000000000.0` is the reflexive equality of spec §4.3.1.
BEZEL_TEST(unsigned_double_member_and_closure_7251,
           closure_uint_argument_is_value_preserving_5_1) {
    expectValue("    var f: (double) -> double := lambda(x: double): double => x; "
                "    var u: uint := 4000000000u; "
                "    Console.WriteLine(f(u)); "
                "    Console.WriteLine(f(u) = 4000000000.0);",
                "4e+09\nTrue");
}

// The same coercion with the `uint` arriving as a LITERAL at the call site.
BEZEL_TEST(unsigned_double_member_and_closure_7251,
           closure_uint_literal_argument_is_value_preserving_5_1) {
    expectValue("    var f: (double) -> double := lambda(x: double): double => x; "
                "    Console.WriteLine(f(4000000000u));",
                "4e+09");
}

// The `ulong` path at the closure call. The value is BEYOND `int64`, so its
// bit pattern read as a signed integer is -1 — a different wrong number from
// the `uint`'s -294967296. The second line compares the received value with
// the explicit `double(...)` cast, which §5.2 makes a value conversion and
// which the compiler already performs unsigned-aware, so the row asks the
// CLOSURE's coercion to agree with the cast.
BEZEL_TEST(unsigned_double_member_and_closure_7251,
           closure_ulong_beyond_int64_agrees_with_the_cast_5_1) {
    expectValue("    var f: (double) -> double := lambda(x: double): double => x; "
                "    var ul: ulong := 18446744073709551615UL; "
                "    Console.WriteLine(f(ul)); "
                "    Console.WriteLine(f(ul) = double(ul));",
                "18446744073709552000\nTrue");
}

// THE CONTROL for the `ulong` path: a `ulong` BELOW `int64` has the same
// bit pattern read either way, so the pre-fix `sitofp` happened to be right
// for it. This row is green on BOTH arms and shows the fix does not disturb
// that case.
BEZEL_TEST(unsigned_double_member_and_closure_7251,
           closure_ulong_below_int64_is_unchanged_5_1) {
    expectValue("    var f: (double) -> double := lambda(x: double): double => x; "
                "    var ul: ulong := 4000000000UL; "
                "    Console.WriteLine(f(ul));",
                "4e+09");
}

// THE SIGNED CONTROLS at the closure call — green on BOTH arms. A signed
// operand keeps the `sitofp` it always had, and a NEGATIVE signed argument
// keeps its sign: `f(-3)` is -3.0.
BEZEL_TEST(unsigned_double_member_and_closure_7251,
           closure_signed_arguments_are_unchanged_5_1) {
    expectValue("    var f: (double) -> double := lambda(x: double): double => x; "
                "    Console.WriteLine(f(-3)); "
                "    Console.WriteLine(f(4000000000L)); "
                "    Console.WriteLine(f(4e9));",
                "-3\n4e+09\n4e+09");
}

// THE CONTROL THAT IDENTIFIES THE MECHANISM: an ORDINARY (non-closure) call
// with the same `double` parameter already converted the `uint` correctly
// (the shared numeric coercion is unsigned-aware), so the closure path was
// the odd one out. This row is green on BOTH arms.
BEZEL_TEST(unsigned_double_member_and_closure_7251,
           ordinary_call_uint_argument_is_already_correct_5_1) {
    auto r = runAot(prog("    Console.WriteLine(C.M(4000000000u));",
                         "  public static method M(x: double): double => x; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();
    }
    BEZEL_EXPECT_EQ(out, "4e+09");
}
