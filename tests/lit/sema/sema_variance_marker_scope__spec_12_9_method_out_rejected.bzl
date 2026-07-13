// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_marker_scope, spec_12_9_method_out_rejected).
//
// §12.9: a method's own type parameter is never variant, even when the
// method sits on an ordinary (non-generic) class.

module M; class Util public static method Identity<out T>(x: T): T => x; end Util;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` may not be marked `out` — variance markers are valid only on an interface's own type parameters
// CHECK-SAME: (§12.9)
