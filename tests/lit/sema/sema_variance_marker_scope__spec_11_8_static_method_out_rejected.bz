// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_marker_scope, spec_11_8_static_method_out_rejected).
//
// §11.8 worked example: `INumber<T>` declares `static method Zero(): T;` —
// marking T covariant is rejected even though `Zero` alone is a pure output
// position (the §12.9 position check would accept it; §11.8 forbids the
// marker outright once any static member is present).

module M; interface INumber<out T> where T: INumber<T> static method Zero(): T; end INumber;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: interface `INumber` declares a static member, so its type parameter `T` may not be marked `out`
// CHECK-SAME: (§11.8)
