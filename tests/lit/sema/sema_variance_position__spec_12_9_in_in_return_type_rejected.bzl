// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_position, spec_12_9_in_in_return_type_rejected).
//
// §12.9 example B1: `interface IBadIn<in T> { method Produce(): T; }`
// T appears as method return type — output position — violating `in`.

module M; interface IBadIn<in T> method Produce(): T; end IBadIn;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` is contravariant (`in`) but appears in an output (return) position in method `Produce`
// CHECK-SAME: (§12.9)
