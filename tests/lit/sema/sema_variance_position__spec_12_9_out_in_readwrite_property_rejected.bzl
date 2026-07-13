// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_position, spec_12_9_out_in_readwrite_property_rejected).
//
// §12.9: T in a read-write property type is an input position (has setter).
// A read-write property is NOT a pure output position.

module M; interface IBad3<out T> property Item: T; end IBad3;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` is covariant (`out`) but appears in a read-write property `Item`, which has a setter (input position)
// CHECK-SAME: (§12.9)
