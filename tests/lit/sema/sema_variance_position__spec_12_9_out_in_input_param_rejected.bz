// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_position, spec_12_9_out_in_input_param_rejected).
//
// §12.9 example A1: `interface IBad<out T> { method Consume(item: T); }`
// T appears as an ordinary method parameter — input position — which violates
// the `out` (covariant) annotation.

module M; interface IBad<out T> method Consume(item: T); end IBad;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` is covariant (`out`) but appears in an input (parameter) position in parameter `item` of method `Consume`
// CHECK-SAME: (§12.9)
