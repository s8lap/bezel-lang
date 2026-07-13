// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_restrictions_test.cpp
//   (BEZEL_TEST sema_enum, instance_field_diagnoses).

module M; enum E A, B; public _x: int := 0; end E;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum `E` may not declare an instance field
// CHECK-SAME: (§10.5)
