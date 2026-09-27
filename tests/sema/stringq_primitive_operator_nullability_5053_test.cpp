// fix #5053 (§7.11, v0.303): `string?` argument binding to a primitive `string`
// operator parameter — the nullability gate in `findBinaryOperator`'s
// `operandMatchesParam` only checked Named types, not the Primitive case.
//
// Before the fix, `v * sn` compiled when `sn: string?` — the Primitive branch
// at ExprTypecheck_Exprs.cpp:5551 accepted `string?` against a `string` parameter
// because it checked only `at->kind == TypeRef::Kind::Primitive` and not
// nullability. The operator resolved, codegen passed the `{i8, string}` wrapper
// struct to the function expecting bare `string`, and the runtime got a malformed
// value. This test pins the rejection.
//
// The fix makes `operandMatchesParam` return false for the nullable-primitive
// / non-nullable-primitive pair, so the operator does not resolve and the
// generic "no matching operator" diagnostic fires — the §7.11 gate is the
// *reason* the match fails, the generic diagnostic is the *result*.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

// The issue's verbatim repro: should be a compile error.
BEZEL_TEST(lifted_operators_rejected_5053, stringq_nullarg_to_primitive_string_param_rejected) {
    auto r = bezel::test::parseAndTypecheckGated(
        "module M; "
        "record V(X: int) "
        "  public static method operator *(v: V, s: string): V "
        "    => new V(v.X + s.Length); "
        "end V; "
        "public class Program "
        "  public static method Main(): int "
        "    var v := new V(1); "
        "    var sn: string? := \"ab\"; "
        "    var w := v * sn; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The specific error: operator not defined because the nullable-primitive
    // argument (string?) did not match the non-nullable-primitive parameter (string).
    bool foundOpNotDefined = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("not defined for type `V` with these operands") != std::string::npos)
            foundOpNotDefined = true;
    }
    BEZEL_EXPECT_TRUE(foundOpNotDefined);
}

// Positive control: non-nullable `string` argument binds correctly.
BEZEL_TEST(lifted_operators_rejected_5053, string_non_nullarg_binds_correctly) {
    auto r = bezel::test::parseAndTypecheckGated(
        "module M; "
        "record V(X: int) "
        "  public static method operator *(v: V, s: string): V "
        "    => new V(v.X + s.Length); "
        "end V; "
        "public class Program "
        "  public static method Main(): int "
        "    var v := new V(1); "
        "    var sn: string := \"ab\"; "
        "    var w := v * sn; "
        "    return w.X; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Control: non-nullable string + non-nullable string is fine.
BEZEL_TEST(lifted_operators_rejected_5053, string_plus_string_accepted) {
    auto r = bezel::test::parseAndTypecheckGated(
        "module M; "
        "class C public static method Main(): int "
        "  var t := \"a\" + \"b\"; "
        "  return t.Length; "
        "end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Equality = and <> on string? are exempt per §8.4.
BEZEL_TEST(lifted_operators_rejected_5053, stringq_equality_admitted) {
    auto r = bezel::test::parseAndTypecheckGated(
        "module M; "
        "class C public static method Main(): int "
        "  var a: string? := null; "
        "  var b: string? := null; "
        "  if a = b then return 1; else return 0; end; "
        "end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(lifted_operators_rejected_5053, stringq_inequality_admitted) {
    auto r = bezel::test::parseAndTypecheckGated(
        "module M; "
        "class C public static method Main(): int "
        "  var a: string? := null; "
        "  var b: string? := null; "
        "  if a <> b then return 1; else return 0; end; "
        "end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Mixed: non-nullable V with nullable string? operand at + on string (built-in concat).
BEZEL_TEST(lifted_operators_rejected_5053, stringq_at_concatenation_rejected) {
    auto r = bezel::test::parseAndTypecheckGated(
        "module M; "
        "class C public static method Main(): int "
        "  var sn: string? := null; "
        "  var t := sn + \"x\"; "
        "  return 0; "
        "end Main; "
        "end C;");
    // string + string? — the concat operator takes (string, string); the second
    // operand is string? which does not match the non-nullable string parameter.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
