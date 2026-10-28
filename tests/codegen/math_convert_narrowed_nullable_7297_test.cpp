// Spec-derived regression test for issues #7297 (face (b)) and #7232 (the
// narrowed half) — a §7.4-NARROWED nullable at a `Math` / `Convert`
// PARAMETER.
//
// ─── THE NORMATIVE SENTENCES (CHECKED in docs/, v0.345) ─────────────────────
//
// spec §7.4 (v0.345), "How narrowing is established" (docs/Bezel_Language_Spec.md
// line 909):
//
//   "…and from a declaration whose initializer is such an `expr` — `var x: T? :=
//   expr;` or `const x: T? := expr;` (§21.1) — which is the declaration's
//   assignment, its narrowing invalidated by exactly the forms below;"
//
//   and (line 909, the null-test bullet):
//   "a null test in either spelling — `x <> null` or `x is not null` (narrowing
//   the true branch) …"
//
//   and (line 909, the early-exit bullet):
//   "an early exit: after `if x = null then return; end;` … the surviving path
//   is narrowed"
//
// spec §22.6 (v0.345), "Overloading" (docs/Bezel_Language_Spec.md line 4591):
//
//   "**The argument's static type** is its type at the call: a read of a
//   binding §7.4 has narrowed non-null is of its non-nullable type, as §7.11
//   reads it …"
//
// spec §5.1 (v0.345), "Nullable conversions" (docs/Bezel_Language_Spec.md line
// 649):
//
//   "Each implicit conversion of this section also **lifts** to the nullable
//   forms: `S?` implicitly converts to `T?` whenever `S` implicitly converts to
//   `T`"
//   => the lifting is ONE-WAY: a `double?` reaches NO non-nullable `double`
//      parameter, so an UN-narrowed `double?` is a compile error (§7.3).
//
// stdlib §2.2 (`Math`) — every declared form takes a NON-nullable parameter:
//   "Round(x: double, digits: int := 0): double", "Sqrt(x: double): double",
//   "Floor(x: double): double", "Ceiling(x: double): double",
//   "Truncate(x: double): double", "Pow(x: double, y: double): double",
//   "Sign(x: int|long|double): int", "Abs(x: int): int" (and the `long`,
//   `double` and `decimal` forms), "Fmod(x: double, y: double): double",
//   "IEEERemainder(x: double, y: double): double".
//
// stdlib §2.3 (`Convert`) — "ToString(value: int): string" (and the `long`,
//   `float`, `double`, `decimal`, `bool` forms), "ToString(value: int, toBase:
//   int): string", "ToInt32(value: string): int".
//
// ─── WHAT THE DEFECT WAS ────────────────────────────────────────────────────
//
// A §7.4-NARROWED read is the `double` it denotes (§22.6), so it is LEGAL at a
// `double` parameter — but the `Math` / `Convert` call lowerings loaded the
// WHOLE nullable carrier (the `{i8 hasValue; double}` box) and passed it where
// a SCALAR was expected. The compiler then ABORTED inside the LLVM verifier on
// a LEGAL program:
//
//   var d: double? := 2.5;
//   Console.WriteLine(Math.Floor(d));
//     "Call parameter type does not match function signature!
//       %1 = load { i8, double }, ptr %0, align 8"
//     "bezelc: codegen error: IR verification failed (malformed IR emitted)"
//                                                    -> rc=1 (a COMPILER CRASH)
//
// (the same shape for `Math.Ceiling`, `Math.Truncate`, `Math.Sqrt`,
// `Math.Round`, `Math.Pow`, `Math.Sign`, `Math.Abs`, `Math.Fmod`,
// `Math.IEEERemainder`, `Convert.ToString`, `Convert.ToString(value, toBase)`
// and `Convert.ToInt32`).
//
// ─── THE FIX ─────────────────────────────────────────────────────────────────
//
// The `Math` / `Convert` lowerings now extract the PAYLOAD of a narrowed read
// at the positions §7's nullability gate claims (`common::
// builtinParamIsNonNullable` — the ONE predicate the gate reads), so the
// argument that reaches the arithmetic is the `double` the read denotes.
//
// The extraction is applied at exactly the positions the §7 gate claims, so an
// UN-narrowed nullable is still REFUSED there by the gate (the control rows
// below) and never reaches the extraction.
//
// ─── THE RULE THIS TEST PINS ────────────────────────────────────────────────
//
// | the argument                                    | the verdict              |
// |-------------------------------------------------|--------------------------|
// | a §7.4-NARROWED nullable read (`T?`)           | ADMITTED, and the call    |
// |                                                 | computes with the        |
// |                                                 | PAYLOAD (not the box)     |
// | an UN-narrowed nullable (`T?`)                  | REFUSED (§7)              |
// | a narrowed local REASSIGNED from a nullable      | REFUSED (§7) — the        |
// |                                                 | narrowing is invalidated  |
// | a nullable PARAMETER                            | REFUSED (§7) — narrowing   |
// |                                                 | applies to locals only    |
//
// The oracle is the PROGRAM'S OUTPUT: a narrowed read must print the PAYLOAD's
// value, never a box, and never abort the compiler.

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

// The §7 refusal: the program did not run, the driver rejected it at a REAL
// source location (never a crash / signal / timeout / ASan report), and the
// diagnostic carries the house §7 wording.
void expectRefusedBySection7(const JitResult& r, const char* member) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("the argument to `") + member +
            "` is nullable but the target type is non-nullable (§7)")
                .c_str()));
    // The pre-fix symptom: the compiler aborted inside the LLVM verifier.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "terminated by signal"));
}

}  // namespace

// ===========================================================================
// The NARROWED nullable at a `Math` position. ADMITTED, and it computes with
// the PAYLOAD.
// ===========================================================================

// §7.4's "the declaration's assignment" narrows a declared-nullable local
// whose initializer is non-nullable, so this read is a `double` at the call
// (§22.6) — and `Math.Floor` of 2.5 is 2.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           narrowed_double_floor_computes_the_payload_7_4_22_6) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    Console.WriteLine(Math.Floor(d));")),
                    "2");
}

// The same narrowing established by §7.4's null test in the `<>` spelling.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           null_test_narrowed_double_floor_computes_the_payload_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    if d <> null then "
                        "      Console.WriteLine(Math.Floor(d)); "
                        "    end;")),
                    "2");
}

// The same narrowing established by §7.4's early exit.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           early_exit_narrowed_double_floor_computes_the_payload_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    if d = null then "
                        "      return 0; "
                        "    end; "
                        "    Console.WriteLine(Math.Floor(d));")),
                    "2");
}

// §7.8's null-forgiving operator reads the present value — the same `double`
// the narrowed read denotes.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           forced_nullable_double_floor_computes_the_payload_7_8) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    Console.WriteLine(Math.Floor(d!));")),
                    "2");
}

// stdlib §2.2: `Ceiling(x: double): double` — 2.5 ceils to 3.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           narrowed_double_ceiling_computes_the_payload_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    Console.WriteLine(Math.Ceiling(d));")),
                    "3");
}

// stdlib §2.2: `Truncate(x: double): double` — 2.5 truncates to 2.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           narrowed_double_truncate_computes_the_payload_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    Console.WriteLine(Math.Truncate(d));")),
                    "2");
}

// stdlib §2.2: `Sqrt(x: double): double`.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           narrowed_double_sqrt_computes_the_payload_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    Console.WriteLine(Math.Sqrt(d));")),
                    "1.5811388300841898");
}

// stdlib §2.2: `Round(x: double, digits: int := 0): double` — banker's
// rounding, 2.5 → 2.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           narrowed_double_round_computes_the_payload_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    Console.WriteLine(Math.Round(d));")),
                    "2");
}

// The two-parameter form — `Round(x: double, digits: int := 0): double` — with
// the narrowed operand at the `x` position and a plain `int` at `digits`.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           narrowed_double_round_with_digits_computes_the_payload_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    if d <> null then "
                        "      Console.WriteLine(Math.Round(d, 2)); "
                        "    end;")),
                    "2.5");
}

// stdlib §2.2: `Pow(x: double, y: double): double` — BOTH operands narrowed.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           narrowed_double_pow_computes_the_payload_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    Console.WriteLine(Math.Pow(d, 2.0));")),
                    "6.25");
}

// stdlib §2.2: `Fmod(x: double, y: double): double`.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           narrowed_double_fmod_computes_the_payload_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    if d <> null then "
                        "      Console.WriteLine(Math.Fmod(d, 1.0)); "
                        "    end;")),
                    "0.5");
}

// stdlib §2.2: `IEEERemainder(x: double, y: double): double`.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           narrowed_double_ieeeremainder_computes_the_payload_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    if d <> null then "
                        "      Console.WriteLine(Math.IEEERemainder(d, 1.0)); "
                        "    end;")),
                    "0.5");
}

// stdlib §2.2: `Sign(x: int|long|double): int` — the result is an `int` at
// every operand type, and the operand's PAYLOAD is what it signs.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           narrowed_double_sign_computes_the_payload_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    if d <> null then "
                        "      Console.WriteLine(Math.Sign(d)); "
                        "    end;")),
                    "1");
}

// stdlib §2.2: `Abs(x: int): int` — the `int` overload at a narrowed `int?`.
// (`Math.Abs` over a narrowed `double?`/`float?` is the RESULT-TYPE half of
// this family and belongs to the semantic layer — see the RESIDUAL note in
// the commit body.)
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           narrowed_int_abs_computes_the_payload_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var n: int? := -3; "
                        "    if n <> null then "
                        "      Console.WriteLine(Math.Abs(n)); "
                        "    end;")),
                    "3");
}

// ===========================================================================
// The NARROWED nullable at a `Convert` position. ADMITTED, and it renders the
// PAYLOAD.
// ===========================================================================

// stdlib §2.3: `ToString(value: int): string`.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           narrowed_int_to_string_renders_the_payload_2_3) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var i: int? := 7; "
                        "    if i <> null then "
                        "      Console.WriteLine(Convert.ToString(i)); "
                        "    end;")),
                    "7");
}

// stdlib §2.3: `ToString(value: double): string`.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           narrowed_double_to_string_renders_the_payload_2_3) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    Console.WriteLine(Convert.ToString(d));")),
                    "2.5");
}

// stdlib §2.3: `ToString(value: decimal): string` — a `decimal` payload
// renders as a decimal (its own type, never a binary float).
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           narrowed_decimal_to_string_renders_the_payload_2_3) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var m: decimal? := 2.5m; "
                        "    if m <> null then "
                        "      Console.WriteLine(Convert.ToString(m)); "
                        "    end;")),
                    "2.5");
}

// stdlib §2.3 / §9.1: `ToString(value: int, toBase: int): string` — the
// narrowed operand renders the RADIX form, not the decimal text.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           narrowed_int_to_string_with_base_renders_the_radix_form_2_3_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var i: int? := 255; "
                        "    if i <> null then "
                        "      Console.WriteLine(Convert.ToString(i, 16)); "
                        "    end;")),
                    "ff");
}

// stdlib §2.3: `ToInt32(value: string): int`.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           narrowed_string_to_int32_computes_the_payload_2_3) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var s: string? := \"5\"; "
                        "    if s <> null then "
                        "      Console.WriteLine(Convert.ToInt32(s)); "
                        "    end;")),
                    "5");
}

// ===========================================================================
// The CONTROL. The UN-narrowed spelling is STILL REFUSED by §7.
// ===========================================================================

// The plainest un-narrowed nullable: a local declared `double?` and
// initialized with `null` — §7.4's "the declaration's assignment" narrows only
// a NON-nullable initializer, so this read is still `double?` at the call,
// and §5.1's one-way lifting reaches no non-nullable `double` parameter.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           unnarrowed_double_floor_is_refused_7_5_1) {
    auto r = runAot(prog(
        "    var d: double? := null; "
        "    Console.WriteLine(Math.Floor(d));"));
    expectRefusedBySection7(r, "Math.Floor");
}

// The same at the `Math.Abs` position.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           unnarrowed_double_abs_is_refused_7_5_1) {
    auto r = runAot(prog(
        "    var d: double? := null; "
        "    Console.WriteLine(Math.Abs(d));"));
    expectRefusedBySection7(r, "Math.Abs");
}

// The same at a `Convert` position.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           unnarrowed_string_to_int32_is_refused_7_5_1) {
    auto r = runAot(prog(
        "    var s: string? := null; "
        "    Console.WriteLine(Convert.ToInt32(s));"));
    expectRefusedBySection7(r, "Convert.ToInt32");
}

// §7.4's invalidation: a narrowed local REASSIGNED from a nullable-typed
// expression loses the narrowing, so the read is `double?` again.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           invalidated_narrowing_double_floor_is_refused_7_5_1) {
    auto r = runAot(prog(
        "    var d: double? := 2.5; "
        "    d := null; "
        "    Console.WriteLine(Math.Floor(d));"));
    expectRefusedBySection7(r, "Math.Floor");
}

// §7.4: "Narrowing applies to locals — and `readonly` fields — only", so a
// nullable PARAMETER is never narrowed and reaches no non-nullable parameter.
BEZEL_TEST(math_convert_narrowed_nullable_7297,
           nullable_parameter_double_floor_is_refused_7_5_1) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(F(2.5)); "
        "    return 0; "
        "  end Main; "
        "  public static method F(x: double?): int "
        "    return Math.Floor(x); "
        "  end F; "
        "end C;");
    expectRefusedBySection7(r, "Math.Floor");
}
