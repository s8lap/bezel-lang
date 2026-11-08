// fix #8266 (stdlib §5.1, spec §3.6 rule 2, §5.1, §14.2, v0.355): the
// run-time rows for a no-selector `Sum()` typed as the body's own type
// parameter `T` (stdlib §5.1: "public method Sum(): owned T;" over
// `extension IEnumerable<T> where T: INumber<T>`). The rejected `: int` shapes
// are pinned in tests/sema/linq_generic_body_sum_type_param_result_8266_test.cpp;
// these are the legal spellings that observe the same result: a `: T` body and
// a `: string` body through interpolation.
//
// Leak-checked (runAotChecked: generated-code ASan + LeakSanitizer), the record
// row looped 100x so a per-call leak of the folded `Label` strings cannot hide.
//
// fix #8253 (stdlib §5.1, spec §11.8 / §14.2, v0.355) adds the rows below:
// the generic body's no-selector `Sum()` must accumulate in the INSTANTIATED
// element type, not in the `int` the unresolved type parameter used to give it.
// stdlib §5.1 declares the no-selector form as
//
//     public extension IEnumerable<T> where T: INumber<T>
//       public method Sum(): owned T;        // T.Zero()/`+` over §2.19
//     end;
//
// and spec §11.8 models it as an accumulation over `T.Zero()` and `+` — so at
// T = double the total is a double, at T = long a long, and at T = byte a byte
// (overflow-checked in it, spec §6.1 / stdlib §5). The rows below use the
// STRICT AOT oracle (`runAot`): the defect was a COMPILE-TIME IR abort (the
// compiler's own `verifyModule` rejected the emitted module), so the row must
// drive the AOT pipeline and observe the program's stdout — a clean compile
// would not be a measurement.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The issue's record M2, except that `+` keeps the LEFT operand's Label
// instead of concatenating: a concatenating record `operator +` leaks its
// result strings in any context (#8255, open; MEASURED on the parent with no
// `Sum()` at all), which this leak-checked row must not inherit. The Label of
// a sum is therefore the seed's ("z", from Zero), which also pins that the
// fold starts at `T.Zero()`.
const char* kM2 =
    "public record M2(Label: string, Cents: int): INumber<M2>\n"
    "  public static method Zero(): M2 => new M2(\"z\", 0);\n"
    "  public static method operator +(a: M2, b: M2): M2 =>"
    " new M2(a.Label, a.Cents + b.Cents);\n"
    "  public static method One(): M2 => new M2(\"o\", 1);\n"
    "  public static method operator -(a: M2, b: M2): M2 =>"
    " new M2(a.Label, a.Cents - b.Cents);\n"
    "  public static method operator *(a: M2, b: M2): M2 =>"
    " new M2(a.Label, a.Cents * b.Cents);\n"
    "  public static method operator /(a: M2, b: M2): M2 =>"
    " new M2(a.Label, a.Cents / b.Cents);\n"
    "  public static method operator <(a: M2, b: M2): bool =>"
    " a.Cents < b.Cents;\n"
    "  public static method operator >(a: M2, b: M2): bool =>"
    " a.Cents > b.Cents;\n"
    "  public static method operator <=(a: M2, b: M2): bool =>"
    " a.Cents <= b.Cents;\n"
    "  public static method operator >=(a: M2, b: M2): bool =>"
    " a.Cents >= b.Cents;\n"
    "end M2;\n";

}  // namespace

// `: T` at T = int: 5 + 6.
BEZEL_TEST(codegen_linq_generic_body_sum_type_param_result_8266,
           t_body_at_int_prints_the_sum) {
    JitResult r = runAotChecked(
        "module M;\n"
        "class C\n"
        "  public static method G<T>(a: Array<T>): T where T: INumber<T> =>"
        " a.Sum();\n"
        "  public static method Main()\n"
        "    var a := new Array<int>();\n"
        "    a.Add(5); a.Add(6);\n"
        "    Console.WriteLine(G(a));\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\n");
}

// `: T` at the user record M2, looped 100x. Each sum folds from
// Zero() = M2("z", 0): Label "z", Cents 11. 100 x 11 = 1100.
BEZEL_TEST(codegen_linq_generic_body_sum_type_param_result_8266,
           t_body_at_user_inumber_record_loop_100x_no_leak) {
    JitResult r = runAotChecked(std::string("module Conf;\n") + kM2 +
        "class C\n"
        "  public static method G<T>(a: Array<T>): T where T: INumber<T> =>"
        " a.Sum();\n"
        "end C;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var i := 0;\n"
        "    var t := 0;\n"
        "    var last := \"\";\n"
        "    while i < 100 do\n"
        "      var a := new Array<M2>();\n"
        "      a.Add(new M2(\"a\", 5)); a.Add(new M2(\"b\", 6));\n"
        "      var s := C.G(a);\n"
        "      t := t + s.Cents;\n"
        "      last := s.Label;\n"
        "      i := i + 1;\n"
        "    end;\n"
        "    Console.WriteLine(t);\n"
        "    Console.WriteLine(last);\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1100\nz\n");
}

// `: string` through interpolation of the `T`-typed result, at T = int (the
// review's q07 asked for the same observation through a `string` local).
BEZEL_TEST(codegen_linq_generic_body_sum_type_param_result_8266,
           string_body_interpolates_the_t_sum) {
    JitResult r = runAotChecked(
        "module M;\n"
        "class C\n"
        "  public static method S<T>(a: Array<T>): string where T: INumber<T> =>"
        " $\"{a.Sum()}\";\n"
        "  public static method Main()\n"
        "    var a := new Array<int>();\n"
        "    a.Add(5); a.Add(6);\n"
        "    Console.WriteLine(S(a));\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\n");
}

// fix #8253 (stdlib §5.1, spec §11.8 / §14.2, v0.355): THE ISSUE'S PROGRAM.
// A no-selector `Sum()` in a generic body over `Array<T> where T: INumber<T>`,
// at T = double. Pre-fix the accumulator was typed from the UNRESOLVED `T`
// (the `int` fallback), so the emitted IR mixed the two widths —
//
//     Both operands to a binary operator are not of the same type!
//       %17 = add i32 %16, double %15
//     Call parameter type does not match function signature!
//       %sum.r = load i32, ptr %0, align 4
//     double  %dbl.r0 = call ptr @bz_double_to_string_r(i32 %sum.r)
//     bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// — and the COMPILE ABORTED. stdlib §5.1's `Sum(): owned T` accumulates in T's
// own type (spec §11.8's model: `T.Zero()`, then `+`), so at T = double the
// total is 1.5 + 2.25 = 3.75.
BEZEL_TEST(codegen_linq_generic_body_sum_type_param_result_8266,
           t_body_at_double_prints_the_sum) {
    JitResult r = runAot(
        "module M;\n"
        "public class Program\n"
        "  static method H<T>(a: Array<T>): T where T: INumber<T> => a.Sum();\n"
        "  public static method Main()\n"
        "    var a := new Array<double>();\n"
        "    a.Add(1.5); a.Add(2.25);\n"
        "    Console.WriteLine(H(a));\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3.75\n");
}

// fix #8253 (stdlib §5.1, spec §11.8, v0.355): the same generic body at
// T = float. The `float` accumulator is the second FP width the unresolved
// `T` broke (`add i32, float`), so the row is a second instance of the same
// defect at a different element width — 1.5f + 2.25f = 3.75.
BEZEL_TEST(codegen_linq_generic_body_sum_type_param_result_8266,
           t_body_at_float_prints_the_sum) {
    JitResult r = runAot(
        "module M;\n"
        "public class Program\n"
        "  static method H<T>(a: Array<T>): T where T: INumber<T> => a.Sum();\n"
        "  public static method Main()\n"
        "    var a := new Array<float>();\n"
        "    a.Add(1.5f); a.Add(2.25f);\n"
        "    Console.WriteLine(H(a));\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3.75\n");
}

// fix #8253 (stdlib §5.1, spec §11.8, v0.355): the 64-bit integer width, and
// the row that shows the defect was not only a COMPILE abort but a SILENT
// MISCOMPILE: the unresolved `T` gave an `i32` accumulator, and the element
// (i64) was truncated into it, so 3e9 + 6e9 printed 410_065_408 (9e9 mod
// 2^32) instead of 9_000_000_000. stdlib §5.1: `Array<long>.Sum(): long`.
BEZEL_TEST(codegen_linq_generic_body_sum_type_param_result_8266,
           t_body_at_long_is_not_truncated) {
    JitResult r = runAot(
        "module M;\n"
        "public class Program\n"
        "  static method H<T>(a: Array<T>): T where T: INumber<T> => a.Sum();\n"
        "  public static method Main()\n"
        "    var a := new Array<long>();\n"
        "    a.Add(3000000000L); a.Add(6000000000L);\n"
        "    Console.WriteLine(H(a));\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "9000000000\n");
}

// fix #8253 (stdlib §5.1, spec §11.8, v0.355): the UNSIGNED 64-bit width.
// The accumulator must be unsigned at its own width (spec §6.1's checked
// `+`), so 5e9 + 5e9 = 10_000_000_000 — which the pre-fix `i32` accumulator
// reported as 1_410_065_408.
BEZEL_TEST(codegen_linq_generic_body_sum_type_param_result_8266,
           t_body_at_ulong_is_not_truncated) {
    JitResult r = runAot(
        "module M;\n"
        "public class Program\n"
        "  static method H<T>(a: Array<T>): T where T: INumber<T> => a.Sum();\n"
        "  public static method Main()\n"
        "    var a := new Array<ulong>();\n"
        "    a.Add(5000000000UL); a.Add(5000000000UL);\n"
        "    Console.WriteLine(H(a));\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "10000000000\n");
}

// fix #8253 (stdlib §5.1, spec §11.8, v0.355): the NARROW-integer width.
// stdlib §5: "a narrow integer element type accumulates in that type" — so
// the accumulator is an i8, not the `i32` the unresolved `T` gave it (whose
// reconciliation spelled the i8→i32 WIDENING as a `trunc i8 to i32`, which
// LLVM's verifier rejects — the same "DestTy too big for Trunc" abort the
// concrete-receiver row of #4695 records). 200 + 50 = 250, in range for byte.
BEZEL_TEST(codegen_linq_generic_body_sum_type_param_result_8266,
           t_body_at_byte_accumulates_in_byte) {
    JitResult r = runAot(
        "module M;\n"
        "public class Program\n"
        "  static method H<T>(a: Array<T>): T where T: INumber<T> => a.Sum();\n"
        "  public static method Main()\n"
        "    var a := new Array<byte>();\n"
        "    a.Add(byte(200)); a.Add(byte(50));\n"
        "    Console.WriteLine(H(a));\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "250\n");
}

// fix #8253 (stdlib §5, spec §6.1, v0.355): the narrow-integer width is
// overflow-CHECKED IN ITSELF (stdlib §5: "`bytes.Sum()` over an `Array<byte>`
// raises `OverflowException` past 255"), so the generic body's accumulator
// must be the UNSIGNED i8 add — 200 + 200 = 400 raises. This row also pins the
// SIGNEDNESS of the accumulator at the narrow width (byte and sbyte are the
// same i8), which the in-range row above cannot see.
BEZEL_TEST(codegen_linq_generic_body_sum_type_param_result_8266,
           t_body_at_byte_overflow_is_checked_in_byte) {
    JitResult r = runAot(
        "module M;\n"
        "public class Program\n"
        "  static method H<T>(a: Array<T>): T where T: INumber<T> => a.Sum();\n"
        "  public static method Main()\n"
        "    var a := new Array<byte>();\n"
        "    a.Add(byte(200)); a.Add(byte(200));\n"
        "    try\n"
        "      Console.WriteLine(H(a));\n"
        "    catch\n"
        "      OverflowException => Console.WriteLine(\"caught\")\n"
        "    end;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "caught\n");
}
