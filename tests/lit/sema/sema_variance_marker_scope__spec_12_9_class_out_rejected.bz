// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_marker_scope, spec_12_9_class_out_rejected).
//
// §12.9: `class Box<out T>` — the spec's own counter-example — is rejected.

module M; class Box<out T> private _v: T; public method Get(): T => _v; end Box;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` may not be marked `out` — variance markers are valid only on an interface's own type parameters
// CHECK-SAME: (§12.9)
