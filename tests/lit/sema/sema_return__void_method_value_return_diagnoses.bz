// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/return_form_test.cpp
//   (BEZEL_TEST sema_return, void_method_value_return_diagnoses).

module M; class C public method F() return 1; end F; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: this callable has no return type — `return` may not carry a value
// CHECK-SAME: (§21.7)
