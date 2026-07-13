// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/method_body_presence_test.cpp
//   (BEZEL_TEST sema_body, interface_method_with_body_diagnoses).

module M; interface I method F(): int => 1; end I;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: expected ';' after interface method signature
