// Codegen regression tests for issue #507: immediately-invoked lambda
// expressions (IIFEs) with arguments crash the compiler (SIGILL / exit 132).
//
// Root (§27.3): `lowerCall` handles `callee is LambdaExpr` only via the
// generic `!calleeSym` branch that checks `ctx_.exprType(*call.callee)`.
// When the lambda has UNANNOTATED params the sema TypeRef's funcReturn is
// inferred from the body, but the funcParams[i].type entries are null for
// untyped params.  The `emitClosureCall` helper then builds a function type
// `i32(ptr, i32, …)` for the indirect call — but the actual LLVM function
// emitted by `lowerLambda` has params typed by the callee argument values,
// which may differ.  For annotated params the type is explicit and both sides
// agree; for unannotated params with int args both sides happen to agree on
// i32, so the simple cases pass.  The SIGILL arises when the callee is a bare
// LambdaExpr and there is a mismatch between the expected function type and
// the actual type in the IR verifier / JIT — in particular when `calleeType`
// from `ctx_.exprType` is null (no sema coverage for IIFE-callee call return
// type) and the generic `!calleeSym` branch returns null (no closure call
// issued at all), causing the fallthrough path to generate an illegal
// instruction attempting to call a non-function.
//
// Fix: in `lowerCall`, add an explicit early check for `call.callee` being a
// `LambdaExpr`; lower the lambda to a closure via `lowerLambda` and dispatch
// through the closure-call path with a function type built from the lambda's
// actual LLVM param/ret types (bypassing the TypeRef-based reconstruction that
// is unreliable for untyped params).
//
// Spec reference: §27.3 (lambda invocation), §27.5 (closure capture)
//
// Coverage:
//   - IIFE with one argument, unannotated param (the original crash case)
//   - IIFE with no arguments
//   - IIFE capturing an outer local
//   - IIFE with two parameters
//   - IIFE returning a string (tests non-int return path)
//   - Nested IIFE  `(((x)=>x)(...))`
//   - CONTROL: `var f := (x)=>x*2; f(5)` (lambda-in-local) must still work
//   - CONTROL: passing a lambda to a function as arg must still work

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ---------------------------------------------------------------------------
// §27.3 — IIFE with one unannotated argument (the original #507 crash case)
// ---------------------------------------------------------------------------

// §27.3 / #507: `((x) => x * 2)(5)` — lambda has one UNANNOTATED param,
// called immediately with integer argument 5. Result must be 10.
// If broken: SIGILL / exit 132 (compiler asserts or emits illegal instruction).
// This is the exact crash case from the bug report.
BEZEL_TEST(codegen_iife_507, iife_single_unannotated_param_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var r := (lambda(x) => x * 2)(5); "
        "    return r; "
        "  end Main; "
        "end C;"), 10);
}

// §27.3 / #507: annotated-param variant for comparison (must also pass).
BEZEL_TEST(codegen_iife_507, iife_single_annotated_param_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var r := (lambda(x: int) => x * 2)(5); "
        "    return r; "
        "  end Main; "
        "end C;"), 10);
}

// §27.3 / #507: unannotated-param IIFE, result used directly in return.
// `((x) => x + 3)(7)` == 10.
BEZEL_TEST(codegen_iife_507, iife_single_unannotated_param_return_direct_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    return (lambda(x) => x + 3)(7); "
        "  end Main; "
        "end C;"), 10);
}

// ---------------------------------------------------------------------------
// §27.3 — IIFE with no arguments
// ---------------------------------------------------------------------------

// §27.3 / #507: `(() => 42)()` — no-arg IIFE returning a constant.
// Exercises the zero-param closure path through lowerCall.
BEZEL_TEST(codegen_iife_507, iife_no_arg_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    return (lambda() => 42)(); "
        "  end Main; "
        "end C;"), 42);
}

// §27.3 / #507: no-arg IIFE stored in a var first, then called — sanity that
// the no-arg path also works via the local-variable closure route.
BEZEL_TEST(codegen_iife_507, iife_no_arg_via_local_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var f := (lambda() => 42); "
        "    return f(); "
        "  end Main; "
        "end C;"), 42);
}

// ---------------------------------------------------------------------------
// §27.3 / §27.5 — IIFE capturing an outer local
// ---------------------------------------------------------------------------

// §27.3 / §27.5 / #507: IIFE reads an outer local `n`. The closure env must
// be populated with the cell for `n`. Result: 5 * 3 = 15.
BEZEL_TEST(codegen_iife_507, iife_captures_outer_local_27_3_27_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var n := 5; "
        "    return (lambda(x: int) => x * n)(3); "
        "  end Main; "
        "end C;"), 15);
}

// §27.3 / §27.5 / #507: no-arg IIFE reads outer local. `(() => n)()` == 7.
BEZEL_TEST(codegen_iife_507, iife_no_arg_captures_outer_local_27_3_27_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var n := 7; "
        "    return (lambda() => n)(); "
        "  end Main; "
        "end C;"), 7);
}

// ---------------------------------------------------------------------------
// §27.3 — IIFE with two parameters
// ---------------------------------------------------------------------------

// §27.3 / #507: IIFE with two annotated params. `((a: int, b: int) => a + b)(3, 4)` == 7.
BEZEL_TEST(codegen_iife_507, iife_two_annotated_params_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    return (lambda(a: int, b: int) => a + b)(3, 4); "
        "  end Main; "
        "end C;"), 7);
}

// §27.3 / #507: IIFE with two UNANNOTATED params. `((a, b) => a + b)(3, 4)` == 7.
BEZEL_TEST(codegen_iife_507, iife_two_unannotated_params_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    return (lambda(a, b) => a + b)(3, 4); "
        "  end Main; "
        "end C;"), 7);
}

// narrow #3756 (§27.3 / §27.4, v0.286): two UNANNOTATED params, both
// SAME-typed (int, matching the codegen `lowerCall` bypass's verified
// case — see below), but each argument is a non-trivial EXPRESSION
// (`2 + 1`, `10 - 6`) rather than a bare literal, unlike
// `iife_two_unannotated_params_27_3`'s two plain literals.
// `((a, b) => a * b)(2 + 1, 10 - 6)` = 3 * 4 = 12.
//
// NOTE on scope: a genuinely MIXED-TYPE two-param unannotated IIFE
// (e.g. `(lambda(a, b) => a + b.Length)(3, "hello")`, `a` from an `int`
// argument and `b` from a `string` one) was measured while adding this
// row to SILENTLY compute the wrong value (`0`/no diagnostic, not even
// a clean crash) — a genuine, PRE-EXISTING codegen gap in `lowerCall`'s
// IIFE bypass (per `iife_two_annotated_params_27_3`'s neighboring
// annotated control, the annotated form is unaffected), predating #3756
// and orthogonal to this narrow fix's sema-only change (a fix would need
// to thread real per-parameter types into the IIFE callee by peeking the
// call's arguments before visiting the callee — a larger change than
// this lane's scope per CLAUDE.md's "no while-we're-at-it expansions").
// Filed as https://github.com/s8lap/bezel-lang/issues/3797 with the
// measured repro instead of shipped as an (incorrect) ACCEPT row here.
BEZEL_TEST(codegen_iife_507, iife_two_unannotated_params_expr_args_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    return (lambda(a, b) => a * b)(2 + 1, 10 - 6); "
        "  end Main; "
        "end C;"), 12);
}

// §27.3 / #507: two-param IIFE with capture. `((a: int, b: int) => a * b + n)(2, 3)` where n=1 → 7.
BEZEL_TEST(codegen_iife_507, iife_two_params_with_capture_27_3_27_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var n := 1; "
        "    return (lambda(a: int, b: int) => a * b + n)(2, 3); "
        "  end Main; "
        "end C;"), 7);
}

// ---------------------------------------------------------------------------
// §27.3 — IIFE returning a string (non-int return)
// ---------------------------------------------------------------------------

// §27.3 / #507: IIFE returns a string. Verify the result via `.Length`.
// `(() => \"hello\")().Length` == 5.
BEZEL_TEST(codegen_iife_507, iife_returns_string_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    return (lambda() => \"hello\")().Length; "
        "  end Main; "
        "end C;"), 5);
}

// ---------------------------------------------------------------------------
// §27.3 — Nested IIFE `(((x: int)=>x)(...))`
// ---------------------------------------------------------------------------

// §27.3 / #507: nested IIFE — the outer call's arg is itself an IIFE.
// `((x: int) => x * 2)(((y: int) => y + 1)(4))` = (4+1)*2 = 10.
BEZEL_TEST(codegen_iife_507, iife_nested_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    return (lambda(x: int) => x * 2)((lambda(y: int) => y + 1)(4)); "
        "  end Main; "
        "end C;"), 10);
}

// narrow #3756 (§27.3 / §27.4, v0.286): an IIFE whose OWN argument is
// itself a lambda LITERAL (a function VALUE passed positionally, not a
// call producing a scalar — the counterpart of `iife_nested_27_3` above,
// which nests a *called* IIFE). One CallExpr, callee = LambdaExpr AND
// arg[0] = LambdaExpr at once: exercises the fix's new callee-position
// `pendingLambdaArgContext_` toggle (around the callee visit) alongside
// the pre-existing #3756 argument-position toggle (around the arg visit,
// set/restored independently in the arg-visiting loop) in the SAME call,
// guarding against the two save/restore pairs leaking into each other.
// `f` and `x` are annotated (this row is about the callee/argument
// interaction, not further inference); `(lambda(f: (int) -> int) => f(5))
// (lambda(x: int) => x * 2)` = 10.
BEZEL_TEST(codegen_iife_507, iife_arg_is_itself_a_lambda_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    return (lambda(f: (int) -> int) => f(5))(lambda(x: int) => x * 2); "
        "  end Main; "
        "end C;"), 10);
}

// ---------------------------------------------------------------------------
// §27.3 — CONTROL: lambda stored in var (must still pass — regression guard)
// ---------------------------------------------------------------------------

// §27.3 / control: `var f := (x: int) => x * 2; f(5)` — lambda stored in a
// local, called through the local. Must still return 10. This path goes through
// the LocalSymbol branch of lowerCall, not the LambdaExpr branch.
BEZEL_TEST(codegen_iife_507, control_lambda_in_local_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var f := lambda(x: int) => x * 2; "
        "    return f(5); "
        "  end Main; "
        "end C;"), 10);
}

// §27.3 / control: passing a lambda as an argument to a method — the Apply
// path through ParameterSymbol in lowerCall. Must still return 10.
BEZEL_TEST(codegen_iife_507, control_lambda_passed_to_function_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Apply(f: (int) -> int, n: int): int "
        "    return f(n); "
        "  end Apply; "
        "  public static method Main(): int "
        "    return Apply(lambda(x: int) => x * 2, 5); "
        "  end Main; "
        "end C;"), 10);
}

// §27.3 / control: no-arg lambda stored in local, called — regression guard
// for the zero-param local-symbol closure path.
BEZEL_TEST(codegen_iife_507, control_no_arg_lambda_in_local_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var f := lambda() => 99; "
        "    return f(); "
        "  end Main; "
        "end C;"), 99);
}

// ---------------------------------------------------------------------------
// §27.3 / §27.4 — fix #3797: IIFE unannotated-param type inference
// ---------------------------------------------------------------------------
//
// Per §27.4: "When the lambda's target type is known from context, parameter
// type annotations may be omitted." For an immediately-invoked lambda
// `(lambda(params) => body)(args)`, the target type IS the call's argument
// types. Pre-fix #3797, sema did not thread the per-argument types into the
// IIFE callee (the argument-visiting loop runs after the callee visit), so
// each unannotated lambda param had `resolvedDeclType == null`; codegen's
// IIFE bypass built the LLVM function type from the actual argument VALUES
// (correct for the call signature) but `lowerLambda` inside the body fell
// back to `i32` for every untyped param — producing a type mismatch between
// the body's parameter slots and the call-site argument types, and a silent
// wrong value (`0` / default).
//
// Fix #3797: in `ExprTypecheck_Exprs.cpp`'s CallExpr visit, when the callee
// is a bare LambdaExpr, peek each argument's type by visiting it BEFORE the
// callee and seed `pendingLambdaParamTypes_` so the lambda visitor consumes
// the real per-parameter types (the same thread that typed-local init /
// argument-lambda paths already use, #829 / #3756).
//
// The IIFE result's own exprType is also now populated by typeCall's
// `!calleeSym` branch (`ctx_.exprType(*ce.callee)->funcReturn`), so member
// access on the IIFE result lowers correctly (the pre-#3797 `#3079` guard
// fired for `r.ToString()` when the result had no sema type).

// §27.4 / fix #3797: the issue's headline repro. An unannotated `b: string`
// typed by the arg `"hello"`; `b.Length` reads the string's length (5), NOT
// the i32-defaulted slot's bytes (0). Pre-fix: returned 0, no diagnostic.
BEZEL_TEST(codegen_iife_507, iife_mixed_unannotated_params_member_access_27_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    return (lambda(a, b) => b.Length)(3, \"hello\"); "
        "  end Main; "
        "end C;"), 5);
}

// §27.4 / fix #3797: the issue's positive case (homogeneous-typed IIFE with
// the result consumed via `.ToString()`). Pre-fix: the IIFE call result had
// no sema type, so `.ToString()` failed to lower (`internal: ... #3079`)
// and the line was silently dropped — the program exited 0 with no output.
// Post-fix: prints "10".
BEZEL_TEST(codegen_iife_507, iife_unannotated_result_member_access_27_4) {
    // runAotOut strips the trailing newline (lane-rules §15); r.stdoutText
    // captured directly would carry it.
    std::string out = bezel::test::runAotOut(
        "module M; import System; class C "
        "  public static method Main(): int "
        "    var r := (lambda(x) => x * 2)(5); "
        "    Console.WriteLine(r.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(out, std::string("10"));
}

// §27.4 / fix #3797: control — an annotated IIFE of the same shape still
// works (regression guard against the fix narrowing an already-correct path).
BEZEL_TEST(codegen_iife_507, iife_mixed_annotated_params_control_27_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    return (lambda(a: int, b: string) => b.Length)(3, \"hello\"); "
        "  end Main; "
        "end C;"), 5);
}

// §27.4 / fix #3797: a single-param IIFE whose body uses a member-access on
// the unannotated param — exercises the same fix path on the simplest shape
// (a single positional arg, no second param to confuse the index alignment).
BEZEL_TEST(codegen_iife_507, iife_single_unannotated_param_member_access_27_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    return (lambda(s) => s.Length)(\"world\"); "
        "  end Main; "
        "end C;"), 5);
}

// §27.4 / fix #3797: three-param mixed-type IIFE — first arg drives an int
// expression, second drives a string.Length, third drives another int
// expression — to confirm positional alignment survives beyond two params.
BEZEL_TEST(codegen_iife_507, iife_three_unannotated_params_mixed_27_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    return (lambda(a, b, c) => a + b.Length + c)(1, \"abc\", 100); "
        "  end Main; "
        "end C;"), 104);
}

// §27.4 / fix #3797: IIFE where the body calls a method on the unannotated
// string param (`.ToUpper()` is a stdlib §2.3 string member), not just
// reads a primitive field — confirms the seeded TypeRef is propagated
// far enough for arbitrary member-call resolution, not just field loads.
BEZEL_TEST(codegen_iife_507, iife_unannotated_param_method_call_27_4) {
    std::string out = bezel::test::runAotOut(
        "module M; import System; class C "
        "  public static method Main(): int "
        "    var s := (lambda(s) => s.ToUpper())((\"hi\")); "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(out, std::string("HI"));
}
