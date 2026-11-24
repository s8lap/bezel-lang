// fix #8690 (stdlib §5, spec §3.6 rule 2, §14.2 *Interface conformance*,
// §22.6, v0.359): the SELECTOR forms of `Sum` and `Average` check the
// selector's RESULT type against the declared signature.
//
// SPEC BASIS (QUOTED, v0.359).
//   stdlib §5 (Aggregation): "public method Sum<U>(selector: (T) -> U): owned U
//     where U: INumber<U>;" and the seven `Average(selector: (T) -> int | long
//     | uint | ulong | float | double | decimal)` overloads (no generic one).
//   spec §3.6 rule 2: "A generic body type-checks under the weakest assumption
//     its constraints establish" -- a `U` the body's constraints do not bound
//     by `INumber<U>` does not satisfy `U: INumber<U>`.
//   stdlib §5.1 / #4730: a nullable `T?` does not satisfy `T: INumber<T>`.
//
// THE DEFECT. The selector aggregates admitted a `U` that fails the bound:
// `Sum` at an unconstrained type parameter or a nullable result, and `Average`
// at a type-parameter selector (no overload takes one). The unbounded call
// compiled and printed through the wrong arm. Every REJECT row below is RED on
// the parent (the program compiled and ran).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

const char* kSumBound = "Sum(selector) requires the selector's result type to "
                        "satisfy `U: INumber<U>`";
const char* kAvgNoOverload = "no overload of Average(selector) applies";

void expectRejected(const JitResult& r, const char* diag) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, diag));
}

}  // namespace

// The issue's repro (Symptom 1): a type-parameter selector result whose
// constraint (`notnull`) does not guarantee `INumber<T>`. PARENT: compiled and
// printed 3.75.
BEZEL_TEST(codegen_linq_selector_aggregate_bound_8690,
           sum_selector_at_type_param_without_inumber_bound_rejected_spec_3_6) {
    expectRejected(
        runAot("module M;\n"
               "public class Program\n"
               "  static method H<T>(a: Array<T>): T where T: notnull =>"
               " a.Sum(lambda(x) => x);\n"
               "  public static method Main()\n"
               "    var a := new Array<double>();\n"
               "    a.Add(1.5); a.Add(2.25);\n"
               "    Console.WriteLine(H(a));\n"
               "  end Main;\n"
               "end Program;\n"),
        kSumBound);
}

// The UNCONSTRAINED type-parameter spelling. PARENT: compiled.
BEZEL_TEST(codegen_linq_selector_aggregate_bound_8690,
           sum_selector_at_unconstrained_type_param_rejected_spec_3_6) {
    expectRejected(
        runAot("module M;\n"
               "public class Program\n"
               "  static method H<T>(a: Array<T>): T => a.Sum(lambda(x) => x);\n"
               "  public static method Main()\n"
               "    var a := new Array<double>();\n"
               "    a.Add(1.5); a.Add(2.25);\n"
               "    Console.WriteLine(H(a));\n"
               "  end Main;\n"
               "end Program;\n"),
        kSumBound);
}

// The vtable-backed `IEnumerable<T>` receiver arm. PARENT: compiled.
BEZEL_TEST(codegen_linq_selector_aggregate_bound_8690,
           sum_selector_at_type_param_without_bound_ienumerable_receiver_rejected) {
    expectRejected(
        runAot("module M;\n"
               "public class Program\n"
               "  static method H<T>(a: IEnumerable<T>): T where T: notnull =>"
               " a.Sum(lambda(x) => x);\n"
               "  public static method Main()\n"
               "    var a := new Array<double>();\n"
               "    a.Add(1.5);\n"
               "    Console.WriteLine(H(a));\n"
               "  end Main;\n"
               "end Program;\n"),
        kSumBound);
}

// A NULLABLE selector result (lazy receiver). PARENT: compiled and printed 7.
BEZEL_TEST(codegen_linq_selector_aggregate_bound_8690,
           sum_selector_with_nullable_result_rejected_stdlib_5_1) {
    expectRejected(
        runAot("module M;\n"
               "public class Program\n"
               "  public static method Main()\n"
               "    var a := new Array<int>();\n"
               "    a.Add(3); a.Add(4);\n"
               "    Console.WriteLine(a.Where(lambda(x) => true)"
               ".Sum(lambda(x) => x as int?));\n"
               "  end Main;\n"
               "end Program;\n"),
        kSumBound);
}

// The concrete-array receiver arm of the nullable spelling.
BEZEL_TEST(codegen_linq_selector_aggregate_bound_8690,
           sum_selector_with_nullable_result_concrete_array_rejected_stdlib_5_1) {
    expectRejected(
        runAot("module M;\n"
               "public class Program\n"
               "  public static method Main()\n"
               "    var a := new Array<int>();\n"
               "    a.Add(3); a.Add(4);\n"
               "    Console.WriteLine(a.Sum(lambda(x) => x as int?));\n"
               "  end Main;\n"
               "end Program;\n"),
        kSumBound);
}

// A `bool` selector result is no `INumber`. PARENT: compiled.
BEZEL_TEST(codegen_linq_selector_aggregate_bound_8690,
           sum_selector_with_bool_result_rejected_stdlib_5) {
    expectRejected(
        runAot("module M;\n"
               "public class Program\n"
               "  public static method Main()\n"
               "    var a := new Array<int>();\n"
               "    a.Add(3); a.Add(4);\n"
               "    Console.WriteLine(a.Sum(lambda(x) => x > 3));\n"
               "  end Main;\n"
               "end Program;\n"),
        kSumBound);
}

// `Average` has no generic overload: a type-parameter selector result is no
// overload at all, even at `where T: INumber<T>`. PARENT: compiled, printed
// 1.875.
BEZEL_TEST(codegen_linq_selector_aggregate_bound_8690,
           average_selector_at_type_param_rejected_stdlib_5) {
    expectRejected(
        runAot("module M;\n"
               "public class Program\n"
               "  static method H<T>(a: Array<T>): double where T: INumber<T> =>"
               " a.Average(lambda(x) => x);\n"
               "  public static method Main()\n"
               "    var a := new Array<double>();\n"
               "    a.Add(1.5); a.Add(2.25);\n"
               "    Console.WriteLine(H(a));\n"
               "  end Main;\n"
               "end Program;\n"),
        kAvgNoOverload);
}

// `Average` at a nullable selector result (lazy receiver).
BEZEL_TEST(codegen_linq_selector_aggregate_bound_8690,
           average_selector_with_nullable_result_rejected_stdlib_5) {
    expectRejected(
        runAot("module M;\n"
               "public class Program\n"
               "  public static method Main()\n"
               "    var a := new Array<int>();\n"
               "    a.Add(3); a.Add(4);\n"
               "    Console.WriteLine(a.Where(lambda(x) => true)"
               ".Average(lambda(x) => x as int?));\n"
               "  end Main;\n"
               "end Program;\n"),
        kAvgNoOverload);
}
