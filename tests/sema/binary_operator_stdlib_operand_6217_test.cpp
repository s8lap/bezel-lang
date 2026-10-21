// fix #6217 (§25.2 "Restrictions and resolution", §5.1 "Reference conversions",
// v0.339): an operator's parameter is matched against its operand "by the
// ordinary argument rules" (§25.2), i.e. the §5.1 conversions a call
// `F(p: T)` admits. The binary form graded two declaration-less stdlib types
// as Exact by declaration-pointer equality (nullptr == nullptr) and ignored
// generic arguments, so a non-convertible operand selected the user operator
// and the program ran (printing 1001). The compound form (#6091) graded them
// by a closed name table instead, which rejected legal conversions the call
// admits (SortedSet<int> / ICollection<int> -> IEnumerable<int>,
// Span<int> -> ReadOnlySpan<int>, a user exception -> Exception). Both forms
// now share one grade (ExprTC::operatorNamedOperandMatch).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {
const char* kDecls =
    "public interface IA end IA; "
    "public class Animal: IA end Animal; "
    "public class Dog: Animal end Dog; "
    "public class Box<T> end Box; "
    "public class MyEx: Exception "
    "  public constructor(m: string) base(m); end constructor; "
    "end MyEx; ";
// `operator +(v: V, p: <param>)` adding 1000; `stmt` uses it on `v` and `r`.
std::string prog(const std::string& param, const std::string& setup,
                 const std::string& stmt) {
    return std::string("module M; ") + kDecls +
           "record V(X: int) "
           "  public static method operator +(v: V, p: " + param +
           "): V => new V(v.X + 1000); "
           "end V; "
           "public class Program "
           "  public static method Main(): int "
           "    var v := new V(1); " + setup + " " + stmt +
           "    Console.WriteLine(v.X); return 0; "
           "  end Main; "
           "end Program;";
}
std::string binary(const std::string& param, const std::string& setup) {
    return prog(param, setup, "v := v + r; ");
}
std::string compound(const std::string& param, const std::string& setup) {
    return prog(param, setup, "v += r; ");
}
const char* kNoBinaryOp = "operator `+` is not defined for type `V`";
const char* kNoCompoundOp =
    "compound assignment `+=` has no valid result on record type `V`";

void expectBinaryRejected(const std::string& param, const std::string& setup) {
    auto r = runAot(binary(param, setup));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNoBinaryOp));
}
void expectBinaryRuns(const std::string& param, const std::string& setup) {
    auto r = runAot(binary(param, setup));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1001\n");
}
// The compound form's codegen does not yet lower a user operator whose
// right operand is a declaration-less, interface or `Object`-typed value (IR
// verification, #6656), so its accept rows assert the sema verdict through
// the gated sema-only typecheck, as #6091's a3-a5 rows do. Not vacuous: the
// same gate reports the compound rejection rows below.
void expectCompoundSemaAccepts(const std::string& param,
                               const std::string& setup) {
    auto r = bezel::test::parseAndTypecheckGated(compound(param, setup));
    BEZEL_EXPECT_TRUE(static_cast<bool>(r.sourceFile));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
void expectCompoundRejected(const std::string& param,
                            const std::string& setup) {
    auto r = runAot(compound(param, setup));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNoCompoundOp));
}
}  // namespace

// ---- binary form: rejections (§25.2 "No applicable operator") -------------

// The issue's three repros.
BEZEL_TEST(binary_operator_stdlib_operand_6217,
           r1_stringbuilder_into_ienumerable_int_spec_25_2) {
    expectBinaryRejected("IEnumerable<int>", "var r := new StringBuilder();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           r2_exception_into_stringbuilder_spec_25_2) {
    expectBinaryRejected("StringBuilder", "var r := new Exception(\"e\");");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           r3_array_string_into_ienumerable_int_spec_25_2) {
    expectBinaryRejected("IEnumerable<int>", "var r := new Array<string>();");
}

// Same head, different type arguments: §12.9, `Array<T>` is invariant and
// `IEnumerable<out T>` is covariant only over reference-typed arguments.
BEZEL_TEST(binary_operator_stdlib_operand_6217,
           r4_array_string_into_array_int_spec_5_1) {
    expectBinaryRejected("Array<int>", "var r := new Array<string>();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           r5_ienumerable_string_into_ienumerable_int_spec_5_1) {
    expectBinaryRejected("IEnumerable<int>",
                         "var r: IEnumerable<string> := new Array<string>();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           r6_array_long_into_ienumerable_int_spec_5_1) {
    expectBinaryRejected("IEnumerable<int>", "var r := new Array<long>();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           r7_random_into_stringbuilder_spec_5_1) {
    expectBinaryRejected("StringBuilder", "var r := new Random();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           r8_array_dog_into_array_animal_spec_12_9) {
    expectBinaryRejected("Array<Animal>", "var r := new Array<Dog>();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           r9_user_generic_box_int_into_box_string_spec_12_9) {
    expectBinaryRejected("Box<string>", "var r := new Box<int>();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           r10_readonly_span_into_span_spec_5_1) {
    expectBinaryRejected("Span<int>",
                         "using var buf := new OwningBuffer<int>(4); "
                         "var r := buf.AsReadOnlySpan();");
}

// ---- binary form: legal §5.1 conversions run ------------------------------
// The operator bodies ignore `p`: a body that USES a stdlib collection
// operand through an interface parameter crashes at run time, pre-existing
// (#6655), which these rows do not exercise.

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           a1_sortedset_into_ienumerable_spec_5_1) {
    expectBinaryRuns("IEnumerable<int>", "var r := new SortedSet<int>();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           a2_icollection_into_ienumerable_spec_5_1) {
    expectBinaryRuns("IEnumerable<int>",
                     "var r: ICollection<int> := new Array<int>();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           a3_ireadonlylist_into_ienumerable_spec_5_1) {
    expectBinaryRuns("IEnumerable<int>",
                     "var r: IReadOnlyList<int> := new Array<int>();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           a4_span_into_readonly_span_spec_5_1) {
    expectBinaryRuns("ReadOnlySpan<int>",
                     "using var buf := new OwningBuffer<int>(4); "
                     "var r := buf.AsSpan();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           a5_owning_array_into_ienumerable_spec_5_1) {
    expectBinaryRuns("IEnumerable<Dog>", "using var r := new OwningArray<Dog>();");
}

// §5.1 "including the compiler-generated `IDisposable`" (§28.2): every class.
BEZEL_TEST(binary_operator_stdlib_operand_6217,
           a6_stdlib_class_into_idisposable_spec_28_2) {
    expectBinaryRuns("IDisposable", "var r := new Array<int>();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           a7_user_class_into_idisposable_spec_28_2) {
    expectBinaryRuns("IDisposable", "var r := new Dog();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           a8_array_dog_into_ienumerable_animal_spec_12_9) {
    expectBinaryRuns("IEnumerable<Animal>", "var r := new Array<Dog>();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           a9_user_exception_into_exception_spec_5_1) {
    expectBinaryRuns("Exception", "var r := new MyEx(\"e\");");
}

// ---- compound form agrees (§6.6: `x op= e` is `x := x op e`) -------------

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           c1_compound_sortedset_into_ienumerable_sema_accepts_spec_5_1) {
    expectCompoundSemaAccepts("IEnumerable<int>",
                              "var r := new SortedSet<int>();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           c2_compound_icollection_into_ienumerable_sema_accepts_spec_5_1) {
    expectCompoundSemaAccepts("IEnumerable<int>",
                              "var r: ICollection<int> := new Array<int>();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           c3_compound_ireadonlylist_into_ienumerable_sema_accepts_spec_5_1) {
    expectCompoundSemaAccepts("IEnumerable<int>",
                              "var r: IReadOnlyList<int> := new Array<int>();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           c4_compound_span_into_readonly_span_sema_accepts_spec_5_1) {
    expectCompoundSemaAccepts("ReadOnlySpan<int>",
                              "using var buf := new OwningBuffer<int>(4); "
                              "var r := buf.AsSpan();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           c5_compound_owning_array_into_ienumerable_sema_accepts_spec_5_1) {
    expectCompoundSemaAccepts("IEnumerable<Dog>",
                              "using var r := new OwningArray<Dog>();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           c6_compound_user_exception_into_exception_sema_accepts_spec_5_1) {
    expectCompoundSemaAccepts("Exception", "var r := new MyEx(\"e\");");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           c7_compound_user_class_into_object_sema_accepts_spec_5_1) {
    // The binary form's #6005 `Object` rule, now shared.
    expectCompoundSemaAccepts("Object", "var r := new Dog();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           c8_compound_user_generic_box_int_into_box_string_spec_12_9) {
    expectCompoundRejected("Box<string>", "var r := new Box<int>();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           c9_compound_array_string_into_array_int_spec_5_1) {
    expectCompoundRejected("Array<int>", "var r := new Array<string>();");
}

BEZEL_TEST(binary_operator_stdlib_operand_6217,
           c10_compound_stringbuilder_into_ienumerable_int_spec_25_2) {
    expectCompoundRejected("IEnumerable<int>", "var r := new StringBuilder();");
}
