// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/unary_minus_1588_test.cpp
//   (BEZEL_TEST sema_unary_minus_1588, unary_minus_bool_rejected_spec_3_5).

module M; class C public method M(a: bool) _ := -a; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: unary operator `-` is not valid on `bool` — unary `-` (negation) is defined only on numeric types
// CHECK-SAME: (§3.5)
