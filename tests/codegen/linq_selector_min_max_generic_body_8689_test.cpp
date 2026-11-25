// fix #8689 (stdlib §5, spec §3.6 rule 2, §14.1, §14.2, v0.359): the SELECTOR
// form of `Min`/`Max` accumulates in the selector's return type `U` read
// through the GENERIC SUBSTITUTION, so a generic body instantiated at a
// non-`int` type no longer emits malformed IR.
//
// SPEC BASIS (QUOTED, v0.359).
//   stdlib §5 (Aggregation): "public method Min<U>(selector: (T) -> U): U
//     where U: IComparable<U>;" and "public method Max<U>(selector: (T) -> U):
//     U where U: IComparable<U>;"
//     — so the SELECTOR form's result type is the selector's return type `U`.
//   spec §3.6 rule 2 (Derived type properties, common rules): "For a generic
//     type or member, the property is evaluated at each instantiation, after
//     substituting the type arguments. A generic body type-checks under the
//     weakest assumption its constraints establish".
//   spec §14.1 (Generic declarations): the monomorphised body is the one that
//     runs, so the substitution is what its reads must see.
//
// THE DEFECT — the Min/Max twin of #8338 (the `Sum` selector). Codegen's
// selector `Min`/`Max` accumulator read the selector's return type WITHOUT
// `resolveTypeParamRef`, so inside a generic body the PLACEHOLDER `T` — a
// `Kind::Named` type parameter, never a `Kind::Primitive` — fell through to
// the `int` DEFAULT, and the emitted IR mixed an i32 accumulator with the
// INSTANTIATED use site:
//
//   %mm.r = load i32, ptr %1, align 4
//   double %dbl.r0 = call ptr @bz_double_to_string_r(i32 %mm.r)
//   bezelc: codegen error: IR verification failed (malformed IR emitted; see
//            report above)
//
// MEASURED on the base (r30-c1 @ c00f5a63f), each row below RED:
//   * T = double and T = float — the compiler ABORTED in the LLVM IR
//     verifier (the message above);
//   * T = long — the total was TRUNCATED to 32 bits: 6000000000 printed as
//     1705032704;
//   * T = string — the POINTER was truncated to 32 bits: "zzz" printed as
//     3883072;
//   * the LAZY twin (`a.Where(lambda(x) => true).Max(lambda(x) => x)`) failed
//     the same way as the concrete receiver.
//
// Sema already types the call `U` (`: int => a.Max(lambda(x) => x)` is
// refused with the §5.1/§14.2 type-parameter diagnostic), so this is a
// CODEGEN defect: the same typing #8338 applied to the `Sum` arm.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// A generic body `H<T>(a: Array<T>): T where T: IComparable<T>` whose body is
// `body`, called on an `Array<elem>` filled by `adds`, printing the result.
std::string genericProgram(const char* elem, const char* adds,
                           const char* body) {
    return std::string("module M;\n"
                       "public class Program\n"
                       "  static method H<T>(a: Array<T>): T\n"
                       "    where T: IComparable<T> => ") +
           body +
           ";\n"
           "  public static method Main()\n"
           "    var a := new Array<" +
           elem + ">();\n    " + adds +
           "\n"
           "    Console.WriteLine(H(a));\n"
           "  end Main;\n"
           "end Program;\n";
}

// The same shape with the `Max`/`Min` call written out at the CONCRETE
// receiver (no generic body at all) — the twin the fix must leave alone.
std::string concreteProgram(const char* elem, const char* adds,
                            const char* body) {
    return std::string("module M;\n"
                       "public class Program\n"
                       "  public static method Main()\n"
                       "    var a := new Array<") +
           elem +
           ">();\n    " + adds +
           "\n"
           "    Console.WriteLine(" + body +
           ");\n"
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
// The issue's repro, verbatim shape: the SELECTOR `Max` in a generic body at
// T = double.
// BASE: the compiler aborted in the LLVM IR verifier ("%mm.r = load i32" fed
// to `bz_double_to_string_r(double)`); the program never ran.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_selector_min_max_generic_body_8689,
           selector_max_generic_body_double_is_not_malformed_ir_spec_3_6) {
    expectPrints(runAot(genericProgram("double", "a.Add(1.5); a.Add(2.25);",
                                        "a.Max(lambda(x) => x)")),
                 "2.25\n");
}

// ---------------------------------------------------------------------------
// The `Min` sibling of the same row.
// BASE: the same IR-verification abort.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_selector_min_max_generic_body_8689,
           selector_min_generic_body_double_is_not_malformed_ir_spec_3_6) {
    expectPrints(runAot(genericProgram("double", "a.Add(1.5); a.Add(2.25);",
                                        "a.Min(lambda(x) => x)")),
                 "1.5\n");
}

// ---------------------------------------------------------------------------
// The same shape at T = float.
// BASE: the same IR-verification abort.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_selector_min_max_generic_body_8689,
           selector_max_generic_body_float_is_not_malformed_ir_spec_3_6) {
    expectPrints(runAot(genericProgram("float", "a.Add(1.5f); a.Add(2.25f);",
                                        "a.Max(lambda(x) => x)")),
                 "2.25\n");
}

// ---------------------------------------------------------------------------
// The same shape at T = long — the accumulator must be 64 bits wide.
// BASE: 6000000000 printed as 1705032704 (truncated to 32 bits).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_selector_min_max_generic_body_8689,
           selector_max_generic_body_long_is_not_truncated_spec_3_6) {
    expectPrints(runAot(genericProgram("long",
                                        "a.Add(3000000000L); a.Add(6000000000L);",
                                        "a.Max(lambda(x) => x)")),
                 "6000000000\n");
}

// ---------------------------------------------------------------------------
// The LAZY receiver twin the issue names: `a.Where(lambda(x) => true).Max(…)`.
// BASE: the same IR-verification abort.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_selector_min_max_generic_body_8689,
           selector_max_lazy_generic_body_double_is_not_malformed_ir_spec_3_6) {
    expectPrints(
        runAot(genericProgram("double", "a.Add(1.5); a.Add(2.25);",
                               "a.Where(lambda(x) => true).Max(lambda(x) => x)")),
        "2.25\n");
}

// ---------------------------------------------------------------------------
// T = string: the selector's `U` is a POINTER, and the accumulator must hold
// the pointer (and order by the §9.6 string relation), not a 32-bit word.
// BASE: "zzz" printed as 3883072 (the pointer truncated).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_selector_min_max_generic_body_8689,
           selector_max_generic_body_string_is_not_truncated_spec_3_6) {
    expectPrints(runAot(genericProgram("string", "a.Add(\"aaa\"); a.Add(\"zzz\");",
                                        "a.Max(lambda(x) => x)")),
                 "zzz\n");
}

// ---------------------------------------------------------------------------
// CONTROL 1 — the `int` instantiation, which the fix must leave EXACTLY as it
// was: `int` is the accumulator's default type, so a fix that keyed the
// accumulator on something else would change this answer.
// BASE: 9 (correct).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_selector_min_max_generic_body_8689,
           control_selector_max_generic_body_int_is_unchanged_spec_5) {
    expectPrints(runAot(genericProgram("int", "a.Add(3); a.Add(9);",
                                        "a.Max(lambda(x) => x)")),
                 "9\n");
}

// ---------------------------------------------------------------------------
// CONTROL 2 — the CONCRETE receiver, outside any generic body: the selector's
// `U` was already read from a concrete type, so this path must be untouched
// by the fix.
// BASE: 2.25 (correct).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_selector_min_max_generic_body_8689,
           control_selector_max_concrete_double_is_unchanged_spec_5) {
    expectPrints(runAot(concreteProgram("double", "a.Add(1.5); a.Add(2.25);",
                                         "a.Max(lambda(x) => x)")),
                 "2.25\n");
}
