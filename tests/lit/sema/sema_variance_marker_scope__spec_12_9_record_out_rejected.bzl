// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_marker_scope, spec_12_9_record_out_rejected).
//
// §12.9: a record's own type parameter may not be marked variant either.

module M; record Pair<out T>(a: T, b: T);

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` may not be marked `out` — variance markers are valid only on an interface's own type parameters
// CHECK-SAME: (§12.9)
