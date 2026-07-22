// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/lambda_param_record_destructuring_1587_test.cpp
//   (BEZEL_TEST sema_1587_lambda_destructure, arity_mismatch_over_binding_rejected_spec_27_4).

module M; record Entry(K: int, V: int); class C public static method Main(): int var f := lambda(Entry(k, v, w)) => k + v + w; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: destructuring lambda parameter `Entry(...)` binds 3 value(s) but record `Entry` has 2 positional field(s); the binding count must match
// CHECK-SAME: (§27.4)
