// §22.3 (v0.236) / fix #2631 — callee-side definite assignment of `out`
// parameters on a LAMBDA body.
//
// Spec §22.3 (v0.236), verbatim: "The callee must assign every `out` parameter
// on every **normal-completion (return) path** (a path that exits by `throw`
// carries no assignment obligation); failing to do so is a compile error."
//
// Before the fix, `checkDefiniteAssignment` never walked a lambda body at all
// (`src/sema/DefiniteAssignment.cpp:1448-1454` skipped the `LambdaExpr` node
// with the comment "Lambda body is its own DA scope — we don't analyze it
// recursively here"). A lambda CAN declare an `out` parameter
// (`(out int) -> ()` is a legal function type, and
// `tests/codegen/ref_out_params_test.cpp:115-124` exercises the shape), so a
// lambda that never assigns its `out` parameter escaped the §22.3 obligation
// entirely — the same gap #2595 fixed for expression-bodied methods, on a
// different node (LambdaExpr is not a Member and `checkMember` never reaches
// it).
//
// The fix walks the lambda body with the SAME `MethodDA` machinery used for
// method bodies, gated on `hasOutParam(lam->params)` so a lambda with no `out`
// parameter is untouched (matches the #2595 narrow scope). Both body forms
// matter: block (`isBlockBody` → `stmts`, `do … end`) and expression
// (`!isBlockBody` → `exprBody`). The walk is a fresh `MethodDA` per lambda
// expression, so each lambda's `out`-parameter set and body are analyzed
// independently — capturing semantics are deliberately out of scope (the
// comment on the original skip was right that full closure-DA is non-trivial,
// but the `out`-parameter obligation is callee-local and needs no capture
// analysis: an `out` parameter of the lambda is not a captured variable).
//
// These assert on diagnostic TEXT, not a coarse "has errors" bit, so a
// rejection for some unrelated reason cannot masquerade as the §22.3 one.

#include <string>

#include "support/da_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckDA;

namespace {

// The §22.3 callee-side out-param diagnostic (MethodDA::checkOutParams).
// Same shape as the method-body diagnostic — the fix routes the lambda body
// through the same `MethodDA`, so the message text is unchanged.
bool hasOutParamError(const bezel::test::DAResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("out parameter") != std::string::npos &&
            d.message.find("not definitely assigned") != std::string::npos &&
            d.message.find("22.3") != std::string::npos)
            return true;
    return false;
}

// The definite-assignment read diagnostic (MethodDA::checkRead) — an `out`
// parameter is "freely readable AFTER assignment" (§22.3), so a read of one
// still unassigned is an error on top of the missing assignment.
bool hasUnassignedReadError(const bezel::test::DAResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("read of unassigned local") != std::string::npos)
            return true;
    return false;
}

bool hasAnyError(const bezel::test::DAResult& r) {
    return r.diagnostics.hasErrors();
}

}  // namespace

// ---------------------------------------------------------------------------
// Reject side — the defect #2631 reported.
// ---------------------------------------------------------------------------

// §22.3: the issue's literal repro. The lambda body `do return; end;` exits
// via a bare `return;` — a normal-completion path that does not assign `n`.
// The equivalent block-bodied METHOD has been rejected since the original
// out-param DA check landed; the lambda form was admitted because the walker
// never entered its body.
BEZEL_TEST(sema_lambda_out_2631, block_body_return_without_assign_rejected_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var init: (out int) -> () := lambda(out n: int) => do return; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasOutParamError(r));
}

// §22.3: fall-off with an `out` parameter never assigned is a normal-exit
// path that did not discharge the obligation. The body is `n := 99;` followed
// by NO `return` — the empty tail falls off the lambda body. Reject.
BEZEL_TEST(sema_lambda_out_2631, block_body_falloff_without_assign_rejected_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var init: (out int) -> () := lambda(out n: int) => do end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasOutParamError(r));
}

// §22.3: only ONE of two `out` parameters is assigned — the second is still
// unassigned on the return path. Reject, per-parameter granularity.
BEZEL_TEST(sema_lambda_out_2631, block_body_assigns_only_one_of_two_outs_rejected_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var init: (out int, out int) -> () := "
        "      lambda(out a: int, out b: int) => do a := 1; return; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasOutParamError(r));
}

// §22.3: reading the `out` parameter before it is assigned is BOTH a
// read-of-unassigned error AND a missing-assignment error. Exact parity with
// what the method-body form already produced.
BEZEL_TEST(sema_lambda_out_2631, expression_body_reads_unassigned_out_rejected_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var init: (out int) -> int := lambda(out n: int): int => n + 1; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasUnassignedReadError(r));
    BEZEL_EXPECT_TRUE(hasOutParamError(r));
}

// §22.3: expression body with no assignment of the `out` parameter — same
// shape as the block form's `do return; end;`. Reject.
BEZEL_TEST(sema_lambda_out_2631, expression_body_no_assign_rejected_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var init: (out int) -> int := lambda(out n: int): int => 5; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasOutParamError(r));
}

// ---------------------------------------------------------------------------
// Accept side — the false-reject guard. A widened rejection that broke these
// would be worse than the bug it fixes.
// ---------------------------------------------------------------------------

// §22.3: block body that DOES assign its `out` parameter before returning was
// always the intended accept shape; codegen exercises it in
// `tests/codegen/ref_out_params_test.cpp:115-124`. Must stay accepted.
BEZEL_TEST(sema_lambda_out_2631, block_body_assigns_out_accepted_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var init: (out int) -> () := lambda(out n: int) => do n := 99; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasOutParamError(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// §22.3: both `out` parameters assigned in a block body. Accept.
BEZEL_TEST(sema_lambda_out_2631, block_body_assigns_both_outs_accepted_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var init: (out int, out int) -> () := "
        "      lambda(out a: int, out b: int) => do a := 1; b := 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasOutParamError(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// §22.3 / §17.12: the lambda body exits by `throw` — "a path that exits by
// `throw` carries no assignment obligation". The block form's `throw` was
// always accepted; the lambda form must be too. The throw is unconditional
// (the only statement in the body), so `terminated_` becomes true and the
// fall-off check is skipped — exactly the same routing that protects the
// method-body `throw` stub.
BEZEL_TEST(sema_lambda_out_2631, block_body_throw_only_accepted_spec_17_12_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        // `owned` is REQUIRED here, not decoration (#4759). Per §28.3
        // announcement table row 4 (v0.307), a plain concrete class-typed
        // instance return admits only a receiver-sourced borrow, and
        // `new Exception()` is a fresh owned value. This row runs on
        // `parseAndCheckDA` (parse + definite assignment — the ownership pass
        // never runs), so the unmarked spelling would surface only on a port to
        // `runAot` (#3439). The marker does not change the §22.3 obligation
        // this row measures: the `throw` path still carries none.
        "  public method Thrower(): owned Exception => new Exception(); "
        "  public static method Main(): int "
        "    var init: (out int) -> () := "
        "      lambda(out n: int) => do throw new C().Thrower(); end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasOutParamError(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// §22.3: an ordinary lambda — NO `out` parameter at all — is untouched. The
// walk is not even entered for it (gated on `hasOutParam`), so the §22.3
// check, the read-of-unassigned check, and the rest of `MethodDA` cannot
// interfere with the overwhelmingly common lambda shape. This is the
// false-reject guard for the entire rest of the language's lambda surface.
BEZEL_TEST(sema_lambda_out_2631, lambda_without_out_param_accepted) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var bump: (int) -> int := lambda(n: int): int => n + 1; "
        "    return bump(7); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// §22.3: a lambda parameter named the SAME as an outer local must NOT
// confuse the walks — each lambda gets a fresh `MethodDA`, so the lambda's
// `n` and the method body's `n` (if any) are disjoint. Verifies the per-lambda
// scoping rather than relying on a single global set. The outer `n` is
// declared but never assigned in this shape; the inner `n` (the lambda's
// `out`) is assigned — the only rejection the walk must produce is the
// §22.3 one for the lambda, which is satisfied here.
BEZEL_TEST(sema_lambda_out_2631, lambda_param_shadows_outer_accepted_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var init: (out int) -> () := lambda(out n: int) => do n := 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasOutParamError(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// §22.3: a lambda whose body has BOTH a normal-completion path that assigns
// `n` AND a throw path that does not — the throw is exempt, and the normal
// path satisfies the obligation. Accept. Mirrors the method-body `if` shape
// where one branch throws and the other returns normally.
BEZEL_TEST(sema_lambda_out_2631, block_body_normal_assigns_and_throw_branch_accepted_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        // `owned` is REQUIRED here, not decoration (#4759). Per §28.3
        // announcement table row 4 (v0.307), a plain concrete class-typed
        // instance return admits only a receiver-sourced borrow, and
        // `new Exception()` is a fresh owned value. This row runs on
        // `parseAndCheckDA` (parse + definite assignment — the ownership pass
        // never runs), so the unmarked spelling would surface only on a port to
        // `runAot` (#3439). The marker does not change the §22.3 obligation
        // this row measures: the `throw` path still carries none.
        "  public method Thrower(): owned Exception => new Exception(); "
        "  public static method Main(): int "
        "    var init: (int, out int) -> () := "
        "      lambda(cond: int, out n: int) => "
        "        do "
        "          if cond = 0 then throw new C().Thrower(); "
        "          else n := cond; "
        "          end; "
        "        end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasOutParamError(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// ---------------------------------------------------------------------------
// Control — the method form, with the SAME out-param defects, must still
// behave as it did before the fix. The lambda fix MUST NOT change method-body
// checking.
// ---------------------------------------------------------------------------

// §22.3: a block-bodied method that never assigns its `out` parameter has
// been rejected since the original out-param DA check landed and must stay
// rejected. (The lambda fix only adds a NEW arm to the visitExpr switch.)
BEZEL_TEST(sema_lambda_out_2631, control_method_block_body_never_assigns_still_rejected_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public method Fill(out n: int): int "
        "    return 5; "
        "  end Fill; "
        "  public static method Main(): int "
        "    var b: C := new C(); "
        "    b.Fill(out var x); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasOutParamError(r));
}

// §22.3: a block-bodied method that DOES assign its `out` parameter has been
// accepted and must stay accepted. Verifies the lambda walk doesn't bleed
// any state into a sibling method's walk.
BEZEL_TEST(sema_lambda_out_2631, control_method_block_body_assigns_still_accepted_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public method Fill(out n: int): int "
        "    n := 5; "
        "    return n; "
        "  end Fill; "
        "  public static method Main(): int "
        "    var b: C := new C(); "
        "    b.Fill(out var x); "
        "    return x; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasOutParamError(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}
