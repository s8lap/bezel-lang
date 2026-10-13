// Regression tests for #6004 — a compound assignment `v += e` whose user
// `operator +` takes a BUILT-IN exception type as its right parameter
// (`operator +(V, Exception)`) failed LLVM verification ("Call parameter type
// does not match function signature!"). The compound lowering's overload
// search (findCompoundOperatorOverload, LlvmBackend_Layout.cpp) matched the
// operand's type by TypeSymbol declaration; a built-in exception has none, so
// no overload was found and the built-in integer tail ran on the record.
//
// Spec: §25.1 / §25.2 (v0.338): `a op= b` is `a := a op b`, and parameters
// match operands in order, a derived-typed operand satisfying a base-typed
// parameter by the ordinary argument rules. §31.4: the built-in exception
// hierarchy (ArgumentException derives from Exception).
//
// Rows assert the program's stdout (the observable), not the exit code.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Each operator adds 1 (for an exception operand) or the int operand (for an
// int operand), so the printed N shows which overload the compound selected.
std::string prog(const char* opDecls, const char* body) {
    return std::string("module M; record V(N: int) ") + opDecls +
           " end V; class P public static method Main(): int " + body +
           " return 0; end Main; end P;";
}

}  // namespace

// #6004 / §25.1: the issue's repro verbatim — one operator over Exception.
BEZEL_TEST(compound_builtin_exception_operator_6004, exception_operand_compound_runs) {
    auto r = runAot(prog(
        "public static pure method operator +(a: V, e: Exception): V => new V(a.N + 1);",
        "  using var d := new Exception(\"x\"); "
        "  var v := new V(0); "
        "  v += d; v += d; "
        "  Console.WriteLine(v.N.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// #6004 / §25.2: the same over ArgumentException (the built-in with a
// declared member, named by its own type rather than the base).
BEZEL_TEST(compound_builtin_exception_operator_6004, argument_exception_operand_compound_runs) {
    auto r = runAot(prog(
        "public static pure method operator +(a: V, e: ArgumentException): V => new V(a.N + 10);",
        "  using var e := new ArgumentException(\"x\"); "
        "  var v := new V(1); "
        "  v += e; "
        "  Console.WriteLine(v.N.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("11\n"));
}

// #6004 / §25.2 + §31.4: a DERIVED built-in operand (ArgumentException) meets
// a BASE-typed parameter (Exception) — the derived-satisfies-base rule.
BEZEL_TEST(compound_builtin_exception_operator_6004, derived_exception_operand_matches_base_parameter) {
    auto r = runAot(prog(
        "public static pure method operator +(a: V, e: Exception): V => new V(a.N + 1);",
        "  using var e := new ArgumentException(\"x\"); "
        "  var v := new V(0); "
        "  v += e; "
        "  Console.WriteLine(v.N.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// #6004 / §25.2: the Exception overload beside an int overload (the issue's
// second shape): each compound selects its own parameter's overload.
BEZEL_TEST(compound_builtin_exception_operator_6004, exception_overload_beside_int_overload) {
    auto r = runAot(prog(
        "public static pure method operator +(a: V, e: Exception): V => new V(a.N + 1); "
        "public static pure method operator +(a: V, k: int): V => new V(a.N + k);",
        "  using var d := new Exception(\"x\"); "
        "  var v := new V(0); "
        "  v += d; v += 5; "
        "  Console.WriteLine(v.N.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6\n"));
}

// Control: the explicit `v := v + d` form already worked (lowerBinary reads
// sema's recorded operator); it must keep giving the same answer.
BEZEL_TEST(compound_builtin_exception_operator_6004, explicit_binary_form_control) {
    auto r = runAot(prog(
        "public static pure method operator +(a: V, e: Exception): V => new V(a.N + 1);",
        "  using var d := new Exception(\"x\"); "
        "  var v := new V(0); "
        "  v := v + d; v := v + d; "
        "  Console.WriteLine(v.N.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// #6004 / §22.6 exact-beats-convert (v0.338): `v += e` with an ArgumentException
// operand over BOTH overloads must select the exact ArgumentException overload
// (+100), whichever is declared first. Before the exact-first pass the first
// declared base-typed overload won (+1 when Exception came first).
BEZEL_TEST(compound_builtin_exception_operator_6004, exact_overload_wins_exception_declared_first) {
    auto r = runAot(prog(
        "public static pure method operator +(a: V, e: Exception): V => new V(a.N + 1); "
        "public static pure method operator +(a: V, e: ArgumentException): V => new V(a.N + 100);",
        "  using var e := new ArgumentException(\"x\"); "
        "  var v := new V(0); "
        "  v += e; "
        "  Console.WriteLine(v.N.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// #6004 / §22.6: the same two overloads declared in the other order must give
// the same exact-overload answer (+100).
BEZEL_TEST(compound_builtin_exception_operator_6004, exact_overload_wins_exception_declared_second) {
    auto r = runAot(prog(
        "public static pure method operator +(a: V, e: ArgumentException): V => new V(a.N + 100); "
        "public static pure method operator +(a: V, e: Exception): V => new V(a.N + 1);",
        "  using var e := new ArgumentException(\"x\"); "
        "  var v := new V(0); "
        "  v += e; "
        "  Console.WriteLine(v.N.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// #6004 / §22.6: the explicit `v := v + e` form on the same two overloads
// (sema's exact match) gives +100; the compound form must agree with it.
BEZEL_TEST(compound_builtin_exception_operator_6004, explicit_form_agrees_exact_overload) {
    auto r = runAot(prog(
        "public static pure method operator +(a: V, e: Exception): V => new V(a.N + 1); "
        "public static pure method operator +(a: V, e: ArgumentException): V => new V(a.N + 100);",
        "  using var e := new ArgumentException(\"x\"); "
        "  var v := new V(0); "
        "  v := v + e; "
        "  Console.WriteLine(v.N.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// #6004 / §25.2 + §31.4: a derived-of-derived operand (ArgumentNullException)
// with only the Exception overload takes the nearest base parameter (+1).
BEZEL_TEST(compound_builtin_exception_operator_6004, derived_of_derived_operand_matches_exception) {
    auto r = runAot(prog(
        "public static pure method operator +(a: V, e: Exception): V => new V(a.N + 1);",
        "  using var e := new ArgumentNullException(\"x\"); "
        "  var v := new V(0); "
        "  v += e; "
        "  Console.WriteLine(v.N.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// #6004 / §25.2 + §31.4: a derived-of-derived operand with only the nearer
// base overload (ArgumentException) takes it (+100), not the further base.
BEZEL_TEST(compound_builtin_exception_operator_6004, derived_of_derived_operand_takes_nearest_base) {
    auto r = runAot(prog(
        "public static pure method operator +(a: V, e: ArgumentException): V => new V(a.N + 100);",
        "  using var e := new ArgumentNullException(\"x\"); "
        "  var v := new V(0); "
        "  v += e; "
        "  Console.WriteLine(v.N.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// #6004 / §22.6 exact-beats-convert (v0.338): overloads that differ ONLY by
// nullability (`Exception?` vs `Exception`). A non-null Exception operand is
// an exact match for the non-nullable parameter (+1), so `v += e` must select
// it in both declaration orders, as the explicit `v := v + e` does. Before the
// nullability check the first declared overload won (+3 when `Exception?` came
// first).
BEZEL_TEST(compound_builtin_exception_operator_6004, nullable_overload_declared_first_nonnull_operand) {
    auto r = runAot(prog(
        "public static pure method operator +(a: V, e: Exception?): V => new V(a.N + 3); "
        "public static pure method operator +(a: V, e: Exception): V => new V(a.N + 1);",
        "  using var e := new Exception(\"x\"); "
        "  var v := new V(0); "
        "  v += e; "
        "  Console.WriteLine(v.N.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// #6004 / §22.6: the same overloads declared in the other order give the same
// non-nullable exact answer (+1).
BEZEL_TEST(compound_builtin_exception_operator_6004, nullable_overload_declared_second_nonnull_operand) {
    auto r = runAot(prog(
        "public static pure method operator +(a: V, e: Exception): V => new V(a.N + 1); "
        "public static pure method operator +(a: V, e: Exception?): V => new V(a.N + 3);",
        "  using var e := new Exception(\"x\"); "
        "  var v := new V(0); "
        "  v += e; "
        "  Console.WriteLine(v.N.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// #6004 / §22.6: a NULLABLE operand is an exact match for the `Exception?`
// parameter (+3), not the non-nullable one; the compound must give the explicit
// form's answer.
BEZEL_TEST(compound_builtin_exception_operator_6004, nullable_operand_selects_nullable_overload) {
    auto r = runAot(prog(
        "public static pure method operator +(a: V, e: Exception): V => new V(a.N + 1); "
        "public static pure method operator +(a: V, e: Exception?): V => new V(a.N + 3);",
        "  var e: Exception? := new Exception(\"x\"); "
        "  var v := new V(0); "
        "  v += e; "
        "  Console.WriteLine(v.N.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// #6004 / §22.6 + §25.2: a nullable DERIVED operand (ArgumentException?) over a
// nullable base parameter (Exception?) and a non-null derived one; the exact
// non-null overload does not take a nullable operand, so the base-chain
// nullable overload (+1) is the answer, as the explicit form gives.
BEZEL_TEST(compound_builtin_exception_operator_6004, nullable_derived_operand_takes_nullable_base) {
    auto r = runAot(prog(
        "public static pure method operator +(a: V, e: Exception?): V => new V(a.N + 1); "
        "public static pure method operator +(a: V, e: ArgumentException): V => new V(a.N + 100);",
        "  var e: ArgumentException? := new ArgumentException(\"x\"); "
        "  var v := new V(0); "
        "  v += e; "
        "  Console.WriteLine(v.N.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}
