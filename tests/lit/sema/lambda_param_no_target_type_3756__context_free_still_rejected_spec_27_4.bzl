// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/lambda_param_no_target_type_reject_3756_test.cpp
//   (BEZEL_TEST lambda_param_no_target_type_reject_3756,
//    arithmetic_body_no_context_still_rejected_spec_27_4).
//
// narrow #3756 (§27.3 / §27.4, v0.286): a genuinely CONTEXT-FREE lambda
// (`var h := lambda(c) => c + 1;` — not a call- or constructor-argument, not
// an IIFE callee, no typed local/field/return target) must stay rejected
// after the IIFE-callee fix below. This is the negative twin of
// iife_507_test.cpp's `iife_single_unannotated_param_27_3`: the ONLY
// difference between the two programs is whether the lambda is IMMEDIATELY
// INVOKED (`(lambda(x) => x * 2)(5)`, accepted — the call's own arguments
// are the target-type context per §27.4) or merely bound to a `var` with no
// target and no call at all (rejected here) — proving the fix's new
// callee-position `pendingLambdaArgContext_` thread is keyed on the lambda
// actually being a CallExpr callee, not on any looser signal (e.g. "the
// body does arithmetic on the parameter", which this row's body also does,
// to rule out a fix that only rejects when the body itself fails to
// resolve).

module M; import System; class C public static method Main(): int var h := lambda(c) => c + 1; Console.WriteLine(h(5).ToString()); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: parameter `c` needs a type annotation: no target type to infer from
// CHECK-SAME: (§27.4)
