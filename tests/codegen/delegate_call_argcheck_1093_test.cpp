// Regression tests for #1093 (§5.1): a call THROUGH a function-typed value (a
// delegate local/parameter) skipped argument type-checking, so a wrong-typed or
// wrong-arity argument was silently accepted and miscompiled — while the same
// invalid call through a named method is correctly rejected. Sema now checks a
// function-typed call's arguments against the function type's parameter list
// (arity + per-argument §5.1 assignability), excluding method/constructor
// callees (which keep their own checkCallArity). Found by adversarial discovery
// sweep #6.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #1093: a wrong-TYPED argument to a delegate call is rejected (was: accepted,
// miscompiled).
BEZEL_TEST(delegate_call_argcheck_1093, wrong_typed_arg_rejected) {
    auto r = runJit(
        "module M; class C public static method Main(): int "
        "  var f: (int)->int := (a: int) => a + 100; "
        "  var g: (int)->int := (b: int) => b * 7; "
        "  return f(g); "                                  // g is (int)->int, not int
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// #1093: a wrong-ARITY delegate call is rejected.
BEZEL_TEST(delegate_call_argcheck_1093, wrong_arity_rejected) {
    auto r = runJit(
        "module M; class C public static method Main(): int "
        "  var f: (int)->int := (a: int) => a + 100; "
        "  return f(); "                                   // 0 args, expects 1
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// #1093: a wrong scalar type (string where int expected) is rejected.
BEZEL_TEST(delegate_call_argcheck_1093, wrong_scalar_type_rejected) {
    auto r = runJit(
        "module M; class C public static method Main(): int "
        "  var f: (int)->int := (a: int) => a + 100; "
        "  return f(\"hi\"); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// A correctly-typed delegate call is unchanged.
BEZEL_TEST(delegate_call_argcheck_1093, correct_call_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var f: (int)->int := (a: int) => a + 100; "
        "  return f(5); "                                  // 105
        "end Main; end C;"), 105);
}

// A function-typed PARAMETER (higher-order call) with a correct argument works.
BEZEL_TEST(delegate_call_argcheck_1093, higher_order_param_ok) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Apply(fn: (int)->int, x: int): int => fn(x); "
        "  public static method Main(): int "
        "    return Apply((a: int) => a + 1, 41); "        // 42
        "  end Main; end C;"), 42);
}

// A WIDENING argument (int → long) is assignable and accepted (§5.1).
BEZEL_TEST(delegate_call_argcheck_1093, widening_arg_accepted) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var f: (long)->long := (a: long) => a + 1; "
        "  return int(f(41)); "                            // 42
        "end Main; end C;"), 42);
}

// A nullary function value is unchanged.
BEZEL_TEST(delegate_call_argcheck_1093, nullary_func_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var f: ()->int := () => 42; "
        "  return f(); "                                   // 42
        "end Main; end C;"), 42);
}
