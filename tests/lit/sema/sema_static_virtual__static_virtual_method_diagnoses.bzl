// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_virtual_test.cpp
//   (BEZEL_TEST sema_static_virtual, static_virtual_method_diagnoses).

module M; class C public static virtual method F(): int => 1; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: static member `F` may not be virtual, abstract, or override
// CHECK-SAME: (§12.1)
