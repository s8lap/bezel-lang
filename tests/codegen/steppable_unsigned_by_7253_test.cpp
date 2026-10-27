// Regression tests for #7253 — an UNSIGNED `by` at `ISteppable<T>.Step`'s
// `long` parameter was SIGN-EXTENDED, so the VALUE was destroyed.
//
// WHAT THE DOCUMENTS REQUIRE.
//
// spec §5.1 "Implicit conversions" (v0.344) lists the widening table:
//
//   | `uint`  | `long`, `ulong`, `float`, `double`, `decimal` |
//
// So an UNSIGNED operand IS admitted at a `long` parameter. §5.1 is a
// conversion table of VALUES: an integer conversion that fits the target
// carries the source's numeric value across. `uint` 4000000000 is a POSITIVE
// number; a conversion that yields -294967296 has not converted the value, it
// has converted the 32-bit BIT PATTERN.
//
// stdlib §2.19 "INumber<T>, ISteppable<T>, IParsable<T>" (v0.344) declares:
//
//   public interface ISteppable<T> where T: ISteppable<T>
//     static method Step(value: T, by: long): owned T;   // advance in T's space
//
// `by` is declared `long` — a SIGNED type — so an unsigned argument is
// CONVERTED at the call, and §5.1's conversion converts VALUES.
//
// spec §11.8 "Static interface members" (v0.344) is the reachability rule:
// `T.Step(...)` inside a generic body dispatches, at instantiation, to the
// conforming type's static member; the primitive numeric tower's conformance
// is compiler-provided.
//
// WHAT THE DEFECT WAS.
//
// The `Step` lowering widened the `by` argument to the `long` width with an
// UNCONDITIONAL `CreateSExt` — a SIGN-extending integer widening. The `uint`
// 4000000000 (bit pattern 0xEE6B2800) was read as the `int32` -294967296, so
// the `by` argument arrived as a NEGATIVE number:
//
//   T.Step(1.0, 4000000000u)   printed -294967295   (should be 4000000001)
//   T.Step(1.0, 4000000000L)   printed  4000000001   (the control)
//
// The two spellings of the same number disagreed.
//
// THE FIX.
//
// The widening is now SIGNEDNESS-AWARE: an unsigned source ZERO-extends
// (`zext`, which preserves the value), a signed source keeps the `sext` it
// always had (so a negative `by` keeps its sign). The signedness is read with
// `isUnsignedTargetExpr` — the house predicate the #7242 `double`-parameter
// converter and the #7250 mixed-operator fix use — so the three sites cannot
// disagree. It is NOT inferred from the bit width: `int` and `uint` are both
// 32 bits, `long` and `ulong` both 64.
//
// THE MATRIX.
//
// `Step`'s `value: T` is GENERIC, so `T` may be `double`, `decimal`, `float`
// or an integer type (the issue's note: "the fix must not assume `T` is a
// floating type ... Check all three"). The rows below cover a floating `T`, a
// `decimal` `T` and integer `T`s.
//
// THE PROOF HIERARCHY.
//
// Every PIN row below FAILS pre-fix: the program printed a wrong value, so it
// never produced the value §5.1's widening implies. Each pin row therefore
// pins the VALUE, not merely "it compiled". The CONTROL rows are green on
// both arms and show this change is a widening of the unsigned path only.
//
// The rows are also the issue's own claim, made testable: "the `long` spelling
// is right, so the two spellings of the same number disagree" — the
// `two_spellings_agree` row runs BOTH spellings in ONE program and requires
// them to print the same value.
//
// THE SCOPE OF THE CHANGE.
//
// The `by` WIDENING is what this file pins. Two neighbouring behaviours are
// deliberately NOT pinned here, because neither is the sign-extension this
// issue is about:
//
//   * an integer `T` whose advance leaves `T`'s range (`T.Step(1, 4000000000L)`
//     over `T = int`): the result does not fit `T`, and the documents do not
//     settle what a `Step` CALL returns in that case (spec §6.1's checked
//     integer arithmetic vs spec §16.3's silent, never-wrapping advance).
//     That is a separate question about the RESULT type, not about the
//     `by` argument's value.
//
//   * a generic method's own `long` parameter: the generic specialization
//     gives that parameter the ARGUMENT's width, so an unsigned argument
//     reaches the body unconverted. That is the generic-call argument
//     coercion, a different site from `Step`'s `by`.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

using bezel::test::diagnosticMessageContains;

namespace {

// The generic helper whose BODY performs the `ISteppable<T>.Step` call — the
// shape that reaches the compiler-provided primitive conformance (spec §11.8,
// stdlib §2.19).
//
//   params  — the helper's FULL parameter list, e.g. "v: T" or "by: uint, v: T"
//   byExpr  — the `by` argument written at the `T.Step(v, ...)` call
//   mainBody— the statements of `Main` (the helper's call site)
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

// The same helper, called with the given arguments.
std::string stepCall(const std::string& params, const std::string& byExpr,
                     const std::string& args) {
    return stepProg(params, byExpr,
                    "  Console.WriteLine(F.Nxt(" + args + ").ToString()); ");
}

// The accepting shape: the program RAN, it produced exactly this text, and it
// did not fail the compiler.
void expectValue(const std::string& program, const std::string& expected) {
    auto r = runAot(program);
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
// THE PIN ROW — the `uint` spelling of the number, at a floating `T`.
//
// stdlib §2.19's `Step(value: T, by: long)`: the `by` argument is converted
// to `long`, and §5.1's conversion converts the VALUE. `1.0 + 4000000000` is
// 4000000001 — exactly what the `long` spelling prints.
// ===========================================================================
BEZEL_TEST(steppable_unsigned_by_7253, step_double_uint_by_keeps_its_value_5_1) {
    expectValue(stepCall("v: T", "4000000000u", "1.0"), "4000000001");
}

// ===========================================================================
// THE ISSUE'S OWN CLAIM — the two spellings of the same number must AGREE.
//
// Both helpers run in ONE program, so the row pins the disagreement itself,
// not just one spelling's value.
// ===========================================================================
BEZEL_TEST(steppable_unsigned_by_7253, two_spellings_agree_5_1) {
    expectValue(
        "module M; static class F "
        "  public static method ByUint<T>(v: T): T where T: ISteppable<T> "
        "    return T.Step(v, 4000000000u); "
        "  end ByUint; "
        "  public static method ByLong<T>(v: T): T where T: ISteppable<T> "
        "    return T.Step(v, 4000000000L); "
        "  end ByLong; "
        "end F; "
        "class C public static method Main(): int "
        "  Console.WriteLine(F.ByUint(1.0).ToString()); "
        "  Console.WriteLine(F.ByLong(1.0).ToString()); "
        "  return 0; "
        "end Main; end C;",
        "4000000001\n4000000001");
}

// ===========================================================================
// THE `by` AS A PARAMETER — the shape whose IR shows the widening
// instruction (a literal folds into a constant, a parameter does not).
// ===========================================================================
BEZEL_TEST(steppable_unsigned_by_7253,
           step_double_uint_parameter_by_keeps_its_value_5_1) {
    expectValue(stepCall("v: T, by: uint", "by", "1.0, 4000000000u"),
                "4000000001");
}

// The same, with the `by` a `uint` VARIABLE — a non-constant source.
BEZEL_TEST(steppable_unsigned_by_7253,
           step_double_uint_variable_by_keeps_its_value_5_1) {
    expectValue(stepProg("v: T, by: uint", "by",
                         "  var u: uint := 4000000000u; "
                         "  Console.WriteLine(F.Nxt(1.0, u).ToString()); "),
                "4000000001");
}

// The `by` on the LEFT of the helper's parameter list — the same conversion,
// reached with the parameters in the other order.
BEZEL_TEST(steppable_unsigned_by_7253,
           step_double_uint_by_first_parameter_keeps_its_value_5_1) {
    expectValue(stepCall("by: uint, v: T", "by", "4000000000u, 1.0"),
                "4000000001");
}

// ===========================================================================
// THE `T` MATRIX — `T` is generic, so it may be `double`, `decimal`, `float`
// or an integer type (the issue's note: "Check all three").
// ===========================================================================

// A `decimal` `T`. stdlib §2.19's `Step` advances in T's space; the decimal
// path promotes `by` and adds through the runtime (§6.4 semantics).
BEZEL_TEST(steppable_unsigned_by_7253,
           step_decimal_uint_by_keeps_its_value_5_1) {
    expectValue(stepCall("v: T", "4000000000u", "1.0m"), "4000000001");
}

// A `float` `T` — §5.1 allows a `float` to lose precision, but never to
// overflow, so the value is 4e+09 (4000000001 is not representable in a
// `float`'s 24-bit significand).
BEZEL_TEST(steppable_unsigned_by_7253,
           step_float_uint_by_keeps_its_value_5_1) {
    expectValue(stepCall("v: T", "4000000000u", "1.0f"), "4e+09");
}

// A `long` `T` — the result 4000000001 FITS `long`, so this row is free of
// the integer-`T` overflow question and pins the unsigned `by` alone.
BEZEL_TEST(steppable_unsigned_by_7253, step_long_uint_by_keeps_its_value_5_1) {
    expectValue(stepCall("v: T", "4000000000u", "1L"), "4000000001");
}

// An `ulong` `T` — the sibling of the `long` row.
BEZEL_TEST(steppable_unsigned_by_7253,
           step_ulong_uint_by_keeps_its_value_5_1) {
    expectValue(stepCall("v: T", "4000000000u", "1UL"), "4000000001");
}

// ===========================================================================
// THE CONTROLS — green on BOTH arms.
//
// A signed `by` must convert exactly as it did before the fix, and a NEGATIVE
// `by` must keep its sign. The negative rows are the single most likely way
// this fix could have gone wrong (a fix that zero-extends indiscriminately
// would turn -4000000000 into a huge positive number).
// ===========================================================================
BEZEL_TEST(steppable_unsigned_by_7253, step_double_long_by_is_unchanged_5_1) {
    expectValue(stepCall("v: T", "4000000000L", "1.0"), "4000000001");
}

BEZEL_TEST(steppable_unsigned_by_7253,
           step_double_negative_long_by_keeps_its_sign_5_1) {
    expectValue(stepCall("v: T", "-4000000000L", "1.0"), "-3999999999");
}

// The same negative `by`, arriving as a `long` PARAMETER.
BEZEL_TEST(steppable_unsigned_by_7253,
           step_double_negative_long_parameter_keeps_its_sign_5_1) {
    expectValue(stepCall("v: T, by: long", "by", "1.0, -4000000000L"),
                "-3999999999");
}

BEZEL_TEST(steppable_unsigned_by_7253,
           step_decimal_long_by_is_unchanged_5_1) {
    expectValue(stepCall("v: T", "4000000000L", "1.0m"), "4000000001");
}

BEZEL_TEST(steppable_unsigned_by_7253, step_long_long_by_is_unchanged_5_1) {
    expectValue(stepCall("v: T", "4000000000L", "1L"), "4000000001");
}

// An `int` `T` with an IN-RANGE `by` — the integer path, with no overflow
// question in it. The `uint` spelling must give the same answer as the `long`
// spelling: both fit `int`.
BEZEL_TEST(steppable_unsigned_by_7253,
           step_int_small_long_and_uint_by_agree_5_1) {
    expectValue(
        "module M; static class F "
        "  public static method ByUint<T>(v: T): T where T: ISteppable<T> "
        "    return T.Step(v, 2u); "
        "  end ByUint; "
        "  public static method ByLong<T>(v: T): T where T: ISteppable<T> "
        "    return T.Step(v, 2L); "
        "  end ByLong; "
        "end F; "
        "class C public static method Main(): int "
        "  Console.WriteLine(F.ByUint(1).ToString()); "
        "  Console.WriteLine(F.ByLong(1).ToString()); "
        "  return 0; "
        "end Main; end C;",
        "3\n3");
}

// A `char` `T` — `char` conforms to `ISteppable<char>` but not to `INumber`
// (stdlib §2.19), and the advance is a code-point advance.
BEZEL_TEST(steppable_unsigned_by_7253, step_char_by_is_unchanged_5_1) {
    expectValue(stepCall("v: T", "2L", "'a'"), "c");
}

// A `byte` `T` — `byte` is UNSIGNED, so this row also pins that the unsigned
// source is zero-extended on the way into the `long` (the value 200 is the
// same either way, so this row is green on both arms).
BEZEL_TEST(steppable_unsigned_by_7253, step_byte_uint_by_is_unchanged_5_1) {
    expectValue(stepCall("v: T", "200u", "byte(0)"), "200");
}

// The narrow-unsigned sources at one `uint` parameter: `byte`, `ushort` and
// `uint` must all reach the `Step` call with their VALUE.
BEZEL_TEST(steppable_unsigned_by_7253,
           step_double_narrow_unsigned_by_matrix_5_1) {
    expectValue(stepProg("v: T, by: uint", "by",
                         "  var b: byte := 200; "
                         "  var s: ushort := 60000; "
                         "  var u: uint := 4000000000u; "
                         "  Console.WriteLine(F.Nxt(1.0, b).ToString()); "
                         "  Console.WriteLine(F.Nxt(1.0, s).ToString()); "
                         "  Console.WriteLine(F.Nxt(1.0, u).ToString()); "),
                "201\n60001\n4000000001");
}

// ===========================================================================
// THE ISSUE'S OWN PRINTED REPRO — a NON-generic `by: long` parameter.
//
// The issue's code block is a plain static method with a `by: long`
// parameter, and it already produced the right value on this base (the
// ordinary call path converts the argument with `coerceNumericToType`, which
// is unsigned-aware). This row keeps it that way and records the fact.
// ===========================================================================
BEZEL_TEST(steppable_unsigned_by_7253,
           plain_long_parameter_uint_argument_is_value_preserving_5_1) {
    expectValue(
        "module M; static class F "
        "  public static method Nxt(x: double, by: long): double "
        "    return x + by; "
        "  end Nxt; "
        "end F; "
        "class C public static method Main(): int "
        "  Console.WriteLine(F.Nxt(0.0, 4000000000u).ToString()); "
        "  Console.WriteLine(F.Nxt(0.0, 4000000000L).ToString()); "
        "  return 0; "
        "end Main; end C;",
        "4e+09\n4e+09");
}
