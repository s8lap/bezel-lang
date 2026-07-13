// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_position, spec_12_9_in_in_readonly_property_rejected).
//
// §12.9: T in a readonly property is a pure output position — violates `in`.

module M; interface IBadIn2<in T> readonly property Current: T; end IBadIn2;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` is contravariant (`in`) but appears in a readonly property `Current` — readonly properties are output positions
// CHECK-SAME: (§12.9)
