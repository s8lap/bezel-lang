// fix #6091 (§6.6 "Operator overloads", §25.2 "No applicable operator", v0.338):
// a compound assignment `x op= e` desugars to `x := x op e` (§6.6), and that
// binary `x op e` is resolved per §25.2. When no `operator op` overload applies
// to the operand types in order, the use is a compile error at the expression
// (§25.2 "No applicable operator"). Before this fix the named-target compound
// check skipped every right operand whose type is a declaration-less stdlib
// class (StringBuilder, Exception, Array<int>): `v += sb` with only
// `operator +(V, Exception)` was accepted by sema and codegen then failed IR
// verification; `v >>= e` with no `operator >>` at all compiled silently.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {
// Program with one record V and an operator overload set supplied by the caller.
std::string prog(const std::string& opDecls, const std::string& body) {
    return std::string("module M; record V(X: int) ") + opDecls + " end V; " +
           "public class Program "
           "  public static method Main(): int " + body +
           "    Console.WriteLine(v.X); return 0; "
           "  end Main; "
           "end Program;";
}
const char* kExcOp =
    "public static method operator +(v: V, e: Exception): V => new V(v.X + 1000); ";
// `operator +(v: V, s: <param>)` returning V with X + 1000 (used by the
// stdlib-collection-interface rows below).
std::string opOver(const std::string& param) {
    return "public static method operator +(v: V, s: " + param +
           "): V => new V(v.X + 1000); ";
}
}  // namespace

// ---- reject rows (§6.6 / §25.2) -------------------------------------------

BEZEL_TEST(sema_compound_assign_no_applicable_operator_6091,
           r1_issue_repro_stringbuilder_over_exception_op_spec_25_2) {
    auto r = runAot(prog(kExcOp,
        "var v := new V(1); var sb := new StringBuilder(); v += sb; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "compound assignment `+=` has no valid result on record type `V`"));
}

BEZEL_TEST(sema_compound_assign_no_applicable_operator_6091,
           r2_shift_assign_with_no_shift_operator_spec_6_6) {
    auto r = runAot(prog(kExcOp,
        "var v := new V(1); var e := new Exception(\"e\"); v >>= e; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "compound assignment `>>=` has no valid result on record type `V`"));
}

BEZEL_TEST(sema_compound_assign_no_applicable_operator_6091,
           r3_base_class_operand_does_not_match_derived_param_spec_25_2) {
    // `ArgumentException` is a subtype of `Exception`, so an `Exception` operand
    // does NOT satisfy an `ArgumentException` parameter (§25.2: parameters
    // match operands in order, derived-to-base only).
    auto r = runAot(prog(
        "public static method operator -(v: V, a: ArgumentException): V => v; ",
        "var v := new V(1); var e := new Exception(\"e\"); v -= e; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "compound assignment `-=` has no valid result on record type `V`"));
}

BEZEL_TEST(sema_compound_assign_no_applicable_operator_6091,
           r4_array_operand_over_stringbuilder_param_spec_25_2) {
    auto r = runAot(prog(
        "public static method operator *(v: V, s: StringBuilder): V => v; ",
        "var v := new V(1); var a := new Array<int>(); v *= a; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "compound assignment `*=` has no valid result on record type `V`"));
}

BEZEL_TEST(sema_compound_assign_no_applicable_operator_6091,
           r5_exception_operand_over_stringbuilder_param_spec_25_2) {
    // Two distinct stdlib classes never convert to one another (§5.1).
    auto r = runAot(prog(
        "public static method operator *(v: V, s: StringBuilder): V => v; ",
        "var v := new V(1); var e := new Exception(\"e\"); v *= e; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "compound assignment `*=` has no valid result on record type `V`"));
}

// ---- accept controls (RUN, stdout asserted) -------------------------------

BEZEL_TEST(sema_compound_assign_no_applicable_operator_6091,
           a1_exception_operand_matches_exception_param_spec_25_2) {
    auto r = runAot(prog(kExcOp,
        "var v := new V(1); var e := new Exception(\"e\"); v += e; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1001\n");
}

BEZEL_TEST(sema_compound_assign_no_applicable_operator_6091,
           a2_derived_exception_operand_converts_to_base_param_spec_25_2) {
    auto r = runAot(prog(kExcOp,
        "var v := new V(1); var ae := new ArgumentException(\"a\"); v += ae; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1001\n");
}

// ---- stdlib collection operands into collection-interface parameters ------
// §5.1 / stdlib: Array<T> implements IList<T>, so it converts to IEnumerable<T>,
// ICollection<T> and IReadOnlyList<T> (same element type). The compound form is
// accepted and, since #6656, lowered through the user operator: the rows run it
// and expect 1001 (they asserted only the sema verdict while codegen failed IR
// verification).

BEZEL_TEST(sema_compound_assign_no_applicable_operator_6091,
           a3_array_operand_into_ienumerable_param_runs_spec_25_2) {
    auto r = runAot(prog(opOver("IEnumerable<int>"),
        "var v := new V(1); var s := new Array<int>(2); v += s; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1001\n");
}

BEZEL_TEST(sema_compound_assign_no_applicable_operator_6091,
           a4_array_operand_into_icollection_param_runs_spec_25_2) {
    auto r = runAot(prog(opOver("ICollection<int>"),
        "var v := new V(1); var s := new Array<int>(2); v += s; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1001\n");
}

BEZEL_TEST(sema_compound_assign_no_applicable_operator_6091,
           a5_array_operand_into_ireadonlylist_param_runs_spec_25_2) {
    auto r = runAot(prog(opOver("IReadOnlyList<int>"),
        "var v := new V(1); var s := new Array<int>(2); v += s; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1001\n");
}

BEZEL_TEST(sema_compound_assign_no_applicable_operator_6091,
           a6_array_operand_binary_form_into_ienumerable_param_prints_1001) {
    // Binary-form twin of a3: the binary operator path accepts this and the
    // program runs, so the compound rows above test the same overload.
    auto r = runAot(prog(opOver("IEnumerable<int>"),
        "var v := new V(1); var s := new Array<int>(2); v := v + s; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1001\n");
}

BEZEL_TEST(sema_compound_assign_no_applicable_operator_6091,
           r6_stringbuilder_operand_into_ienumerable_param_spec_25_2) {
    // StringBuilder is not a collection and does not implement IEnumerable<int>
    // (§5.1: no conversion between unrelated stdlib classes). The binary form is
    // accepted by declaration-pointer equality; the compound form is rejected.
    auto r = runAot(prog(opOver("IEnumerable<int>"),
        "var v := new V(1); var sb := new StringBuilder(); v += sb; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "compound assignment `+=` has no valid result on record type `V`"));
}

BEZEL_TEST(sema_compound_assign_no_applicable_operator_6091,
           r7_array_element_type_mismatch_into_ienumerable_param_spec_25_2) {
    // Array<string> implements IEnumerable<string>, not IEnumerable<int>.
    auto r = runAot(prog(opOver("IEnumerable<int>"),
        "var v := new V(1); var s := new Array<string>(2); v += s; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "compound assignment `+=` has no valid result on record type `V`"));
}
