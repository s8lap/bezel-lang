// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/return_form_test.cpp
//   (BEZEL_TEST sema_return, value_method_bare_return_diagnoses).

module M; class C public method F(): int return; end F; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: this callable has a return type — `return;` must provide a value
// CHECK-SAME: (§21.7)
