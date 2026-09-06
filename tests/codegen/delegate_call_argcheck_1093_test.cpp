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

#include "support/aot_result_asserts.hpp"   // fix #3895: failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #1093: a wrong-TYPED argument to a delegate call is rejected (was: accepted,
// miscompiled).
BEZEL_TEST(delegate_call_argcheck_1093, wrong_typed_arg_rejected) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var f: (int)->int := lambda(a: int) => a + 100; "
        "  var g: (int)->int := lambda(b: int) => b * 7; "
        "  return f(g); "                                  // g is (int)->int, not int
        "end Main; end C;");
    // fix #3895 (issue #3892): distinguishes a genuine §5.1 rejection from a
    // runtime crash/signal, which a bare `!ran`/`hasErrors()` pair cannot.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// #1093: a wrong-ARITY delegate call is rejected.
BEZEL_TEST(delegate_call_argcheck_1093, wrong_arity_rejected) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var f: (int)->int := lambda(a: int) => a + 100; "
        "  return f(); "                                   // 0 args, expects 1
        "end Main; end C;");
    // fix #3895 (issue #3892): distinguishes a genuine §5.1 rejection from a
    // runtime crash/signal, which a bare `!ran`/`hasErrors()` pair cannot.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// #1093: a wrong scalar type (string where int expected) is rejected.
BEZEL_TEST(delegate_call_argcheck_1093, wrong_scalar_type_rejected) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var f: (int)->int := lambda(a: int) => a + 100; "
        "  return f(\"hi\"); "
        "end Main; end C;");
    // fix #3895 (issue #3892): distinguishes a genuine §5.1 rejection from a
    // runtime crash/signal, which a bare `!ran`/`hasErrors()` pair cannot.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// A correctly-typed delegate call is unchanged.
BEZEL_TEST(delegate_call_argcheck_1093, correct_call_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var f: (int)->int := lambda(a: int) => a + 100; "
        "  return f(5); "                                  // 105
        "end Main; end C;"), 105);
}

// A function-typed PARAMETER (higher-order call) with a correct argument works.
BEZEL_TEST(delegate_call_argcheck_1093, higher_order_param_ok) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Apply(fn: (int)->int, x: int): int => fn(x); "
        "  public static method Main(): int "
        "    return Apply(lambda(a: int) => a + 1, 41); "        // 42
        "  end Main; end C;"), 42);
}

// A WIDENING argument (int → long) is assignable and accepted (§5.1).
BEZEL_TEST(delegate_call_argcheck_1093, widening_arg_accepted) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var f: (long)->long := lambda(a: long) => a + 1; "
        "  return int(f(41)); "                            // 42
        "end Main; end C;"), 42);
}

// A nullary function value is unchanged.
BEZEL_TEST(delegate_call_argcheck_1093, nullary_func_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var f: ()->int := lambda() => 42; "
        "  return f(); "                                   // 42
        "end Main; end C;"), 42);
}

// ── #1138 / §27.7: calling a function-typed FIELD types the call result, so ───
// a by-value (record) result materializes (was a zeroed record because the
// untyped call result left `var p := h._fn()` untyped → field reads folded to 0).

// A function-typed field returning a record.
BEZEL_TEST(delegate_call_argcheck_1093, field_func_record_result_1138) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pt(x: int, y: int); "
        "class Holder "
        "  public _fn: () -> Pt; "
        "  public constructor() _fn := lambda() => new Pt(3, 4); end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  var p := h._fn(); "
        "  return p.x + p.y; "                             // 7
        "end Main; end C;"), 7);
}

// A function-typed field with parameters returning a record.
BEZEL_TEST(delegate_call_argcheck_1093, field_func_args_record_result_1138) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pt(x: int, y: int); "
        "class Holder "
        "  public _mk: (int, int) -> Pt; "
        "  public constructor() _mk := lambda(a, b) => new Pt(a, b); end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  var p := h._mk(5, 6); "
        "  return p.x + p.y; "                             // 11
        "end Main; end C;"), 11);
}

// Regression: a function-typed field returning an int still works.
BEZEL_TEST(delegate_call_argcheck_1093, field_func_int_result_1138) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Holder "
        "  public _fn: () -> int; "
        "  public constructor() _fn := lambda() => 42; end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  return h._fn(); "                               // 42
        "end Main; end C;"), 42);
}
