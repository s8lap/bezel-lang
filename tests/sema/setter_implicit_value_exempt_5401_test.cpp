// fix-forward #5401 — §21.1 / §23.3 / §23.4, v0.314
//
// RULING SUMMARY (ruling commit b1f76a70, v0.314): a setter's implicit `value`
// is EXCLUDED from the parameter clause (Appendix B / §23.3 lets the setter body
// redeclare it). The prior fix (671d0018) over-rejected by not carving out this
// exemption at all — its §21.1 duplicate-local check fired for `var value` even
// though the implicit `value` param is not in the parameter clause for a regular
// (non-indexer) property setter.
//
// BEHAVIOR MATRIX:
//   T1 ACCEPT: `var value` in a regular property setter body — the implicit
//              `value` param is NOT in the parameter clause (Appendix B / §23.3).
//   T2 REJECT: `var x` in a setter with an explicit `set(x)` param — that param
//              IS in the parameter clause, so shadowing it is §21.1 violation.
//   T3 ACCEPT: `var value` in a NON-setter method body — no implicit `value`
//              param at all, so `var value` is a plain local (should compile).
//   T4 REJECT: indexer setter with `var value` — indexer params (including the
//              indexer setter's implicit `value`) are ALL in the parameter clause
//              per §23.4, so `var value` shadows it and must be rejected.

#include <string>
#include <string_view>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

// Builds a program from the given class+method body.
std::string prog(std::string_view body) {
    return std::string("module M; ") + std::string(body) + " end C;";
}

}  // namespace

// T1 ACCEPT — regular property setter body may redeclare `value`.
// Pre-fix (base a0257e36): REJECTED — "a local named `value` is already
// declared in this scope". This row is the primary accept-row that confirms
// the fix-forward works. The program compiles and runs; the exit code reflects
// that `value` in the setter body refers to the local (42), not the param (99).
// §21.1 / §23.3 (#5401, v0.314)
BEZEL_TEST(sema_setter_implicit_value_exempt_5401,
           t1_setter_body_var_value_accepted_spec_21_1_23_3) {
    // A property setter with `var value` declared inside — must compile clean.
    // The local `value` shadows the implicit setter param for uses within the
    // setter body, so _x := value stores 42 (the local), not 99 (the param).
    auto r = runAot(prog(
        "class C public _x: int := 0; "
        "public property Val: int get => _x; "
        "set => do var value: int := 42; _x := value; end; end; "
        "public static method Main(): int "
        "  using var c := new C(); "
        "  c.Val := 99; "
        "  return c.Val; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// T2 REJECT — explicit setter param `set(x)` means `x` IS in the parameter
// clause; declaring `var x` inside is a §21.1 duplicate-local violation.
// §21.1 (#5401, v0.314)
BEZEL_TEST(sema_setter_implicit_value_exempt_5401,
           t2_explicit_setter_param_var_x_rejected_spec_21_1) {
    auto r = runAot(prog(
        "class C public _x: int := 0; "
        "public property Val: int get => _x; "
        "set(x) => do var x: int := 99; end; end; "
        "public static method Main(): int return 0; end Main;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "a local named `x` is already declared"));
}

// T3 ACCEPT — non-setter method with `var value` in body is a plain local
// declaration; no implicit `value` param exists in a non-setter method.
// §21.1 (#5401, v0.314)
BEZEL_TEST(sema_setter_implicit_value_exempt_5401,
           t3_non_setter_method_var_value_accepted_spec_21_1) {
    auto r = runAot(prog(
        "class C "
        "public method DoThing(): int "
        "  var value: int := 42; "
        "  return value; "
        "end DoThing; "
        "public static method Main(): int "
        "  using var c := new C(); "
        "  return c.DoThing(); "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// T4 REJECT — indexer setter's implicit `value` IS in the parameter clause
// per §23.4; `var value` inside shadows it and must be rejected.
// §21.1 / §23.4 (#5401, v0.314)
BEZEL_TEST(sema_setter_implicit_value_exempt_5401,
           t4_indexer_setter_var_value_rejected_spec_21_1_23_4) {
    auto r = runAot(prog(
        "class C "
        "private owned _arr: Array<int> := new Array<int>(1); "
        "public property [i: int]: int "
        "  get => _arr[i]; "
        "  set => do var value: int := 42; end; "
        "end; "
        "public static method Main(): int "
        "  using var c := new C(); "
        "  c[0] := 1; "
        "  return c[0]; "
        "end Main;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "a local named `value` is already declared"));
}
