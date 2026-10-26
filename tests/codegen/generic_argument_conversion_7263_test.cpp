// Regression tests for #7263 — a GENERIC method's argument was not converted
// to its DECLARED parameter type, so the VALUE was lost.
//
// WHAT THE DOCUMENTS REQUIRE.
//
// spec §5.1 "Implicit conversions" (v0.344) states:
//
//   "A value of one type implicitly converts to another when no programmer-
//    visible information is lost"
//
// and its widening table admits an UNSIGNED integer at every wider type:
//
//   | `byte`   | `short`, `ushort`, `int`, `uint`, `long`, `ulong`, `float`, `double`, `decimal` |
//   | `ushort` | `int`, `uint`, `long`, `ulong`, `float`, `double`, `decimal` |
//   | `uint`   | `long`, `ulong`, `float`, `double`, `decimal` |
//   | `ulong`  | `float`, `double`, `decimal` |
//
// §5.1 is a table of VALUES. An integer conversion that fits the target
// carries the source's NUMERIC value across; it does not reinterpret the
// source's bit pattern. (spec §5.2 makes the same point about the explicit
// form: "Such a cast is a *value* conversion, not a reinterpretation of bits:
// it preserves the numeric value or fails.")
//
// §22.6's applicability stage reads the parameter types "under the implicit
// conversions (§5.1)", so an unsigned argument IS admitted at a `long`
// parameter — and it converts THERE, at the call.
//
// WHAT THE DEFECT WAS.
//
// `emitGenericSpecialization` (src/codegen/LlvmBackend_Calls2.cpp) built the
// specialized function's signature from the ARGUMENT's LLVM type — its own
// header comment said so: "Each param type defaults to its argument's LLVM
// type". So an argument NARROWER than its declared formal reached the
// specialized body UNCONVERTED, at the ARGUMENT's own width:
//
//   define internal i64 @"C$Take$spec"(double %0, i32 %1)   // `by: long`
//     %5 = load i32, ptr %2, align 4
//     %step.by64 = sext i32 %5 to i64          // the DECLARED type's signedness
//
// The body then read the parameter at the DECLARED type — `by` is declared
// `long`, so the conversion applied the DECLARED type's SIGNEDNESS to the
// NARROWER's bit pattern. The `uint` 4000000000 (bit pattern 0xEE6B2800) was
// read as the `int32` -294967296, and the sign extension preserved it:
//
//   Take(1.0, 4000000000u)   printed -294967296   (should be 4000000000)
//   Take(1.0, 4000000000L)   printed  4000000000   (the control)
//
// The two spellings of the same number disagreed — and the ORDINARY (non-
// generic) call path already converted the same argument at the same declared
// formal CORRECTLY, so the generic path was the odd one out.
//
// The defect is NOT recoverable inside the body: there the parameter's type IS
// the declared `long`, so the argument's unsignedness is already gone by the
// time the body widens. The conversion therefore belongs at the GENERIC CALL
// SITE, where the argument's SOURCE expression — and so its signedness — is
// still known.
//
// WHAT THE FIX DOES.
//
// At the generic call site the argument is converted into the DECLARED
// parameter type with the ORDINARY call path's own unsigned-aware coercion
// (`coerceNumericToType`, the same helper the plain path applies to the same
// argument at the same declared formal), and the specialized parameter is
// typed as that declared type:
//
//   define internal i64 @"C$Take$spec"(double %0, i64 %1)   // `by: long`
//     %5 = load i64, ptr %2, align 8
//     ret i64 %5
//
// The signedness is read through the ONE house predicate,
// `isUnsignedTargetExpr` (src/codegen/LlvmBackend_impl.hpp) — the same one
// the #7253 `Step` widening, the #7251 `double`-parameter converter and the
// #5046 nullable lift use, so no two sites can disagree about an operand's
// signedness. It is NOT inferred from the bit width: `int` and `uint` are
// both 32 bits, `long` and `ulong` both 64.
//
// THE MATRIX.
//
// The rows below cover the DECLARED PARAMETER TYPE (the formal) against the
// ARGUMENT SOURCE (the argument), which is the axis the defect lives on:
//
//   * the formal: `long`, `int`, `double`, `float`, `decimal` — every SIGNED
//     numeric type, i.e. every formal whose declared SIGNEDNESS the body
//     would have applied to a narrower source's bit pattern;
//   * the source: `uint`, `byte`, `ushort`, `ulong` — every UNSIGNED integer
//     type, at the widths where the loss is visible.
//
// THE CONTROLS.
//
// The control rows are green on BOTH arms and are what makes the pin rows
// trustworthy:
//
//   * the ORDINARY (non-generic) call path with the same declared formal and
//     the same argument — it was already correct, and it must STAY correct
//     (a fix that changed the plain path would be over-broad);
//   * a SIGNED source at the same generic formal — a negative `long` must
//     keep its sign (a fix that zero-extended indiscriminately would turn
//     -4000000000 into a huge positive number);
//   * an UNSIGNED formal (`ulong`) with the same unsigned source — the
//     declared type's own signedness already made that path correct, and it
//     must stay correct;
//   * a TYPE-PARAMETER formal (`v: T` at `T = double`) with the same
//     unsigned source — the bare-type-parameter conversion (#6900) already
//     handled that shape;
//   * a generic CLASS's CONSTRUCTOR — the constructor path already converted
//     the argument to the declared formal.
//
// THE PROOF HIERARCHY.
//
// Every accepting row that carries an UNSIGNED source at a SIGNED formal
// FAILS pre-fix: the program printed a wrong value, or (for the
// expression-bodied `double` formal) did not even lower to valid IR. Each
// such row therefore pins the VALUE, not merely "it compiled". The control
// rows are green on both arms.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

using bezel::test::diagnosticMessageContains;

namespace {

// A generic method whose formal `by` has the DECLARED type `formal`, called
// with the argument `arg`.
//
//   formal — the declared type of the `by` parameter (e.g. "long")
//   decls  — the statements that bind the argument (e.g. "var u: uint := …;")
//   arg    — the argument expression at the call site
//   retTy  — the generic method's declared return type
std::string takeProg(const std::string& formal, const std::string& decls,
                     const std::string& arg, const std::string& retTy) {
    return "module M; import System; class C "
           "  public static method Take<T>(v: T, by: " + formal + "): " +
           retTy + " "
           "    return by; "
           "  end Take; "
           "  public static method Main(): int " + decls +
           "    Console.WriteLine(Take(1.0, " + arg + ")); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The same generic method, but with an EXPRESSION body (`=> by`) — the
// inline path, which binds the parameter into the CALLER's own stack slot
// rather than into a specialized function.
std::string takeExprProg(const std::string& formal, const std::string& decls,
                         const std::string& arg) {
    return "module M; import System; class C "
           "  public static method Take<T>(v: T, by: " + formal + "): " +
           formal + " => by; "
           "  public static method Main(): int " + decls +
           "    Console.WriteLine(Take(1.0, " + arg + ")); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The ORDINARY (non-generic) call path — the CONTROL. The same declared
// formal, the same argument, no type parameter anywhere.
std::string plainProg(const std::string& formal, const std::string& decls,
                      const std::string& arg) {
    return "module M; import System; class C "
           "  public static method Take(by: " + formal + "): " + formal + " "
           "    return by; "
           "  end Take; "
           "  public static method Main(): int " + decls +
           "    Console.WriteLine(Take(" + arg + ")); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// A generic CLASS's generic method — the same defect through the class
// instantiation's substitution.
std::string classGenProg(const std::string& decls, const std::string& arg) {
    return "module M; import System; "
           "class Box<T> "
           "  public static method Take<U>(v: U, by: long): long "
           "    return by; "
           "  end Take; "
           "end Box; "
           "class C "
           "  public static method Main(): int " + decls +
           "    Console.WriteLine(Box<int>.Take(1, " + arg + ")); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The accepting shape: the program RAN, it produced exactly this text, and
// it did not fail the compiler.
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
// (a) THE ISSUE'S OWN REPRO — a `long` formal and a `uint` argument.
//
// §5.1's table admits `uint` at `long`, and §5.1 converts the VALUE. Pre-fix
// the argument reached the body as an `i32` and the body sign-extended it
// with the DECLARED type's signedness, so the program printed -294967296.
// ===========================================================================
BEZEL_TEST(generic_argument_conversion_7263,
           generic_long_formal_uint_argument_keeps_its_value_5_1) {
    expectValue(takeProg("long", "", "4000000000u", "long"), "4000000000");
}

// The SAME, with the argument as a `uint` VARIABLE — a non-constant source,
// so the conversion cannot be folded away and the value really travels
// through the widened parameter.
BEZEL_TEST(generic_argument_conversion_7263,
           generic_long_formal_uint_variable_keeps_its_value_5_1) {
    expectValue(takeProg("long", "var u: uint := 4000000000u;", "u", "long"),
                "4000000000");
}

// THE TWO SPELLINGS MUST AGREE — the issue's own claim, made testable. One
// program, both spellings of the same number.
BEZEL_TEST(generic_argument_conversion_7263,
           generic_long_formal_uint_and_long_spellings_agree_5_1) {
    expectValue(
        "module M; import System; class C "
        "  public static method ByUint<T>(v: T, by: long): long "
        "    return by; "
        "  end ByUint; "
        "  public static method ByLong<T>(v: T, by: long): long "
        "    return by; "
        "  end ByLong; "
        "  public static method Main(): int "
        "    Console.WriteLine(ByUint(1.0, 4000000000u)); "
        "    Console.WriteLine(ByLong(1.0, 4000000000L)); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "4000000000\n4000000000");
}

// ===========================================================================
// (b) THE ARGUMENT SOURCE MATRIX — every UNSIGNED integer type at a SIGNED
// formal. The loss is visible at every width where the value does not fit
// the narrower type's SIGNED range.
//
// A `byte` 200 is the sharpest case: 200 fits in an `int`, so the value is
// NOT out of range anywhere — only the SIGNEDNESS of the widening is wrong,
// and it turns 200 into -56.
// ===========================================================================
BEZEL_TEST(generic_argument_conversion_7263,
           generic_long_formal_byte_argument_keeps_its_value_5_1) {
    expectValue(takeProg("long", "var b: byte := 200;", "b", "long"), "200");
}

BEZEL_TEST(generic_argument_conversion_7263,
           generic_long_formal_ushort_argument_keeps_its_value_5_1) {
    expectValue(takeProg("long", "var s: ushort := 60000;", "s", "long"),
                "60000");
}

// The `int` formal — the same defect one width down: the body would widen
// the `i8` argument to the declared `int` with the `int`'s signedness.
BEZEL_TEST(generic_argument_conversion_7263,
           generic_int_formal_byte_argument_keeps_its_value_5_1) {
    expectValue(takeProg("int", "var b: byte := 200;", "b", "int"), "200");
}

BEZEL_TEST(generic_argument_conversion_7263,
           generic_int_formal_ushort_argument_keeps_its_value_5_1) {
    expectValue(takeProg("int", "var s: ushort := 60000;", "s", "int"),
                "60000");
}

// ===========================================================================
// (c) THE FORMAL MATRIX — every SIGNED numeric formal, with the same `uint`
// argument.
//
// A `double` formal: §5.1 admits `uint` at `double`, and the conversion
// converts the VALUE (4e9), so the body's own `sitofp` is handed 4e9 and not
// the `int32` -294967296.
// ===========================================================================
BEZEL_TEST(generic_argument_conversion_7263,
           generic_double_formal_uint_argument_keeps_its_value_5_1) {
    expectValue(takeProg("double", "", "4000000000u", "double"), "4e+09");
}

BEZEL_TEST(generic_argument_conversion_7263,
           generic_float_formal_uint_argument_keeps_its_value_5_1) {
    expectValue(takeProg("float", "", "4000000000u", "float"), "4e+09");
}

BEZEL_TEST(generic_argument_conversion_7263,
           generic_decimal_formal_uint_argument_keeps_its_value_5_1) {
    expectValue(takeProg("decimal", "", "4000000000u", "decimal"), "4000000000");
}

// A `ulong` source — beyond `int64`, so its bit pattern read as a signed
// integer is a DIFFERENT wrong number (-1 rather than -294967296). At a
// `double` formal §5.1's table admits `ulong`, and the value is the
// `double` 1.8446744073709552e19.
BEZEL_TEST(generic_argument_conversion_7263,
           generic_double_formal_ulong_argument_keeps_its_value_5_1) {
    expectValue(takeProg("double", "var ul: ulong := 18446744073709551615UL;",
                         "ul", "double"),
                "18446744073709552000");
}

// At a `decimal` formal the value is EXACT (§6.4's decimal semantics).
BEZEL_TEST(generic_argument_conversion_7263,
           generic_decimal_formal_ulong_argument_keeps_its_value_5_1) {
    expectValue(takeProg("decimal", "var ul: ulong := 18446744073709551615UL;",
                         "ul", "decimal"),
                "18446744073709551615");
}

// ===========================================================================
// (d) THE OTHER GENERIC FORMS — the same defect through the other shapes a
// generic call takes.
// ===========================================================================

// A generic CLASS's generic method. The substitution is the CLASS's, the
// call is the method's, and the `by: long` formal is the same declared type.
BEZEL_TEST(generic_argument_conversion_7263,
           generic_class_method_long_formal_uint_argument_keeps_its_value_5_1) {
    expectValue(classGenProg("", "4000000000u"), "4000000000");
}

// An EXPRESSION-bodied generic method — the INLINE path, which binds the
// parameter into the CALLER's own stack slot instead of into a specialized
// function. Pre-fix the slot was typed by the ARGUMENT, so a `double` formal
// left the body's own result an `i32` where the declared `double` was
// required: the module did not lower to valid IR at all ("Call parameter
// type does not match function signature!").
BEZEL_TEST(generic_argument_conversion_7263,
           generic_expression_body_double_formal_uint_argument_keeps_its_value_5_1) {
    expectValue(takeExprProg("double", "", "4000000000u"), "4e+09");
}

BEZEL_TEST(generic_argument_conversion_7263,
           generic_expression_body_long_formal_uint_argument_keeps_its_value_5_1) {
    expectValue(takeExprProg("long", "", "4000000000u"), "4000000000");
}

// THE ISSUE'S OWN SHAPE OVER A GENERIC HELPER — `ISteppable<T>.Step`
// (stdlib §2.19) is declared `Step(value: T, by: long)`, so the `by` is
// converted to `long` at the call, and §5.1's conversion converts the VALUE.
// `1.0 + 4000000000` is 4000000001 — exactly what the `long` spelling gives.
BEZEL_TEST(generic_argument_conversion_7263,
           generic_step_helper_uint_by_keeps_its_value_5_1) {
    expectValue(
        "module M; import System; static class F "
        "  public static method Nxt<T>(v: T): T where T: ISteppable<T> "
        "    return T.Step(v, 4000000000u); "
        "  end Nxt; "
        "end F; "
        "class C public static method Main(): int "
        "  Console.WriteLine(F.Nxt(1.0).ToString()); "
        "  return 0; "
        "end Main; end C;",
        "4000000001");
}

// ===========================================================================
// THE CONTROLS — green on BOTH arms.
//
// (e) THE ORDINARY (NON-GENERIC) CALL PATH. The same declared formal and
// the same argument, with no type parameter anywhere: the plain path
// already converted the argument correctly (its own `coerceExprValueToSlot`
// is unsigned-aware), and this fix must not have changed it.
// ===========================================================================
BEZEL_TEST(generic_argument_conversion_7263,
           plain_long_formal_uint_argument_is_already_correct_5_1) {
    expectValue(plainProg("long", "", "4000000000u"), "4000000000");
}

BEZEL_TEST(generic_argument_conversion_7263,
           plain_double_formal_uint_argument_is_already_correct_5_1) {
    expectValue(plainProg("double", "", "4000000000u"), "4e+09");
}

// (f) A SIGNED SOURCE MUST KEEP ITS SIGN. A fix that zero-extended
// indiscriminately would turn -4000000000 into a huge positive number.
BEZEL_TEST(generic_argument_conversion_7263,
           generic_long_formal_negative_long_keeps_its_sign_5_1) {
    expectValue(takeProg("long", "", "-4000000000L", "long"), "-4000000000");
}

BEZEL_TEST(generic_argument_conversion_7263,
           generic_double_formal_negative_long_keeps_its_sign_5_1) {
    expectValue(takeProg("double", "", "-4000000000L", "double"), "-4e+09");
}

// A signed `int` source at a `long` formal — the signed widening the body
// already performed, unchanged.
BEZEL_TEST(generic_argument_conversion_7263,
           generic_long_formal_negative_int_keeps_its_sign_5_1) {
    expectValue(takeProg("long", "", "-3", "long"), "-3");
}

// (g) AN UNSIGNED FORMAL. `ulong` is unsigned, so the body's own widening
// was already a zero-extend and that path was already correct — it must
// STAY correct.
BEZEL_TEST(generic_argument_conversion_7263,
           generic_ulong_formal_uint_argument_is_already_correct_5_1) {
    expectValue(takeProg("ulong", "", "4000000000u", "ulong"), "4000000000");
}

// (h) A TYPE-PARAMETER FORMAL (`v: T` at `T = double`) with the same
// unsigned source — the bare-type-parameter conversion (#6900) already
// handled that shape, and it must stay correct.
BEZEL_TEST(generic_argument_conversion_7263,
           generic_type_parameter_formal_uint_argument_is_already_correct_5_1) {
    expectValue(
        "module M; import System; class C "
        "  public static method Take<T>(v: T, by: T): T "
        "    return by; "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take<double>(1.0, 4000000000u)); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "4e+09");
}

// (i) A generic CLASS's CONSTRUCTOR — the constructor path already converts
// the argument to the declared formal, and it must stay correct.
BEZEL_TEST(generic_argument_conversion_7263,
           generic_class_constructor_long_formal_uint_argument_is_already_correct_5_1) {
    expectValue(
        "module M; import System; "
        "class Box<T> "
        "  public _by: long; "
        "  public constructor(v: T, by: long) "
        "    _by := by; "
        "  end constructor; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var b := new Box<int>(1, 4000000000u); "
        "    Console.WriteLine(b._by); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "4000000000");
}
