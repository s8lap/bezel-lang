// fix #8266 (stdlib §5.1, spec §3.6 rule 2, §5.1, §14.2, v0.355): inside a
// GENERIC body whose receiver element is the body's own type parameter, the
// no-selector `Sum()` (and `Min()` / `Max()`) is typed as that parameter `T`,
// not `int`.
//
// SPEC BASIS (QUOTED, v0.355).
//   stdlib §5.1: "public extension IEnumerable<T> where T: INumber<T>
//     public method Sum(): owned T;" and "public extension IEnumerable<T>
//     where T: IComparable<T>  public method Min(): T;  public method Max(): T;"
//   spec §3.6 rule 2: "A generic body type-checks under the weakest assumption
//     its constraints establish".
//   spec §5.1 / §14.2 (the existing diagnostic's wording): a generic body
//     converts a type-parameter value only to `Object` and to the bounds its
//     constraints guarantee -- never to a primitive.
//
// MEASURED ON THE PARENT (r30-c1 @ b2e70429): `G<T>(a: Array<T>): int where
// T: INumber<T> => a.Sum()` was ACCEPTED (the call typed `int`); at a user
// `INumber<M2>` record it printed a different garbage integer on every run.
// The control `G<T>(x: T): int where T: INumber<T> => x` was already rejected
// with the diagnostic these rows now expect.
//
// Run-time rows (T = int, a user record, string interpolation) live in
// tests/codegen/linq_generic_body_sum_type_param_result_8266_test.cpp.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The q03 diagnostic (spec §5.1 / §14.2): the conversion from a type-parameter
// value to a primitive is refused.
const char* kTypeParamToPrimitive =
    "`T` is a type parameter, and a generic body converts a type-parameter "
    "value only to `Object` and to the bounds its constraints guarantee";

const char* kM2 =
    "public record M2(Label: string, Cents: int): INumber<M2>\n"
    "  public static method Zero(): M2 => new M2(\"z\", 0);\n"
    "  public static method operator +(a: M2, b: M2): M2 =>"
    " new M2(a.Label + b.Label, a.Cents + b.Cents);\n"
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

// ---------------------------------------------------------------------------
// The issue's repro (q06), verbatim: `: int => a.Sum()` at a user record.
// PARENT: compiled and printed garbage (561235200 / 545842432 / 504399104).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_linq_generic_body_sum_type_param_result_8266,
           issue_repro_int_return_of_type_param_sum_is_rejected_spec_5_1) {
    JitResult r = runAot(std::string("module Conf;\n\n") + kM2 +
        "class C\n"
        "  public static method G<T>(a: Array<T>): int where T: INumber<T> =>"
        " a.Sum();\n"
        "end C;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var a := new Array<M2>();\n"
        "    a.Add(new M2(\"a\", 5)); a.Add(new M2(\"b\", 6));\n"
        "    Console.WriteLine(C.G(a));\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the returned value has a type that is not implicitly convertible"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kTypeParamToPrimitive));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}

// The same at T = int: the body is checked once, against `T` (spec §3.6 rule
// 2), so the instantiation does not rescue it. PARENT: compiled, printed 11.
BEZEL_TEST(sema_linq_generic_body_sum_type_param_result_8266,
           int_return_of_type_param_sum_is_rejected_at_int_instantiation_spec_3_6) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method G<T>(a: Array<T>): int where T: INumber<T> =>"
        " a.Sum();\n"
        "  public static method Main()\n"
        "    var a := new Array<int>();\n"
        "    a.Add(5); a.Add(6);\n"
        "    Console.WriteLine(G(a));\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kTypeParamToPrimitive));
}

// A local initializer of type `int` (the block-body spelling of the same
// conversion). PARENT: compiled.
BEZEL_TEST(sema_linq_generic_body_sum_type_param_result_8266,
           int_local_from_type_param_sum_is_rejected_spec_5_1) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method G<T>(a: Array<T>): int where T: INumber<T>\n"
        "    var n: int := a.Sum();\n"
        "    return n;\n"
        "  end G;\n"
        "  public static method Main()\n"
        "    var a := new Array<int>();\n"
        "    a.Add(5);\n"
        "    Console.WriteLine(G(a));\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kTypeParamToPrimitive));
}

// Receiver twins: an `IEnumerable<T>` parameter (the vtable-backed arm) and a
// lazy `Where(...)` chain (the lazy arm) are typed the same way.
// PARENT: both compiled.
BEZEL_TEST(sema_linq_generic_body_sum_type_param_result_8266,
           int_return_of_type_param_sum_rejected_on_ienumerable_and_lazy_receivers) {
    JitResult r1 = runAot(
        "module M;\n"
        "class C\n"
        "  public static method G<T>(a: IEnumerable<T>): int"
        " where T: INumber<T> => a.Sum();\n"
        "  public static method Main()\n"
        "    var a := new Array<int>();\n"
        "    a.Add(5);\n"
        "    Console.WriteLine(G(a));\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r1.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r1, kTypeParamToPrimitive));
    JitResult r2 = runAot(
        "module M;\n"
        "class C\n"
        "  public static method G<T>(a: Array<T>): int where T: INumber<T> =>"
        " a.Where(lambda(x) => true).Sum();\n"
        "  public static method Main()\n"
        "    var a := new Array<int>();\n"
        "    a.Add(5);\n"
        "    Console.WriteLine(G(a));\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r2.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r2, kTypeParamToPrimitive));
}

// The sibling no-selector aggregations of stdlib §5.1, `Min(): T` / `Max():
// T` under `where T: IComparable<T>`, share the arm: an `int` return of either
// is the same refused conversion.
BEZEL_TEST(sema_linq_generic_body_sum_type_param_result_8266,
           int_return_of_type_param_min_max_is_rejected_spec_5_1) {
    for (const char* m : {"Min", "Max"}) {
        JitResult r = runAot(std::string(
            "module M;\n"
            "class C\n"
            "  public static method G<T>(a: Array<T>): int"
            " where T: IComparable<T> => a.") + m + "();\n"
            "  public static method Main()\n"
            "    var a := new Array<int>();\n"
            "    a.Add(5);\n"
            "    Console.WriteLine(G(a));\n"
            "  end Main;\n"
            "end C;\n");
        BEZEL_EXPECT_FALSE(r.ran);
        BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kTypeParamToPrimitive));
    }
}

// CONTROLS (silent-acceptance mirror): `: T` bodies and a `T` local are
// accepted, and concrete receivers keep their element type -- `Array<int>`
// sums to `int`, `Array<double>` to `double` (assigning the latter to an
// `int` stays refused).
BEZEL_TEST(sema_linq_generic_body_sum_type_param_result_8266,
           control_t_bodies_accepted_and_concrete_results_unchanged) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method G<T>(a: Array<T>): T where T: INumber<T> =>"
        " a.Sum();\n"
        "  public static method L<T>(a: Array<T>): T where T: INumber<T>\n"
        "    var s: T := a.Sum();\n"
        "    return s;\n"
        "  end L;\n"
        "  public static method Main()\n"
        "    var a := new Array<int>();\n"
        "    a.Add(5); a.Add(6);\n"
        "    var n: int := a.Sum();\n"
        "    Console.WriteLine(G(a) + L(a) + n);\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "33\n");
    JitResult d = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main()\n"
        "    var a := new Array<double>();\n"
        "    a.Add(1.5);\n"
        "    var n: int := a.Sum();\n"
        "    Console.WriteLine(n);\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(d.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(d, "not implicitly convertible"));
}
