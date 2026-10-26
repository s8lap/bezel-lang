// Spec-derived regression test for issue #7286 (MIRROR half) — the radix
// class of `Convert.ToString(value, toBase)` is NOT closed.
//
// ─── The normative sentences (CHECKED in docs/, v0.344) ───────────────────────
//
// stdlib §2.3 (v0.344), the `Convert` class listing:
//   "public static method ToString(value: int, toBase: int): string;"
//   "public static method ToString(value: long, toBase: int): string;"
//   and: "`ToString(value, toBase)` returns the text `value.ToString(toBase)`
//   returns (§9.1)".
//
// stdlib §9.1 (v0.344), "The radix alphabet":
//   "`ToString(radix)` emits the **lower-case** digits — a `-` for a negative
//   value followed by the magnitude's digits most significant first, a single
//   `0` for zero, never a leading zero otherwise — and takes the same radix
//   set, raising `ArgumentException` for any other. The two are therefore
//   inverse: `T.Parse(n.ToString(radix), radix)` is `n` for every value of
//   every integer type and every admitted radix. `Convert.ToString(value,
//   toBase)` (§2.3) emits the same text as `ToString(radix)`."
//
// spec §5.1 (v0.344), "Nullable conversions":
//   "For any type `T`, `T` implicitly converts to `T?` … Each implicit
//   conversion of this section also **lifts** to the nullable forms: `S?`
//   implicitly converts to `T?` whenever `S` implicitly converts to `T`".
//   => the lifting is ONE-WAY: an `int?` converts to NO non-nullable `int`.
//
// spec §22.6 (v0.344), "Implicit conversions and tie-breaking":
//   "The argument's static type is its type at the call: a read of a binding
//   §7.4 has narrowed non-null is of its non-nullable type, as §7.11 reads it
//   …, so a narrowed `d: Dog?` is exact at a `Dog` parameter and converts to
//   an `Animal` or a `Dog?` one".
//   and: "If no candidate is applicable at all — no candidate is reachable: a
//   conversion §5.1 does not admit as implicit, … the call is a compile error
//   with no candidates cited, because no overload was reachable."
//
// spec §7.4 (v0.344), "Member access on nullable values":
//   "Narrowing also applies after early return, after `is not null`, …" and
//   "a null test in either spelling — `x <> null` or `x is not null`
//   (narrowing the true branch)".
//
// ─── WHAT THE DEFECT WAS (at the MIRROR site) ────────────────────────────────
//
// The mirror of the primitive site `(255).ToString(radix)` is
// `Convert.ToString(255, radix)`. Its CHECK is in sema
// (`ExprTC::checkBuiltinMathConvertCallArgs`, src/sema/ExprTypecheck_Calls.cpp)
// and its RENDERER is in codegen (src/codegen/LlvmBackend_Calls.cpp).
//
// (1) B1 — a NULLABLE radix was ADMITTED and rendered a SILENTLY WRONG value.
//     `var r: int? := 16; Convert.ToString(255, r)` compiled and printed
//     "255" — the DECIMAL text of the value, not its radix-16 text "ff". The
//     §7 nullability gate (#6782) has NO row for `Convert.ToString`
//     (`common::builtinParamIsNonNullable` carries rows for the
//     `Convert.To*` / `TryTo*` members at position 0 and for `Int32.Parse` /
//     `TryParse`, but never for `Convert.ToString`), so the gate never
//     claimed the position, and the probe SKIPPED every nullable argument.
//
// (2) C4 — a RUNTIME `int` radix was ADMITTED and rendered the DECIMAL text.
//     `var r: int := 16; Convert.ToString(255, r)` compiled and printed "255".
//     The renderer chose the digit format from a COMPILE-TIME CONSTANT
//     (`llvm::dyn_cast<llvm::ConstantInt>(radix)`), so a radix that is not a
//     constant always took the decimal fallback.
//
// (3) B2 — a FOLDABLE constant expression was ADMITTED and rendered the
//     DECIMAL text: `Convert.ToString(255, 8u + 8u)` compiled and printed
//     "255" (the value is 16, so the text is "ff").
//
// ─── THE RULE THIS TEST PINS ─────────────────────────────────────────────────
//
// | the radix argument                    | the verdict                    |
// |---------------------------------------|--------------------------------|
// | a plain `int`                         | ADMITTED                       |
// | a `uint`/`ulong`/`long` CONSTANT      | ADMITTED (if it fits `int`)    |
// | a `uint`/`ulong`/`long` RUNTIME value | REFUSED (§5.1's table)         |
// | a NON-NARROWED nullable `int?`        | REFUSED (§5.1 one-way + §22.6) |
// | a NARROWED nullable (`int?`)          | ADMITTED, and it RENDERS the   |
// |                                       | radix form                     |
// | a RUNTIME `int`                       | ADMITTED, and it RENDERS the   |
// |                                       | radix form                     |
// | a FOLDABLE constant expression        | ADMITTED, and it RENDERS the   |
// |                                       | radix form                     |
//
// The narrowed-nullable rows are the ones the R35 review of the PRIMITIVE
// half rejected an earlier fix for: a narrowed nullable MUST be admitted (and
// must render the radix form). The refusal rows assert a REAL location and a
// SPECIFIC needle — never a crash, never a silent acceptance.
//
// The unadmitted radix (stdlib §9.1's ArgumentException) is #7268's rule and
// is NOT touched here; the corpus row l2_3_0003 still records that
// divergence, and this file does not assert it either way.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::diagnosticMessageContains;

namespace {

std::string prog(const std::string& body) {
    return "module M; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The §5.1 / §22.6 refusal of a NULLABLE radix: the program did not run, the
// driver rejected it at a REAL source location (never a crash / signal /
// timeout / ASan report), and the diagnostic carries the house "no matching
// overload" wording with the declared parameter type and the §5.1 citation.
void expectNullableRadixRefused(const JitResult& r) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no matching overload of `Convert.ToString`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the `toBase` argument must be an `int`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§5.1"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§7.4"));
    // The pre-fix symptom: the compiler accepted the call and printed the
    // DECIMAL text of the value.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
}

}  // namespace

// ===========================================================================
// B1 — the NULLABLE radix. REFUSED when §7.4 has NOT narrowed it.
// ===========================================================================

// The plainest un-narrowed nullable: a local declared `int?` and initialized
// with `null` — §7.4's "the declaration's assignment" narrows only a
// non-nullable initializer, so this read is still `int?` at the call.
BEZEL_TEST(convert_radix_residual_7286_mirror,
           nullable_radix_is_refused_5_1_22_6) {
    auto r = runAot(prog(
        "    var r: int? := null; "
        "    Console.WriteLine(Convert.ToString(255, r));"));
    expectNullableRadixRefused(r);
}

// The narrowing INVALIDATED: §7.4's "the variable is reassigned from a
// nullable-typed expression" drops the fact, so the read is `int?` again.
BEZEL_TEST(convert_radix_residual_7286_mirror,
           nullable_radix_after_the_narrowing_is_invalidated_is_refused_5_1) {
    auto r = runAot(prog(
        "    var r: int? := 16; "
        "    r := null; "
        "    Console.WriteLine(Convert.ToString(255, r));"));
    expectNullableRadixRefused(r);
}

// A nullable PARAMETER is never narrowed (§7.4: "Narrowing applies to locals
// — and `readonly` fields — only", and a parameter's narrowing is not
// established by its declaration).
BEZEL_TEST(convert_radix_residual_7286_mirror,
           nullable_parameter_radix_is_refused_5_1) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(F(16)); "
        "    return 0; "
        "  end Main; "
        "  public static method F(radix: int?): string "
        "    return Convert.ToString(255, radix); "
        "  end F; "
        "end C;");
    expectNullableRadixRefused(r);
}

// ===========================================================================
// B1 — the NARROWED nullable radix. ADMITTED, and it RENDERS the radix form.
// ===========================================================================

// §7.4's null test in the `<>` spelling narrows the true branch, and §22.6
// reads the argument's static type as its non-nullable type there — so the
// radix is the `int` it denotes, and §9.1's radix text is "ff".
BEZEL_TEST(convert_radix_residual_7286_mirror,
           narrowed_nullable_radix_renders_the_radix_form_7_4_22_6) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var r: int? := 16; "
                        "    if r <> null then "
                        "      Console.WriteLine(Convert.ToString(255, r)); "
                        "    end;")),
                    "ff");
}

// The same narrowing established by an EARLY RETURN (§7.4: "Narrowing also
// applies after early return").
BEZEL_TEST(convert_radix_residual_7286_mirror,
           early_return_narrowed_nullable_radix_renders_the_radix_form_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var r: int? := 16; "
                        "    if r = null then "
                        "      return 0; "
                        "    end; "
                        "    Console.WriteLine(Convert.ToString(255, r));")),
                    "ff");
}

// §7.4's "the declaration's assignment" narrows a declared-nullable local
// whose initializer is non-nullable, so this read is narrowed too — and the
// radix renders the radix form.
BEZEL_TEST(convert_radix_residual_7286_mirror,
           declaration_narrowed_nullable_radix_renders_the_radix_form_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var r: int? := 16; "
                        "    Console.WriteLine(Convert.ToString(255, r));")),
                    "ff");
}

// The null-forgiving operator (§17.12) reads the present value — the same
// `int` the narrowed read denotes.
BEZEL_TEST(convert_radix_residual_7286_mirror,
           forced_nullable_radix_renders_the_radix_form_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var r: int? := 16; "
                        "    Console.WriteLine(Convert.ToString(255, r!));")),
                    "ff");
}

// ===========================================================================
// C4 — the RUNTIME radix. ADMITTED, and it RENDERS the radix form.
// ===========================================================================

// stdlib §9.1: the digits are chosen by the radix's VALUE, not by whether
// the argument happens to be a compile-time constant.
BEZEL_TEST(convert_radix_residual_7286_mirror,
           runtime_int_radix_renders_the_radix_form_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var r: int := 16; "
                        "    Console.WriteLine(Convert.ToString(255, r));")),
                    "ff");
}

// The same at radix 8 — "377".
BEZEL_TEST(convert_radix_residual_7286_mirror,
           runtime_int_radix_8_renders_the_octal_form_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var r: int := 8; "
                        "    Console.WriteLine(Convert.ToString(255, r));")),
                    "377");
}

// The same at radix 2 — "11111111".
BEZEL_TEST(convert_radix_residual_7286_mirror,
           runtime_int_radix_2_renders_the_binary_form_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var r: int := 2; "
                        "    Console.WriteLine(Convert.ToString(255, r));")),
                    "11111111");
}

// A runtime radix of a WIDENING type (§5.1's table) — a `byte` 16 is 16.
BEZEL_TEST(convert_radix_residual_7286_mirror,
           runtime_byte_radix_renders_the_radix_form_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var r: byte := byte(16); "
                        "    Console.WriteLine(Convert.ToString(255, r));")),
                    "ff");
}

// A runtime radix of a `char` — §5.1 widens a `char` to an `int`.
BEZEL_TEST(convert_radix_residual_7286_mirror,
           runtime_char_radix_renders_the_radix_form_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var r: char := char(16); "
                        "    Console.WriteLine(Convert.ToString(255, r));")),
                    "ff");
}

// A NEGATIVE value at a runtime radix — §9.1: "a `-` for a negative value
// followed by the magnitude's digits most significant first".
BEZEL_TEST(convert_radix_residual_7286_mirror,
           runtime_radix_negative_value_renders_the_magnitude_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var r: int := 16; "
                        "    Console.WriteLine(Convert.ToString(-42, r));")),
                    "-2a");
}

// ===========================================================================
// B2 — the FOLDABLE constant expression. ADMITTED, radix form.
// ===========================================================================

// §5.1's constant narrowing admits a `uint` constant of any value that fits
// `int` ("the source may be a compile-time constant of **any** integer type");
// the RENDERING follows the radix's VALUE.
BEZEL_TEST(convert_radix_residual_7286_mirror,
           foldable_unsigned_constant_radix_renders_the_radix_form_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    Console.WriteLine(Convert.ToString(255, 8u + 8u));")),
                    "ff");
}

// The same at radix 8 — "377".
BEZEL_TEST(convert_radix_residual_7286_mirror,
           foldable_int_constant_radix_renders_the_octal_form_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    Console.WriteLine(Convert.ToString(255, 4 + 4));")),
                    "377");
}

// ===========================================================================
// The CONTROLS — green on BOTH arms (the conforming spellings are unchanged).
// ===========================================================================

// stdlib §9.1: the lower-case digits.
BEZEL_TEST(convert_radix_residual_7286_mirror,
           int_literal_radix_renders_the_radix_form_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    Console.WriteLine(Convert.ToString(255, 16));")),
                    "ff");
}

// The `uint` literal radix (stdlib §5.1's constant narrowing, across
// signedness).
BEZEL_TEST(convert_radix_residual_7286_mirror,
           uint_literal_radix_renders_the_radix_form_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    Console.WriteLine(Convert.ToString(255, 16u));")),
                    "ff");
}

// Radix 8 — "377".
BEZEL_TEST(convert_radix_residual_7286_mirror,
           int_literal_radix_8_renders_the_octal_form_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    Console.WriteLine(Convert.ToString(255, 8));")),
                    "377");
}

// Radix 2 — "11111111".
BEZEL_TEST(convert_radix_residual_7286_mirror,
           int_literal_radix_2_renders_the_binary_form_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    Console.WriteLine(Convert.ToString(255, 2));")),
                    "11111111");
}

// Radix 10 — the decimal text.
BEZEL_TEST(convert_radix_residual_7286_mirror,
           int_literal_radix_10_renders_the_decimal_form_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    Console.WriteLine(Convert.ToString(255, 10));")),
                    "255");
}

// The one-argument form (stdlib §2.3) is untouched.
BEZEL_TEST(convert_radix_residual_7286_mirror,
           one_argument_form_is_unchanged_2_3) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    Console.WriteLine(Convert.ToString(255));")),
                    "255");
}

// A `long` value above `Int32.MaxValue` keeps its own value (stdlib §2.3's
// `ToString(value: long, toBase: int)` overload).
BEZEL_TEST(convert_radix_residual_7286_mirror,
           long_value_at_radix_16_keeps_its_own_value_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    Console.WriteLine(Convert.ToString(4000000000L, 16));")),
                    "ee6b2800");
}

// A negative value at a constant radix — "2a" after the '-' (stdlib §9.1).
BEZEL_TEST(convert_radix_residual_7286_mirror,
           negative_value_at_radix_16_renders_the_magnitude_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    Console.WriteLine(Convert.ToString(-42, 16));")),
                    "-2a");
}

// A negative value at radix 2 — "-101010".
BEZEL_TEST(convert_radix_residual_7286_mirror,
           negative_value_at_radix_2_renders_the_magnitude_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    Console.WriteLine(Convert.ToString(-42, 2));")),
                    "-101010");
}

// Zero is a single "0" (stdlib §9.1: "a single `0` for zero").
BEZEL_TEST(convert_radix_residual_7286_mirror,
           zero_at_radix_16_renders_a_single_zero_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    Console.WriteLine(Convert.ToString(0, 16));")),
                    "0");
}

// A `string` radix binds to no declared parameter (§5.1 has no conversion TO
// a `string`… and `toBase` is declared `int`), so §22.6 refuses the call —
// the mirror has NO format overload, so this is not the primitive site's B3.
BEZEL_TEST(convert_radix_residual_7286_mirror,
           string_radix_is_refused_5_1) {
    auto r = runAot(prog(
        "    Console.WriteLine(Convert.ToString(255, \"16\"));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no matching overload of `Convert.ToString`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the `toBase` argument must be an `int`"));
}
