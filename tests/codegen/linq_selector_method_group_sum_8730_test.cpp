// fix #8730 (stdlib §5, spec §27.6, §14.5, v0.359): a METHOD-GROUP selector to
// `Sum` is typed (and lowered) at the method's declared result type, exactly
// as the lambda spelling is.
//
// SPEC BASIS (QUOTED, v0.359).
//   stdlib §5: "public method Sum<U>(selector: (T) -> U): owned U where U:
//     INumber<U>;" -- the call's type is the selector's result type `U`.
//   spec §27.6: "a method reference converts to a function value of the
//     parameter's function type"; the method's declared result is `U`.
//
// THE DEFECT. A bare method-group argument carries no Function TypeRef, so the
// selector-result read (sema's concrete-array classification, codegen's
// accumulator width) found nothing and the call was typed -- and accumulated --
// as `int`. MEASURED on the parent: `a.Sum(Id)` with `Id: double -> double`
// over 1.5, 2.25 failed IR verification (`i32` fed to the `double` formatter),
// a `long` method group over 3e9 + 3e9 printed 1705032704.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

void expectPrints(const JitResult& r, const char* out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// The issue's repro: a `double` method group over an `Array<double>`.
BEZEL_TEST(codegen_linq_selector_method_group_sum_8730,
           double_method_group_selector_sums_as_double_spec_27_6) {
    expectPrints(
        runAot("module M;\n"
               "public class Program\n"
               "  static method Id(x: double): double => x;\n"
               "  public static method Main()\n"
               "    var a := new Array<double>();\n"
               "    a.Add(1.5); a.Add(2.25);\n"
               "    Console.WriteLine(a.Sum(Id));\n"
               "  end Main;\n"
               "end Program;\n"),
        "3.75\n");
}

// The lazy receiver arm (`Where(...).Sum(Id)`).
BEZEL_TEST(codegen_linq_selector_method_group_sum_8730,
           double_method_group_selector_lazy_receiver_spec_27_6) {
    expectPrints(
        runAot("module M;\n"
               "public class Program\n"
               "  static method Id(x: double): double => x;\n"
               "  public static method Main()\n"
               "    var a := new Array<double>();\n"
               "    a.Add(1.5); a.Add(2.25);\n"
               "    Console.WriteLine(a.Where(lambda(x) => true).Sum(Id));\n"
               "  end Main;\n"
               "end Program;\n"),
        "3.75\n");
}

// A `long` method group: sema types the sum `long`, so the fold must not
// truncate to `int`. PARENT: 1705032704.
BEZEL_TEST(codegen_linq_selector_method_group_sum_8730,
           long_method_group_selector_is_not_truncated_spec_27_6) {
    expectPrints(
        runAot("module M;\n"
               "public class Program\n"
               "  static method Wide(x: int): long => x * 1000000000L;\n"
               "  public static method Main()\n"
               "    var a := new Array<int>();\n"
               "    a.Add(3); a.Add(3);\n"
               "    var t := a.Sum(Wide);\n"
               "    Console.WriteLine(t);\n"
               "  end Main;\n"
               "end Program;\n"),
        "6000000000\n");
}
