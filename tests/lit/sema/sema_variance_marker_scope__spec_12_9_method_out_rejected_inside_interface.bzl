// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_marker_scope, spec_12_9_method_out_rejected_inside_interface).
//
// §12.9: a method's own type parameter is rejected even INSIDE an interface
// — the position rules govern the interface's own parameters, not a
// method's (the interface's own `out T` on IHost is fine; the method-level
// `out U` on MakeHandler is not).

module M; interface IHost<out T> method MakeHandler<out U>(seed: U): T; end IHost;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `U` may not be marked `out` — variance markers are valid only on an interface's own type parameters
// CHECK-SAME: (§12.9)
