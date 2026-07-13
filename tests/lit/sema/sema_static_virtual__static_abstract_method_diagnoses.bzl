// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_virtual_test.cpp
//   (BEZEL_TEST sema_static_virtual, static_abstract_method_diagnoses).

module M; abstract class C public static abstract method F(): int; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: static member `F` may not be virtual, abstract, or override
// CHECK-SAME: (§12.1)
