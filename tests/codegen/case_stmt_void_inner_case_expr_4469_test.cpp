// fix #4469 / spec §19.7 (v0.306): a `case` STATEMENT whose arm body is a
// parenthesized case EXPRESSION — `case o of A => (case inner of …), B => …`
// — emits malformed IR ("Basic Block … does not have terminator! label
// %case.end") at the inner `case.end` block.
//
// ROOT CAUSE (traced, LlvmBackend_Exprs2.cpp). `lowerCaseExpr` derives
// `resultTy` from the case expression's sema exprType (defaulting to i32
// when no exprType was recorded). A statement-position case expression
// — one used as the body of a CaseStmt arm — has every arm body of type
// `Unit`/void (the arm bodies discard their results via the surrounding
// ExpressionStmt). §19.2's "no common type, no recording" rule
// (ExprTypecheck.cpp:5167) does NOT annotate the case expression's
// exprType when every arm body is itself void, so `resultSemaTy` is
// null and `resultTy` defaults to `i32`.
//
// Then the per-arm loop sees the arm body `lowerExpr` return the void
// sentinel (nullptr, LlvmBackend_Exprs.cpp:90) and hits the legacy
// `if (!v) return nullptr` bail-out BEFORE the deferred `CreateBr(endBB)`
// loop fires. The inner `case.end` is left with no predecessors, the
// outer `lowerCaseStmt`'s `if (!armTerm) builder_.CreateBr(endBB)` branches
// from the void call's last block (e.g. a `cat.done*` block) to the
// outer `casestmt.end`, and the IR verifier rejects the dangling inner
// `case.end`.
//
// THE FIX is one new arm-level branch in `lowerCaseExpr`'s arm loop:
// when an arm body lowers to nullptr AND `resultSemaTy` is null (sema
// recorded no type on the case expression) AND `resultTy` is not already
// void, promote `resultTy` to void for THIS case expression, add the
// arm's convergence block to `incomings` with a null value, advance to
// the next arm — and let the existing post-loop `resultTy->isVoidTy()`
// guard skip PHI construction. The widening path's `isNullPtr`/`widenOne`
// helpers were also null-guarded so a void sentinel in `incomings` does
// not deref null.
//
// SCOPE (intentionally narrow — this lane fixes the parenthesized case-
// expression-as-statement-arm-body shape):
//   - The plain-expression case STATEMENT (`case x of 1 => F(), _ => F()`,
//     no parenthesization on the body) is unaffected — that path lowers
//     arm bodies via the FFI/owned-producer arm at LlvmBackend_Stmts.cpp
//     ~3165, never through `lowerCaseExpr`'s expression arm.
//   - The expression-POSITION case expression (`var r := case … of …`)
//     is unaffected — sema records the common arm type and `resultTy` is
//     never left at the i32 default.
//   - The scrutinee-less `case cond of … end` form (#727) is unaffected —
//     its arm patterns are bool conditions, not expression bodies, so
//     the arm-loop's `if (!v)` branch is never reached.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

const std::string kPrelude =
    "module M; "
    "public choice Inner "
    "  IOk(c: int), "
    "  IErr(s: string); "
    "end Inner; "
    "public choice Outer "
    "  OOk(owned inner: Inner), "
    "  OFail(message: string); "
    "end Outer; "
    "public class C ";

JitResult run(const std::string& mainBody) {
    return runAot(kPrelude +
                  "public static method Main(): int " + mainBody +
                  " end Main; end C;");
}

}  // namespace

// ============================================================
// FIXED ROW — the issue's exact shape. A nested case STATEMENT
// containing a parenthesized case EXPRESSION (the `(case move
// inner of …)` form, with each arm body a void-returning call)
// as the body of the outer arm. Without the fix this SIGABRTs
// at codegen time ("Basic Block … does not have terminator!
// label %case.end"); with the fix it returns the expected value.
// ============================================================

BEZEL_TEST(case_stmt_void_inner_case_expr_4469,
           nested_case_stmt_void_inner_case_expr) {
    auto r = run(
        "var o: Outer := Outer.OOk(Inner.IOk(42)); "
        "case move o of "
        "  Outer.OOk(var inner) => (case move inner of "
        "    Inner.IOk(c) => Console.WriteLine($\"v={c}\"), "
        "    Inner.IErr(s) => Console.WriteLine($\"err={s}\")), "
        "  Outer.OFail(m) => Console.WriteLine($\"fail={m}\") "
        "end; "
        "return 42;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ============================================================
// FIXED ROWS — variations of the same shape. Each row exercises
// a distinct AST position the fix has to handle.
// ============================================================

// The non-matching path is taken (the OOk arm's inner case is not
// evaluated at runtime), but the buggy code path still triggers
// because lowering visits both arms regardless of which is taken.
BEZEL_TEST(case_stmt_void_inner_case_expr_4469,
           nested_case_stmt_void_inner_case_expr_unselected) {
    auto r = run(
        "var o: Outer := Outer.OFail(\"skip-inner\"); "
        "case move o of "
        "  Outer.OOk(var inner) => (case move inner of "
        "    Inner.IOk(c) => Console.WriteLine($\"v={c}\"), "
        "    Inner.IErr(s) => Console.WriteLine($\"err={s}\")), "
        "  Outer.OFail(m) => Console.WriteLine($\"fail={m}\") "
        "end; "
        "return 7;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// A three-arm outer case, with TWO arms carrying parenthesized
// inner case expressions. Proves the fix is per-inner-case, not
// globally once-only.
BEZEL_TEST(case_stmt_void_inner_case_expr_4469,
           nested_case_stmt_two_inner_void_case_exprs) {
    auto r = run(
        "var o: Outer := Outer.OOk(Inner.IErr(\"e\")); "
        "case move o of "
        "  Outer.OOk(var inner) => (case move inner of "
        "    Inner.IOk(c) => Console.WriteLine($\"v={c}\"), "
        "    Inner.IErr(s) => Console.WriteLine($\"err={s}\")), "
        "  Outer.OFail(m) => (case m.Length of "
        "    0 => Console.WriteLine(\"zero\"), "
        "    _ => Console.WriteLine(\"nonzero\")) "
        "end; "
        "return 5;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ============================================================
// ACCEPT-SIDE CONTROL — a plain case STATEMENT (no parenthesized
// expression body) keeps working. A false change in the fix would
// widen the void-promotion to this shape and break it.
// ============================================================

BEZEL_TEST(case_stmt_void_inner_case_expr_4469,
           stmt_form_plain_unaffected_control) {
    auto r = run(
        "var o: Outer := Outer.OOk(Inner.IOk(7)); "
        "var s := \"\"; "
        "case move o of "
        "  Outer.OOk(var inner) => do "
        "    s := \"ok\"; "
        "  end, "
        "  Outer.OFail(m) => do "
        "    s := \"fail\"; "
        "  end "
        "end; "
        "if s = \"ok\" then return 1; else return 0; end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}
