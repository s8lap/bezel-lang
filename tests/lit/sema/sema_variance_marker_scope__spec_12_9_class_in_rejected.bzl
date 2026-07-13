// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_marker_scope, spec_12_9_class_in_rejected).
//
// §12.9: `in` is rejected on a class parameter exactly like `out`.

module M; class Sink<in T> public method Consume(item: T) end Consume; end Sink;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` may not be marked `in` — variance markers are valid only on an interface's own type parameters
// CHECK-SAME: (§12.9)
