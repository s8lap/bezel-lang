// Codegen regression tests for issue #3801: an unannotated function-typed IIFE
// parameter silently returns 0 instead of the correct value.
//
// Root (§27.3 / §27.4 / §27.1): `lowerLambda` honors
// `par->resolvedDeclType` for the LLVM parameter type via the IIFE-arg thread
// (ExprTypecheck_Exprs.cpp's `pendingLambdaParamTypes_` seeded from the call
// args, v0.286 #3797) — so the lambda's storage is correctly typed. But the
// `lowerCall` callee-dispatch arm for a `ParameterSymbol` callee (line ~5227
// of LlvmBackend_Calls2.cpp) reads ONLY `par->declType`, falling back to null
// for an UNANNOTATED lambda param. Without `par->resolvedDeclType` as a
// fallback, an `a(7)` call where `a: (int) -> int` is an unannotated lambda
// parameter left `rt = null`, missed the `rt->kind == Function` dispatch, and
// silently returned 0 (the #2477 zero-init fallback) instead of calling
// through the closure pointer. Verified empirically on the issue repro.
//
// Fix: also consult `par->resolvedDeclType` when `par->declType` is null,
// mirroring the FieldSymbol arm's generic-substitution fallback immediately
// below the site.
//
// Spec reference: §27.3 (lambda expressions), §27.4 (parameter type
// inference), §27.1 (function types), v0.287.
//
// Coverage:
//   - Row 1: the issue repro — unannotated function-typed IIFE param
//   - Row 2: control — annotated IIFE — must still pass
//   - Row 3: control — unannotated IIFE with int param + int arg (#3797)
//   - Row 4: control — unannotated IIFE with string param (#3797)

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
// §27.3 / §27.4 / §27.1 / #3801 — unannotated function-typed IIFE param
// ---------------------------------------------------------------------------

// §27.3 / §27.4 / #3801 (issue repro): an UNANNOTATED function-typed IIFE
// parameter — `(lambda(a, b) => a(7) + b.Length)(f, "hi")` with
// `f: (int) -> int` and `"hi": string`. Pre-fix: silently returned 0
// (a(7)'s call through the closure pointer was skipped because
// `par->declType` is null for the unannotated param `a`). Post-fix:
// returns 16 = 14 + 2.
BEZEL_TEST(codegen_iife_3801, unannotated_iife_function_typed_param_27_3_27_4_27_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System; class C "
        "  public static method Main(): int "
        "    var f := (lambda(x: int) => x * 2); "
        "    var r := (lambda(a, b) => a(7) + b.Length)(f, \"hi\"); "
        "    Console.WriteLine($\"r={r}\"); "
        "    return r; "
        "  end Main; "
        "end C;"), 16);
}

// §27.3 / §27.4 / #3801 (row 2 — control): annotated IIFE — must still pass.
// `(lambda(a: (int) -> int, b: string) => a(7) + b.Length)(f, "hi")` returns 16.
// (This worked before the fix too — its parameter has a written annotation.)
BEZEL_TEST(codegen_iife_3801, annotated_iife_function_typed_param_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System; class C "
        "  public static method Main(): int "
        "    var f := (lambda(x: int) => x * 2); "
        "    var r := (lambda(a: (int) -> int, b: string) => a(7) + b.Length)(f, \"hi\"); "
        "    Console.WriteLine($\"r={r}\"); "
        "    return r; "
        "  end Main; "
        "end C;"), 16);
}

// §27.3 / §27.4 / #3797 (row 3 — control): unannotated IIFE with int param +
// int arg — the original #3797 fix's primary coverage.
// `(lambda(x) => x + 1)(41)` returns 42.
BEZEL_TEST(codegen_iife_3801, unannotated_iife_int_param_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var r := (lambda(x) => x + 1)(41); "
        "    return r; "
        "  end Main; "
        "end C;"), 42);
}

// §27.3 / §27.4 / #3797 (row 4 — control): unannotated IIFE with string param —
// the original #3797 fix's coverage for non-int inferred types.
// `(lambda(b) => b.Length)("hi")` returns 2.
BEZEL_TEST(codegen_iife_3801, unannotated_iife_string_param_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var r := (lambda(b) => b.Length)(\"hi\"); "
        "    return r; "
        "  end Main; "
        "end C;"), 2);
}