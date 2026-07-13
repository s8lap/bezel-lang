// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_restrictions_test.cpp
//   (BEZEL_TEST sema_enum, constructor_diagnoses).

module M; enum E A, B; public constructor() end constructor; end E;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum `E` may not declare a constructor
// CHECK-SAME: (§10.5)
