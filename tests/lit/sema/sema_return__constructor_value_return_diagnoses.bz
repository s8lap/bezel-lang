// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/return_form_test.cpp
//   (BEZEL_TEST sema_return, constructor_value_return_diagnoses).

module M; class C public constructor() return 1; end constructor; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a constructor or destructor may not return a value; use `return;`
// CHECK-SAME: (§21.7)
