// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_position, spec_12_9_out_in_multi_param_rejected).
//
// §12.9: a second method parameter is also an input position.

module M; interface IBad2<out T> method Compare(a: T, b: T): int; end IBad2;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` is covariant (`out`) but appears in an input (parameter) position in parameter `a` of method `Compare`
// CHECK-SAME: (§12.9)
