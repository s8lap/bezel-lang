// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/lambda_param_record_destructuring_1587_test.cpp
//   (BEZEL_TEST sema_1587_lambda_destructure, owned_modifier_rejected_spec_27_4).
//
// "The destructuring parameter admits no modifiers (`ref`, `scoped`)."
// `owned` (a lambda-parameter modifier per §27.1) on a destructuring param
// must be a dedicated diagnostic, not "expected lambda parameter name".

module M; record Entry(K: int, V: int); class C public static method Main(): int var f := lambda(owned Entry(k, v)) => k + v; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a destructuring lambda parameter admits no modifier (`owned`) — the destructuring head names the exact parameter type and carries no separate annotation
// CHECK-SAME: (§27.4)
