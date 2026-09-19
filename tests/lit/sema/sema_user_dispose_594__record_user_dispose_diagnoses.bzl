// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/user_dispose_method_594_test.cpp
//   (BEZEL_TEST sema_user_dispose_594, record_user_dispose_diagnoses).
//
// §28.2 (v0.300): "Declaring a member named `Dispose` on a class or a
// record is a compile error at that declaration, whatever its parameter list."

module M; record Point(x: int, y: int) public method Dispose() _ := 0; end Dispose; end Point;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `Point` may not declare a `Dispose` method directly
// CHECK-SAME: (§28.2)
