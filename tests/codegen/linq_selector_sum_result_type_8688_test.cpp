// fix #8688 (stdlib §5, spec §3.6 rule 2, §5.1, §14.2, v0.357): the SELECTOR
// form of `Sum` is typed as the selector's return type `U` on every receiver
// arm, not as `int`.
//
// SPEC BASIS (QUOTED, v0.357).
//   stdlib §5 (Aggregation): "public method Sum<U>(selector: (T) -> U): owned U
//     where U: INumber<U>;"
//   spec §3.6 rule 2: "A generic body type-checks under the weakest assumption
//     its constraints establish".
//   spec §5.1: a type parameter converts implicitly only "to an interface or
//     class its constraints guarantee conformance to" -- never to a primitive.
//
// THE DEFECT (the selector twin of #8266). sema typed the selector Sum's
// result `int` wherever the selector's return was not a primitive it could
// classify, and the lazy, vtable-backed `IEnumerable<T>` and `string` receiver
// arms typed EVERY selector Sum `int` (or `decimal`). Codegen accumulates in
// the selector's real type (#1757, #8338), so the disagreement surfaced at the
// use site. MEASURED on the parent (r30-c1 @ 978f399cf):
//   * in a generic body, `: int => a.Sum(lambda(x) => x)` at T = double was
//     ACCEPTED and printed 3 for 1.5 + 2.25 (each receiver arm: concrete,
//     lazy, `IEnumerable<T>`, a `Box<T>` field selector, a class-level `T`);
//   * `var s := a.Sum(lambda(x) => x)` in a generic body at T = double printed
//     3 through interpolation;
//   * OUTSIDE any generic body, the lazy / ordered / range / grouping /
//     `IEnumerable<T>` / `string` receivers printed 410065408 for a `long`
//     selector summing 9e9, 5 for a `double` one summing 3.75, 33 for a
//     `decimal` one summing 3.3.
//
// Every row below is RED on the parent.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The spec §5.1 / §14.2 type-parameter-to-primitive diagnostic (the #8266 q03
// wording).
const char* kTypeParamToPrimitive =
    "`T` is a type parameter, and a generic body converts a type-parameter "
    "value only to `Object` and to the bounds its constraints guarantee";

// A generic carrier `H` returning `ret` from `body`, called on an
// `Array<double>` holding 1.5 and 2.25.
std::string genericProgram(const char* param, const char* ret,
                           const char* body) {
    return std::string("module M;\n"
                       "public record Box<T>\n"
                       "  public V: T;\n"
                       "end Box;\n"
                       "public class Program\n"
                       "  static method H<T>(a: ") +
           param + "): " + ret + " where T: INumber<T> => " + body +
           ";\n"
           "  public static method Main()\n"
           "    var a := new Array<double>();\n"
           "    a.Add(1.5); a.Add(2.25);\n"
           "    Console.WriteLine(H(a));\n"
           "  end Main;\n"
           "end Program;\n";
}

void expectTypeParamToPrimitiveRejected(const JitResult& r) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the returned value has a type that is not implicitly convertible"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kTypeParamToPrimitive));
}

// `Main` body over `var a := new Array<elem>()` filled by `fill`, printing
// `expr`.
std::string concreteProgram(const char* elem, const char* fill,
                            const char* expr) {
    return std::string("module M;\n"
                       "public class Program\n"
                       "  public static method Main()\n"
                       "    var a := new Array<") +
           elem + ">();\n    " + fill +
           "\n"
           "    var t := " + expr + ";\n"
           "    Console.WriteLine(t);\n"
           "  end Main;\n"
           "end Program;\n";
}

void expectPrints(const JitResult& r, const char* out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// ---------------------------------------------------------------------------
// The issue's repro (Symptom A), verbatim shape: concrete `Array<T>` receiver.
// PARENT: compiled, printed 3.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_selector_sum_result_type_8688,
           issue_repro_int_return_of_type_param_selector_sum_rejected_spec_5_1) {
    expectTypeParamToPrimitiveRejected(runAot(
        genericProgram("Array<T>", "int", "a.Sum(lambda(x) => x)")));
}

// The LAZY receiver twin. PARENT: compiled, printed 5.
BEZEL_TEST(codegen_linq_selector_sum_result_type_8688,
           lazy_receiver_int_return_of_type_param_selector_sum_rejected_spec_5_1) {
    expectTypeParamToPrimitiveRejected(runAot(genericProgram(
        "Array<T>", "int",
        "a.Where(lambda(x) => true).Sum(lambda(x) => x)")));
}

// The vtable-backed `IEnumerable<T>` receiver twin. PARENT: compiled, printed a
// garbage integer.
BEZEL_TEST(codegen_linq_selector_sum_result_type_8688,
           ienumerable_receiver_int_return_of_type_param_selector_sum_rejected_spec_5_1) {
    expectTypeParamToPrimitiveRejected(runAot(
        genericProgram("IEnumerable<T>", "int", "a.Sum(lambda(x) => x)")));
}

// `U` = `T` reached through a FIELD selector over a non-`T` element
// (`Array<Box<T>>`): the result is keyed off the selector, not the element.
// PARENT: compiled, printed 3.
BEZEL_TEST(codegen_linq_selector_sum_result_type_8688,
           field_selector_of_type_param_int_return_rejected_spec_5_1) {
    JitResult r = runAot(
        "module M;\n"
        "public record Box<T>\n"
        "  public V: T;\n"
        "end Box;\n"
        "public class Program\n"
        "  static method H<T>(a: Array<Box<T>>): int where T: INumber<T> =>"
        " a.Sum(lambda(b) => b.V);\n"
        "  public static method Main()\n"
        "    var a := new Array<Box<double>>();\n"
        "    a.Add(new Box<double>(1.5)); a.Add(new Box<double>(2.25));\n"
        "    Console.WriteLine(H(a));\n"
        "  end Main;\n"
        "end Program;\n");
    expectTypeParamToPrimitiveRejected(r);
}

// A CLASS-level type parameter. PARENT: compiled, printed 3.
BEZEL_TEST(codegen_linq_selector_sum_result_type_8688,
           class_level_type_param_int_return_rejected_spec_3_6) {
    JitResult r = runAot(
        "module M;\n"
        "public class Acc<T> where T: INumber<T>\n"
        "  public method Total(a: Array<T>): int => a.Sum(lambda(x) => x);\n"
        "end Acc;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var a := new Array<double>();\n"
        "    a.Add(1.5); a.Add(2.25);\n"
        "    Console.WriteLine(new Acc<double>().Total(a));\n"
        "  end Main;\n"
        "end Program;\n");
    expectTypeParamToPrimitiveRejected(r);
}

// The legal half, read through an inferred local: `var s := a.Sum(...)` is
// `T`, so the interpolation formats the instantiated `double`. PARENT: the
// local was `int` and printed 3.
BEZEL_TEST(codegen_linq_selector_sum_result_type_8688,
           inferred_local_of_type_param_selector_sum_keeps_the_value_spec_3_6) {
    JitResult r = runAot(
        "module M;\n"
        "public class Program\n"
        "  static method H<T>(a: Array<T>): string where T: INumber<T>\n"
        "    var s := a.Sum(lambda(x) => x);\n"
        "    return $\"{s}\";\n"
        "  end H;\n"
        "  public static method Main()\n"
        "    var a := new Array<double>();\n"
        "    a.Add(1.5); a.Add(2.25);\n"
        "    Console.WriteLine(H(a));\n"
        "  end Main;\n"
        "end Program;\n");
    expectPrints(r, "3.75\n");
}

// ---------------------------------------------------------------------------
// Concrete receivers outside any generic body: stdlib §5's `U` is the
// selector's exact numeric type on every receiver arm.
// ---------------------------------------------------------------------------

// Lazy receiver, `long` selector. PARENT: 410065408 (9e9 mod 2^32).
BEZEL_TEST(codegen_linq_selector_sum_result_type_8688,
           lazy_receiver_long_selector_is_not_truncated_stdlib_5) {
    expectPrints(runAot(concreteProgram(
                     "long", "a.Add(3000000000L); a.Add(6000000000L);",
                     "a.Where(lambda(x) => true).Sum(lambda(x) => x)")),
                 "9000000000\n");
}

// Lazy receiver, `double` selector. PARENT: 5.
BEZEL_TEST(codegen_linq_selector_sum_result_type_8688,
           lazy_receiver_double_selector_keeps_the_fraction_stdlib_5) {
    expectPrints(runAot(concreteProgram(
                     "double", "a.Add(1.5); a.Add(2.25);",
                     "a.Where(lambda(x) => true).Sum(lambda(x) => x)")),
                 "3.75\n");
}

// An ordered (`OrderBy`) receiver, `float` selector. PARENT: 5.
BEZEL_TEST(codegen_linq_selector_sum_result_type_8688,
           ordered_receiver_float_selector_keeps_the_fraction_stdlib_5) {
    expectPrints(runAot(concreteProgram(
                     "float", "a.Add(1.5f); a.Add(2.25f);",
                     "a.OrderBy(lambda(x) => x).Sum(lambda(x) => x)")),
                 "3.75\n");
}

// A range receiver, an `int` element projected to `double`. PARENT: 5.
BEZEL_TEST(codegen_linq_selector_sum_result_type_8688,
           range_receiver_double_selector_over_int_elements_stdlib_5) {
    expectPrints(runAot(concreteProgram("int", "",
                                        "(1..3).Sum(lambda(x) => x * 1.5)")),
                 "9\n");
}

// A grouping receiver, `long` selector. PARENT: -589934592.
BEZEL_TEST(codegen_linq_selector_sum_result_type_8688,
           grouping_receiver_long_selector_is_not_truncated_stdlib_5) {
    expectPrints(
        runAot(concreteProgram(
            "int", "a.Add(3); a.Add(4); a.Add(5);",
            "a.GroupBy(lambda(x) => x mod 2).First()"
            ".Sum(lambda(x) => x * 1000000000L)")),
        "8000000000\n");
}

// The vtable-backed `IEnumerable<T>` receiver, `ulong` selector.
// PARENT: -1981284351.
BEZEL_TEST(codegen_linq_selector_sum_result_type_8688,
           ienumerable_receiver_ulong_selector_is_not_truncated_stdlib_5) {
    JitResult r = runAot(
        "module M;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var a := new Array<ulong>();\n"
        "    a.Add(10000000000000000000UL); a.Add(1UL);\n"
        "    var e: IEnumerable<ulong> := a;\n"
        "    var t := e.Sum(lambda(x) => x);\n"
        "    Console.WriteLine(t);\n"
        "  end Main;\n"
        "end Program;\n");
    expectPrints(r, "10000000000000000001\n");
}

// The `string` receiver (stdlib §9.6; `IEnumerable<char>`, §4.12), `long` selector.
// PARENT: 1705032704.
BEZEL_TEST(codegen_linq_selector_sum_result_type_8688,
           string_receiver_long_selector_is_not_truncated_stdlib_5) {
    expectPrints(runAot(concreteProgram(
                     "int", "",
                     "\"ab\".Sum(lambda(c) => 3000000000L)")),
                 "6000000000\n");
}

// The `string` receiver, `decimal` selector. PARENT: 33.
BEZEL_TEST(codegen_linq_selector_sum_result_type_8688,
           string_receiver_decimal_selector_keeps_the_scale_stdlib_5) {
    expectPrints(runAot(concreteProgram("int", "",
                                        "\"abc\".Sum(lambda(c) => 1.1m)")),
                 "3.3\n");
}
