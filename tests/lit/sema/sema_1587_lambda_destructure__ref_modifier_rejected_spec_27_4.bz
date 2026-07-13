// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/lambda_param_record_destructuring_1587_test.cpp
//   (BEZEL_TEST sema_1587_lambda_destructure, ref_modifier_rejected_spec_27_4).
//
// `ref` on a destructuring param is likewise rejected.

module M; record Entry(K: int, V: int); class C public static method Main(): int var f := (ref Entry(k, v)) => k + v; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a destructuring lambda parameter admits no modifier (`ref`) — the destructuring head names the exact parameter type and carries no separate annotation
// CHECK-SAME: (§27.4)
