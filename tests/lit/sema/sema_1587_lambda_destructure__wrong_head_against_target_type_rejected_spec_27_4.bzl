// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/lambda_param_record_destructuring_1587_test.cpp
//   (BEZEL_TEST sema_1587_lambda_destructure, wrong_head_against_target_type_rejected_spec_27_4).
//
// "when a target type supplies the parameter type, a head naming any other
// type is a compile error (no subtype test, nothing to fail at runtime)."

module M; record Entry(K: int, V: int); record Other(A: int, B: int); class C public static method Main(): int var f: (Entry) -> int := lambda(Other(k, v)) => k + v; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: destructuring lambda parameter head `Other` does not match the expected parameter type (`Entry`) — the head must name exactly the parameter's type, with no subtype test
// CHECK-SAME: (§27.4)
