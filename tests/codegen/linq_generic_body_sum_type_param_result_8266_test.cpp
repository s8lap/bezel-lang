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

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
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
