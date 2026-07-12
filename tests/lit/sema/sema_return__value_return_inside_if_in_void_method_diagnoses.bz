// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/return_form_test.cpp
//   (BEZEL_TEST sema_return, value_return_inside_if_in_void_method_diagnoses).

module M; class C public method F(b: bool) if b then return 1; end; end F; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: this callable has no return type — `return` may not carry a value
// CHECK-SAME: (§21.7)
